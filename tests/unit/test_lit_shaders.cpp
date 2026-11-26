// M6.1：LightRig / MaterialShade 进 uniforms 的 plumbing 测试。
// M6.3 起追加 Gouraud（lambertVS + lambertPerVertexFS）的用例；M6.4 的逐像素用例也放这里。
//
// **分阶段完成**（简报 Step 1 的明示）：§8.1 #6 的完整形态是"改 rig 的 keyColor 与
// MaterialShade::specular ⇒ Gouraud 与逐像素的输出同步变化"。M6.1 只钉 plumbing 那半句
// （指针单一真源 + drawModel 逐组填对）；Gouraud 那半句由下方
// lambertVS_writes_lit_color_into_the_existing_color_slot 补全（改 rig/mat ⇒ VS 输出同步变），
// 逐像素那半句仍留给 M6.4。读者不要以为这条已经测全。
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "harness.h"
#include "loader/model.h"
#include "loader/mtl.h"
#include "math/mat.h"
#include "math/transform.h"
#include "render/builtin_shaders.h"
#include "render/color.h"
#include "render/interpolate.h"
#include "render/lights.h"
#include "render/render_backend.h"
#include "render/sampler.h"
#include "render/shading.h"
#include "render/texture.h"
#include "render/triangle_setup.h"
#include "render/viewport.h"
#include "scene/mesh.h"
#include "test_util.h"

namespace {

namespace fs = std::filesystem;

// TempDir / writeText：与 test_model.cpp / test_scene.cpp 同形的文件局部测试工具
// （sr_tmp_bmp 由 tests/main.cpp 统一建好、且 gitignore）。
struct TempDir {
    fs::path p;
    explicit TempDir(const char* name) : p(fs::path("sr_tmp_bmp") / name) {
        std::error_code ec;
        fs::create_directories(p, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(p, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

void writeText(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);   // ofstream(fs::path)：宽字符打开（Ruling 62）
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
}

// 两个三角形、两个材质组（A 在前 B 在后），分居屏幕两角。
// 与 test_model.cpp 的 draw_model_paints_each_group_with_its_own_diffuse 同一几何。
const char* kTwoGroupObj =
    "mtllib m.mtl\n"
    "v 0.1 0.1 0.5\nv 0.9 0.1 0.5\nv 0.5 0.9 0.5\n"
    "v -0.9 -0.9 0.5\nv -0.1 -0.9 0.5\nv -0.5 -0.1 0.5\n"
    "usemtl A\nf 1 2 3\n"
    "usemtl B\nf 4 5 6\n";

// 单位矩阵 ⇒ OBJ 的 v 坐标直接当 clip 空间用（与 test_model 同一手法）。
sr::VertexUniforms identityVu() {
    sr::VertexUniforms vu;
    vu.model = vu.view = vu.proj = vu.normalMatrix = sr::Mat4::identity();
    return vu;
}

sr::FragmentUniforms baseFu() {
    sr::FragmentUniforms fu;
    fu.baseColor = sr::LinearColor{1, 1, 1};
    fu.sampler = sr::SamplerState{sr::WrapMode::ClampEdge, sr::WrapMode::ClampEdge,
                                  sr::FilterMode::Nearest, false};
    return fu;
}

void expectColorNear(const sr::LinearColor& c, float r, float g, float b, float tol) {
    SR_ASSERT_NEAR(c.r, r, tol);
    SR_ASSERT_NEAR(c.g, g, tol);
    SR_ASSERT_NEAR(c.b, b, tol);
}

// 记录后端：drawRange 每个 draw call 一次虚调用（C5 允许），这里不画像素，
// 只把 PipelineState 里 M6.1 新增的指针关系与**当时的值**抄下来。
// 为什么抄值而不是只存指针：drawModel 的 MaterialShade 是栈上存储，drawModel 返回后
// 就失效了 —— 断言阶段解引用悬垂指针是 UB。指针**相等关系**在 drawRange 内判定并记录。
// 解引用一律先判空（纪律 #1：SR_ASSERT 记录后继续，不许 abort 掉整轮变异验证）。
struct RecordedDraw {
    const sr::LightRig* vuLights = nullptr;
    const sr::LightRig* fuLights = nullptr;
    bool matPresent = false;      // vu.mat 与 fu.mat 都非空
    bool matSameObject = false;   // vu.mat == fu.mat（两阶段同一份，D15）
    sr::MaterialShade mat{};      // draw 时刻的值拷贝
};

class RecordingBackend final : public sr::RenderBackend {
  public:
    std::vector<RecordedDraw> draws;
    const char* name() const override { return "recording"; }
    void drawRange(const sr::Mesh&, const sr::PipelineState& st,
                   uint32_t, uint32_t) override {
        RecordedDraw d;
        d.vuLights = st.vu.lights;
        d.fuLights = st.fu.lights;
        d.matPresent = st.vu.mat != nullptr && st.fu.mat != nullptr;
        // P93（M6.3）：两个 nullptr 也"相等"——不加非空前提，这条在"忘了填 mat"时恒绿。
        d.matSameObject = st.vu.mat != nullptr && st.vu.mat == st.fu.mat;
        if (st.fu.mat != nullptr) d.mat = *st.fu.mat;
        draws.push_back(d);
    }
};

constexpr int kW = 64;
constexpr int kH = 64;
constexpr uint32_t kBg = 0xFF000000u;

// ---- M6.3：Gouraud 夹具 ------------------------------------------------------

// 只开方向光的 rig：点光关（intensity=0，D24）、ambient 0、白光 intensity 1。
// towardLight 是"朝光源"的方向；keyDirWorld 是"照向"（lights.h:15），所以取反。
sr::LightRig keyOnlyRig(const sr::Vec3& towardLight) {
    sr::LightRig rig;
    rig.keyDirWorld = -towardLight;
    rig.keyColor = sr::LinearColor{1.0f, 1.0f, 1.0f};
    rig.keyIntensity = 1.0f;
    rig.pointIntensity = 0.0f;
    rig.ambient = sr::LinearColor{0.0f, 0.0f, 0.0f};
    rig.eyeWorld = sr::Vec3{0.0f, 5.0f, 0.0f};
    return rig;
}

// 覆盖屏幕大部的四边形（两个三角形）：左半法线 (0,0,1)，右半朝 +x 倾 60°。
// L=(0,0,1) 时左半 N·L=1、右半 N·L=0.5 ⇒ 亮度梯度**只来自光照**（顶点色全白）。
// uv 铺满 [0,1]，texturedFS 能采到整张贴图。
sr::Mesh gouraudQuad() {
    sr::Mesh m;
    m.positions = {{-0.8f, -0.8f, 0.5f}, {0.8f, -0.8f, 0.5f},
                   {0.8f, 0.8f, 0.5f}, {-0.8f, 0.8f, 0.5f}};
    const float s = std::sqrt(3.0f) / 2.0f;
    m.normals = {{0.0f, 0.0f, 1.0f}, {s, 0.0f, 0.5f}, {s, 0.0f, 0.5f}, {0.0f, 0.0f, 1.0f}};
    m.uvs = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    m.colors = {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}};
    m.indices = {0, 1, 2, 0, 2, 3};
    m.recomputeBounds();
    return m;
}

// ---- M6.4：逐像素夹具 --------------------------------------------------------

// 四顶点法线全 (0,0,1) 的四边形：L=(0,0,1) 时 N·L 处处 = 1（相同法线的插值不变），
// 亮度差**只可能来自贴图** —— 把光照梯度从贴图判别里隔离出去。uv 铺满 [0,1]。
sr::Mesh litFlatQuad() {
    sr::Mesh m;
    m.positions = {{-0.8f, -0.8f, 0.5f}, {0.8f, -0.8f, 0.5f},
                   {0.8f, 0.8f, 0.5f}, {-0.8f, 0.8f, 0.5f}};
    m.normals = {{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f},
                 {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}};
    m.uvs = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    m.colors = {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}};
    m.indices = {0, 1, 2, 0, 2, 3};
    m.recomputeBounds();
    return m;
}

// 缩小（minified）贴图夹具：uv 在 51.2px 的屏幕边上跨 u∈[0,9]、v∈[0,4.5]，
// 且 u/v 的梯度**不对称、带非零 off-diagonal 分量**（仿射解：u = 4.5 + 5X + 0.625Y，
// v = 2.25 + 0.3125X + 2.5Y，X/Y 为世界坐标）：
//   dudx = 5/32 = 0.15625      dudy = -0.625/32 = -0.01953125
//   dvdx = 0.3125/32           dvdy = -2.5/32 = -0.078125
// 配 **64×32 非方形**纹理 ⇒ rhoX = |(10, 0.3125)| ≈ 10.005、rhoY = |(1.25, 2.5)| ≈ 2.795
// ⇒ rho = max ≈ 10.005 ⇒ lod ≈ 3.32（level 3 + frac 0.32 的三线性混合）。
// 不对称 + 非方形是判别力：梯度分量抄错源（gradV.x ← gradU.x）改 rhoX ⇒ 可观测；
// 纯 gradU↔gradV 对调给 rho ≈ 5.04（level 2）⇒ 也可观测 —— 方形纹理下它会因
// max(rhoX,rhoY) 对称而字节等价（第一轮变异验证的 M04b 实测如此，换非方形后杀）。
sr::Mesh phongMipQuad() {
    sr::Mesh m;
    m.positions = {{-0.8f, -0.8f, 0.5f}, {0.8f, -0.8f, 0.5f},
                   {0.8f, 0.8f, 0.5f}, {-0.8f, 0.8f, 0.5f}};
    m.normals = {{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f},
                 {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}};
    m.uvs = {{0.0f, 0.0f}, {8.0f, 0.5f}, {9.0f, 4.5f}, {1.0f, 4.0f}};
    m.colors = {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}};
    m.indices = {0, 1, 2, 0, 2, 3};
    m.recomputeBounds();
    return m;
}

// r/g/b 两两互异的测试图（r = x*4+1, g = y*4+1, b = 128）。灰度图在通道对调变异下
// 恒等 —— Ruling 67 的对称夹具教训：夹具必须在对调操作下给出不同的数。
sr::ImageRGBA8 makeChannelDistinctImage(int w, int h) {
    sr::ImageRGBA8 img;
    img.width = w;
    img.height = h;
    img.rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t at = (static_cast<size_t>(y) * static_cast<size_t>(w) +
                               static_cast<size_t>(x)) * 4u;
            img.rgba[at] = static_cast<uint8_t>(x * 4 + 1);
            img.rgba[at + 1] = static_cast<uint8_t>(y * 4 + 1);
            img.rgba[at + 2] = 128;
            img.rgba[at + 3] = 255;
        }
    }
    return img;
}

// 高光带半高宽：行 y=32 解码到**线性**（sRGB 空间的半高 ≠ 线性半高，encode 曲线非线性），
// 取峰值，数包含峰位的 ≥ peak/2 连续段。peakOut 回传线性峰值。
int highlightWidthAtHalfMax(const Canvas& c, float& peakOut) {
    float vals[kW];
    int peakX = 0;
    float peak = 0.0f;
    for (int x = 0; x < kW; ++x) {
        vals[x] = sr::decodeSRGB(c.rt.color.readPacked(x, 32)).r;
        if (vals[x] > peak) { peak = vals[x]; peakX = x; }
    }
    peakOut = peak;
    if (!(peak > 0.0f)) return 0;
    const float half = peak * 0.5f;
    int x0 = peakX;
    while (x0 > 0 && vals[x0 - 1] >= half) --x0;
    int x1 = peakX;
    while (x1 < kW - 1 && vals[x1 + 1] >= half) ++x1;
    return x1 - x0 + 1;
}

// 渲一帧进**已就地构造**的 Canvas（Canvas 绝不按值返回，test_util.h）。
// 剔除关掉：颜色与绕向无关（test_interpolate.cpp 的同款理由）。
void drawQuad(Canvas& c, const sr::Mesh& mesh, sr::VertexShaderFn vs, sr::FragmentShaderFn fs,
              const sr::VertexUniforms& vu, const sr::FragmentUniforms& fu, sr::FrameStats& s) {
    c.rt.color.clear(kBg);
    c.rt.depth.clear();
    sr::PipelineState st;
    st.target = &c.rt;
    st.vertexShader = vs;
    st.fragmentShader = fs;
    st.vu = vu;
    st.fu = fu;
    st.cull.backfaceCull = false;
    st.stats = &s;
    sr::BackendA backend;
    backend.draw(mesh, st);
}

}  // namespace

// ---- Step 1: 单一真源 + 逐组填充（§8.1 #6 的 plumbing 半句）------------------

SR_TEST(lights_and_material_reach_both_stages_from_one_source) {
    // 半 A：uniforms 层的单一真源 —— 两个阶段的指针指向**同一个**对象，
    // 改一处，两处都看得见（不是两份拷贝各自漂移）。
    sr::LightRig rig;
    rig.keyColor = sr::LinearColor{0.25f, 0.5f, 0.75f};
    sr::MaterialShade ms;
    ms.specular = sr::LinearColor{0.35f, 0.35f, 0.35f};
    sr::VertexUniforms vu;
    sr::FragmentUniforms fu;
    vu.lights = fu.lights = &rig;
    vu.mat = fu.mat = &ms;
    SR_ASSERT(vu.lights == fu.lights);
    SR_ASSERT(vu.mat == fu.mat);
    rig.keyColor.r = 0.9f;
    SR_ASSERT_NEAR(vu.lights->keyColor.r, 0.9f, 1e-6f);
    SR_ASSERT_NEAR(fu.lights->keyColor.r, 0.9f, 1e-6f);
    ms.specular.g = 0.1f;
    SR_ASSERT_NEAR(vu.mat->specular.g, 0.1f, 1e-6f);
    SR_ASSERT_NEAR(fu.mat->specular.g, 0.1f, 1e-6f);

    // 半 B：drawModel 逐组填 MaterialShade，且两阶段指针关系在**每个 draw call** 上成立。
    // 夹具（Ruling 67）：两个材质的四个数**逐项不同** —— 任何一份状态被写成
    // "永远用 materials[0]"或"抄错字段"，交换变异都能红。
    TempDir dir("m61_one_source");
    writeText(dir.p / "a.obj", kTwoGroupObj);
    writeText(dir.p / "m.mtl",
              "newmtl A\nKd 1 0 0\nKa 0.1 0.2 0.3\nKs 0.4 0.5 0.6\nNs 32\n"
              "newmtl B\nKd 0 1 0\nKa 0.3 0.2 0.1\nKs 0.6 0.5 0.4\nNs 128\n");
    sr::LoadedModel m;
    std::string err;
    const bool ok = sr::loadModel(dir.p / "a.obj", m, err);
    SR_ASSERT(ok);
    if (!ok) {
        std::printf("[m61] one-source 夹具加载失败: %s\n", err.c_str());
        return;
    }
    SR_ASSERT_EQ(m.model.groups.size(), 2);
    if (m.model.groups.size() != 2) return;

    Canvas c{kW, kH};
    RecordingBackend rec;
    sr::VertexUniforms vu2 = identityVu();
    vu2.lights = &rig;
    sr::FragmentUniforms base = baseFu();
    base.lights = &rig;
    sr::FrameStats s;
    sr::drawModel(rec, m, c.rt, vu2, base, &s);
    SR_ASSERT_EQ(rec.draws.size(), 2);
    if (rec.draws.size() != 2) return;
    for (size_t i = 0; i < rec.draws.size(); ++i) {
        const RecordedDraw& d = rec.draws[i];
        // 单一真源：两阶段 lights 指针相等，且就是调用方挂进去的那个 rig。
        SR_ASSERT(d.vuLights == &rig);
        SR_ASSERT(d.fuLights == &rig);
        SR_ASSERT(d.matPresent);
        SR_ASSERT(d.matSameObject);
        if (!d.matPresent) continue;
        const int32_t mi = m.model.groups[i].material;
        SR_ASSERT(mi >= 0 && mi < static_cast<int32_t>(m.model.materials.size()));
        if (mi < 0 || mi >= static_cast<int32_t>(m.model.materials.size())) continue;
        // 填充的 oracle 是 loader 的产物（materials[mi]），不是再算一遍 sRGB ——
        // 这一层要证的是"抄对了那一份"，不是解码曲线本身（曲线由 Step 5 那条钉）。
        const sr::Material& mat = m.model.materials[static_cast<size_t>(mi)];
        expectColorNear(d.mat.diffuse, mat.diffuse.r, mat.diffuse.g, mat.diffuse.b, 1e-6f);
        expectColorNear(d.mat.ambient, mat.ambient.r, mat.ambient.g, mat.ambient.b, 1e-6f);
        expectColorNear(d.mat.specular, mat.specular.r, mat.specular.g, mat.specular.b, 1e-6f);
        SR_ASSERT_NEAR(d.mat.shininess, mat.shininess, 1e-6f);
    }
    // 夹具判别力自检：两组的数必须真的不同，否则上面的逐组断言在交换变异下恒绿。
    // P92（M6.3）：四个字段逐项自检 —— 只钉 specular/shininess 时，未来有人把 Kd/Ka
    // 改成两组同值，diffuse↔ambient 对调之类的交换变异会静默变绿。
    const sr::MaterialShade& a = rec.draws[0].mat;
    const sr::MaterialShade& b = rec.draws[1].mat;
    SR_ASSERT(std::fabs(a.diffuse.r - b.diffuse.r) > 0.01f);
    SR_ASSERT(std::fabs(a.ambient.r - b.ambient.r) > 0.01f);
    SR_ASSERT(std::fabs(a.specular.r - b.specular.r) > 0.01f);
    SR_ASSERT(std::fabs(a.shininess - b.shininess) > 1.0f);
}

// ---- Step 1: 程序几何的 lit 默认 vs OBJ 自己的材质（Review Focus 4）-----------

SR_TEST(procedural_geometry_gets_a_lit_default_and_obj_keeps_its_own_material) {
    // 设计 §14 第 1 条的失效模式：Material 默认 Ks=0/Ka=0/Ns=0，程序球若沿用它，
    // lit golden 会烘进一张零高光的球。两条默认路径必须各走各的、互不污染。
    // A：litDefaultMaterialShade 的四个数（D15）+ 地址稳定（函数内 static）。
    const sr::MaterialShade& lit = sr::litDefaultMaterialShade();
    SR_ASSERT(&lit == &sr::litDefaultMaterialShade());
    expectColorNear(lit.diffuse, 1.0f, 1.0f, 1.0f, 1e-6f);
    expectColorNear(lit.specular, 0.35f, 0.35f, 0.35f, 1e-6f);
    expectColorNear(lit.ambient, 0.02f, 0.02f, 0.02f, 1e-6f);
    SR_ASSERT_NEAR(lit.shininess, 64.0f, 1e-6f);
    // 非零高光默认是这条测试存在的理由本身。
    SR_ASSERT(lit.specular.r > 0.0f);
    SR_ASSERT(lit.shininess > 0.0f);

    // B：程序几何挂 lit 默认 —— 这是 M6.8 lit golden 将要用的接线形状。
    //    今天还没有生产程序几何消费点（M6.8 才接），像素级证明是 M6.8 的 #7a；
    //    这里钉的是"默认值能经指针边界到达两个阶段"这一 plumbing 事实。
    sr::LightRig rig;
    rig.keyColor = sr::LinearColor{1.0f, 1.0f, 1.0f};
    const sr::Mesh sphere = sr::makeUVSphere(1.0f, 6, 9);
    sr::PipelineState st;
    st.vu.lights = &rig;
    st.fu.lights = &rig;
    st.vu.mat = &lit;
    st.fu.mat = &lit;
    RecordingBackend rec;
    rec.draw(sphere, st);
    SR_ASSERT_EQ(rec.draws.size(), 1);
    if (rec.draws.empty()) return;
    SR_ASSERT(rec.draws[0].matPresent);
    SR_ASSERT(rec.draws[0].matSameObject);
    SR_ASSERT(rec.draws[0].vuLights == rec.draws[0].fuLights);
    if (rec.draws[0].matPresent) {
        expectColorNear(rec.draws[0].mat.specular, 0.35f, 0.35f, 0.35f, 1e-6f);
        SR_ASSERT_NEAR(rec.draws[0].mat.shininess, 64.0f, 1e-6f);
    }

    // C：OBJ 保持自己的材质 —— 即使调用方在 base 里放了 lit 默认，drawModel 也必须
    //    按组覆写成文件值；文件没给 Ks/Ka/Ns 就保持 0（尊重文件，不许被默认值覆盖）。
    TempDir dir("m61_obj_own_mat");
    writeText(dir.p / "a.obj", kTwoGroupObj);
    writeText(dir.p / "m.mtl",
              "newmtl A\nKd 0.8 0.2 0.2\n"   // 无 Ks/Ka/Ns ⇒ 全 0
              "newmtl B\nKd 0.2 0.8 0.2\nKs 0.9 0.9 0.9\nKa 0.05 0.05 0.05\nNs 200\n");
    sr::LoadedModel m;
    std::string err;
    const bool ok = sr::loadModel(dir.p / "a.obj", m, err);
    SR_ASSERT(ok);
    if (!ok) {
        std::printf("[m61] obj-own-mat 夹具加载失败: %s\n", err.c_str());
        return;
    }
    SR_ASSERT_EQ(m.model.groups.size(), 2);
    if (m.model.groups.size() != 2) return;
    // 夹具 sanity：组序与材质表序一致（A=0，B=1）。
    SR_ASSERT_EQ(m.model.groups[0].material, 0);
    SR_ASSERT_EQ(m.model.groups[1].material, 1);

    Canvas c{kW, kH};
    RecordingBackend rec2;
    sr::VertexUniforms vu2 = identityVu();
    vu2.lights = &rig;
    vu2.mat = &lit;                 // 调用方提供 lit 默认
    sr::FragmentUniforms base = baseFu();
    base.lights = &rig;
    base.mat = &lit;                // drawModel 必须按组覆写，不许透传
    sr::FrameStats s;
    sr::drawModel(rec2, m, c.rt, vu2, base, &s);
    SR_ASSERT_EQ(rec2.draws.size(), 2);
    if (rec2.draws.size() != 2) return;
    SR_ASSERT(rec2.draws[0].matPresent && rec2.draws[1].matPresent);
    if (!rec2.draws[0].matPresent || !rec2.draws[1].matPresent) return;
    // 即使调用方在 vu.mat 里放了 lit 默认，逐组覆写也必须让两阶段指向**同一份**
    // 逐组 shade（而不是 vu 留着调用方的、fu 用新的）。变异⑤首轮在这里漏过，补上。
    SR_ASSERT(rec2.draws[0].matSameObject && rec2.draws[1].matSameObject);
    // 组 A（plain）：高光/环境/指数全 0 —— lit 默认的 0.35/64 不许漏进来。
    expectColorNear(rec2.draws[0].mat.specular, 0.0f, 0.0f, 0.0f, 1e-6f);
    expectColorNear(rec2.draws[0].mat.ambient, 0.0f, 0.0f, 0.0f, 1e-6f);
    SR_ASSERT_NEAR(rec2.draws[0].mat.shininess, 0.0f, 1e-6f);
    // 组 B（shiny）：文件给的值原样到达（Ks 已解码、Ns 原样）。
    const float ks = sr::srgbToLinear(0.9f);
    expectColorNear(rec2.draws[1].mat.specular, ks, ks, ks, 1e-6f);
    const float ka = sr::srgbToLinear(0.05f);
    expectColorNear(rec2.draws[1].mat.ambient, ka, ka, ka, 1e-6f);
    SR_ASSERT_NEAR(rec2.draws[1].mat.shininess, 200.0f, 1e-6f);
    // diffuse 两组都抄 loader 的产物（不是 lit 默认的 1 1 1）。
    if (!m.model.materials.empty()) {
        expectColorNear(rec2.draws[0].mat.diffuse, m.model.materials[0].diffuse.r,
                        m.model.materials[0].diffuse.g, m.model.materials[0].diffuse.b, 1e-6f);
    }
    if (m.model.materials.size() >= 2) {
        expectColorNear(rec2.draws[1].mat.diffuse, m.model.materials[1].diffuse.r,
                        m.model.materials[1].diffuse.g, m.model.materials[1].diffuse.b, 1e-6f);
    }
}

// ---- Step 1: nullptr = unlit（D15 的早退语义，plumbing 半句）------------------

SR_TEST(null_lights_means_unlit_and_writes_no_nan) {
    // **分阶段完成**：lit shader 的 nullptr 早退是 M6.3/M6.4 的实现；这里钉的是
    // ① 两组 uniforms 的默认就是 nullptr（不发明第四种模式、不加 bool）；
    // ② unlit 画面不被新 plumbing 扰动（挂不挂 rig 逐字节相同 —— 两份 golden 的安全性质）；
    // ③ 输出像素合法：NaN 经 encodeSRGB 会变成字节 0（= 背景色），角点断言能抓住它。
    sr::VertexUniforms vu;
    sr::FragmentUniforms fu;
    SR_ASSERT(vu.lights == nullptr);
    SR_ASSERT(vu.mat == nullptr);
    SR_ASSERT(fu.lights == nullptr);
    SR_ASSERT(fu.mat == nullptr);

    TempDir dir("m61_null_unlit");
    writeText(dir.p / "a.obj", kTwoGroupObj);
    writeText(dir.p / "m.mtl", "newmtl A\nKd 0 1 0\nnewmtl B\nKd 1 0 0\n");
    sr::LoadedModel m;
    std::string err;
    const bool ok = sr::loadModel(dir.p / "a.obj", m, err);
    SR_ASSERT(ok);
    if (!ok) {
        std::printf("[m61] null-unlit 夹具加载失败: %s\n", err.c_str());
        return;
    }

    Canvas c1{kW, kH};
    c1.rt.color.clear(kBg);
    sr::BackendA backend;
    sr::FrameStats s1;
    sr::drawModel(backend, m, c1.rt, identityVu(), baseFu(), &s1);   // lights/mat 全 nullptr
    SR_ASSERT_EQ(s1.drawCalls, 2);
    SR_ASSERT_EQ(c1.rt.color.readPacked(48, 20), sr::encodeSRGB(sr::LinearColor{0, 1, 0}));
    SR_ASSERT_EQ(c1.rt.color.readPacked(16, 52), sr::encodeSRGB(sr::LinearColor{1, 0, 0}));

    // 挂一个 rig 重画：当前 FS 不读 lights ⇒ 图像必须逐字节相同（plumbing 惰性证明）。
    // M6.3/M6.4 引入 lit shader 后 drawModel 仍走 texturedFS（unlit）；若哪天改了这条，
    // 本断言会红，届时按新语义重审，而不是悄悄删掉它。
    sr::LightRig rig;
    rig.keyColor = sr::LinearColor{1.0f, 0.5f, 0.25f};
    rig.keyIntensity = 2.0f;
    Canvas c2{kW, kH};
    c2.rt.color.clear(kBg);
    sr::VertexUniforms vu2 = identityVu();
    vu2.lights = &rig;
    sr::FragmentUniforms base2 = baseFu();
    base2.lights = &rig;
    sr::FrameStats s2;
    sr::drawModel(backend, m, c2.rt, vu2, base2, &s2);
    SR_ASSERT_EQ(s2.drawCalls, 2);
    SR_ASSERT(c1.colorMem == c2.colorMem);
}

// ---- Step 5: 光色与材质色走同一条 sRGB 解码（§8.2 #11）------------------------

SR_TEST(light_and_material_colors_are_srgb_decoded_once) {
    // --key-color 0.5 0.5 0.5 ⇒ ≈0.2140：不是 0.5（没解码）、不是 0.25（朴素 gamma²）、
    // 也不是 ≈0.0374（双重解码）。与 Kd 同一个 srgbToLinear(float)（D5 / Review Focus 5）。
    // Ruling 90 ②：buildLightRig 归 M6.6（光 flag 那时才有）；本条测的是解码帮手
    // decodeLightColor（lights.h）—— M6.6 的 rig 组装将调用它。
    const sr::LinearColor light = sr::decodeLightColor(0.5f, 0.5f, 0.5f);
    expectColorNear(light, 0.2140f, 0.2140f, 0.2140f, 1e-4f);
    SR_ASSERT_NEAR(light.r, sr::srgbToLinear(0.5f), 1e-6f);   // 与 Kd 同函数同结果
    // 两端精确（color.h 的同一条要求：1 解成非 1 会让"无 tint"不再成立）。
    expectColorNear(sr::decodeLightColor(1.0f, 1.0f, 1.0f), 1.0f, 1.0f, 1.0f, 1e-6f);
    expectColorNear(sr::decodeLightColor(0.0f, 0.0f, 0.0f), 0.0f, 0.0f, 0.0f, 1e-6f);

    // 材质半句：parseMtl 的 Kd/Ka/Ks 走同一条曲线、各解码一次（M5.1 既有行为，
    // 在 M6 语境下重新钉住 —— 光与材质若走两条曲线，同值输入会给出两种亮度）。
    std::vector<sr::Material> mats;
    std::vector<std::string> warnings;
    sr::MtlStats stats;
    std::string err;
    const bool ok = sr::parseMtl("newmtl m\nKd 0.5 0.5 0.5\nKa 0.5 0.5 0.5\nKs 0.5 0.5 0.5\n",
                                 mats, warnings, stats, err);
    SR_ASSERT(ok);
    if (!ok) {
        std::printf("[m61] srgb 夹具 parseMtl 失败: %s\n", err.c_str());
        return;
    }
    SR_ASSERT_EQ(mats.size(), 1);
    if (mats.empty()) return;
    expectColorNear(mats[0].diffuse, light.r, light.g, light.b, 1e-6f);
    expectColorNear(mats[0].ambient, light.r, light.g, light.b, 1e-6f);
    expectColorNear(mats[0].specular, light.r, light.g, light.b, 1e-6f);
}

// ---- Step 6（Ruling 90 ① 改写）：shininess 原样存 + 穿过逐组填充 --------------

SR_TEST(shininess_is_stored_raw_and_survives_draw_model_fill) {
    // 夹取与 Ns==0 的判断都在 shading.h（M6.2），本层**原样存**（含 0）。
    // shininess_is_clamped_before_the_pow / Ns_zero_disables_specular_and_is_equivalent_to_Ks_zero
    // 断言的是 shadeBlinnPhong 的行为，归 M6.2 独占 —— 本条不碰那个函数。
    sr::MaterialShade ms;
    SR_ASSERT_NEAR(ms.shininess, 0.0f, 1e-6f);   // 默认 0 = "无高光"哨兵，不许被夹成 1

    // 三个组三种原值：0（下界外）、0.5（分数，夹取/取整都会动它）、100000（上界外）。
    // 任何在填入时夹到 [1,4096] 的"好心"实现会让三条全红。
    TempDir dir("m61_ns_raw");
    writeText(dir.p / "a.obj",
              "mtllib m.mtl\n"
              "v 0.1 0.1 0.5\nv 0.9 0.1 0.5\nv 0.5 0.9 0.5\n"
              "v -0.9 -0.9 0.5\nv -0.1 -0.9 0.5\nv -0.5 -0.1 0.5\n"
              "v -0.9 0.1 0.5\nv -0.1 0.1 0.5\nv -0.5 0.9 0.5\n"
              "usemtl zero\nf 1 2 3\n"
              "usemtl frac\nf 4 5 6\n"
              "usemtl huge\nf 7 8 9\n");
    writeText(dir.p / "m.mtl",
              "newmtl zero\nKd 1 1 1\nNs 0\n"
              "newmtl frac\nKd 1 1 1\nNs 0.5\n"
              "newmtl huge\nKd 1 1 1\nNs 100000\n");
    sr::LoadedModel m;
    std::string err;
    const bool ok = sr::loadModel(dir.p / "a.obj", m, err);
    SR_ASSERT(ok);
    if (!ok) {
        std::printf("[m61] ns-raw 夹具加载失败: %s\n", err.c_str());
        return;
    }
    SR_ASSERT_EQ(m.model.groups.size(), 3);
    if (m.model.groups.size() != 3) return;

    Canvas c{kW, kH};
    RecordingBackend rec;
    sr::FrameStats s;
    sr::drawModel(rec, m, c.rt, identityVu(), baseFu(), &s);
    SR_ASSERT_EQ(rec.draws.size(), 3);
    if (rec.draws.size() != 3) return;
    for (size_t i = 0; i < rec.draws.size(); ++i) {
        SR_ASSERT(rec.draws[i].matPresent);
        if (!rec.draws[i].matPresent) continue;
        const int32_t mi = m.model.groups[i].material;
        SR_ASSERT(mi >= 0 && mi < static_cast<int32_t>(m.model.materials.size()));
        if (mi < 0 || mi >= static_cast<int32_t>(m.model.materials.size())) continue;
        // loader 侧原样（parseMtl 既有约定）+ 填充后仍原样（本任务的新约定）。
        SR_ASSERT_NEAR(m.model.materials[static_cast<size_t>(mi)].shininess,
                       rec.draws[i].mat.shininess, 1e-6f);
    }
    SR_ASSERT_NEAR(rec.draws[0].mat.shininess, 0.0f, 1e-6f);
    SR_ASSERT_NEAR(rec.draws[1].mat.shininess, 0.5f, 1e-6f);
    SR_ASSERT_NEAR(rec.draws[2].mat.shininess, 100000.0f, 1e-6f);
}

// ---- M6.3 Step 1: Gouraud —— lambertVS + lambertPerVertexFS ------------------

SR_TEST(lambertVS_writes_lit_color_into_the_existing_color_slot) {
    // D6：光照结果写进**已有**的 v.color 槽（rgb = 线性光色），不新增 varying。
    SR_ASSERT_EQ(sr::kVaryingFloatCount, 12);
    SR_ASSERT_EQ(sizeof(sr::Varyings), 12 * sizeof(float));

    sr::VertexUniforms vu = identityVu();
    sr::LightRig rig = keyOnlyRig({0.0f, 1.0f, 0.0f});   // L = (0,1,0)
    sr::MaterialShade mat;                                // Kd 1 / Ka 0
    mat.specular = sr::LinearColor{0.35f, 0.35f, 0.35f};
    mat.shininess = 64.0f;
    vu.lights = &rig;
    vu.mat = &mat;
    sr::VSIn in;
    in.position = sr::Vec3{0.0f, 0.0f, 0.0f};
    in.normal = sr::Vec3{0.0f, 1.0f, 0.0f};
    in.uv = sr::Vec2{0.25f, 0.75f};
    in.color = sr::Vec4{0.5f, 0.5f, 0.5f, 1.0f};

    const sr::ClipVertex cv = sr::lambertVS(in, vu);
    // N·L=1；P=原点、eye=(0,5,0) ⇒ V=L ⇒ H=N ⇒ N·H=1 ⇒ 高光取峰值 Ks×1。
    // Ruling 91：Gouraud 在顶点算**完整**光照 ⇒ 0.5（漫反射）+ 0.35（高光）= 0.85。
    // 若用 shadeDiffuseOnly 会得 0.5 —— 没有高光带可摊开，§8.2 #7b 退化成"没有 vs 有"。
    // 期望是**线性**值：再解码一次（srgbToLinear(0.85)≈0.694）或编码往返都会红。
    SR_ASSERT_NEAR(cv.v.color.x, 0.85f, 1e-5f);
    SR_ASSERT_NEAR(cv.v.color.y, 0.85f, 1e-5f);
    SR_ASSERT_NEAR(cv.v.color.z, 0.85f, 1e-5f);
    SR_ASSERT_NEAR(cv.v.color.w, 1.0f, 1e-6f);

    // MVP 与 transformVS 共用 clipFromPosition：clip 逐分量一致（不许逐字复制后漂移）。
    const sr::ClipVertex ref = sr::transformVS(in, vu);
    SR_ASSERT_NEAR(cv.clip.x, ref.clip.x, 1e-6f);
    SR_ASSERT_NEAR(cv.clip.y, ref.clip.y, 1e-6f);
    SR_ASSERT_NEAR(cv.clip.z, ref.clip.z, 1e-6f);
    SR_ASSERT_NEAR(cv.clip.w, ref.clip.w, 1e-6f);
    // 其余槽位照常填：M6.4 的逐像素 FS 与 debug 视图要读 worldNormal/worldPos。
    expectVec3Near(cv.v.worldNormal, 0.0f, 1.0f, 0.0f, 1e-6f);
    expectVec3Near(cv.v.worldPos, 0.0f, 0.0f, 0.0f, 1e-6f);
    SR_ASSERT_NEAR(cv.v.uv.x, 0.25f, 1e-6f);
    SR_ASSERT_NEAR(cv.v.uv.y, 0.75f, 1e-6f);

    // 单一真源（D15）：经指针改 rig/mat ⇒ VS 输出同步变化（§8.1 #6 的 Gouraud 半句）。
    rig.keyColor = sr::LinearColor{1.0f, 0.0f, 0.0f};
    const sr::ClipVertex red = sr::lambertVS(in, vu);
    SR_ASSERT_NEAR(red.v.color.x, 0.85f, 1e-5f);
    SR_ASSERT_NEAR(red.v.color.y, 0.0f, 1e-5f);
    SR_ASSERT_NEAR(red.v.color.z, 0.0f, 1e-5f);
    rig.keyColor = sr::LinearColor{1.0f, 1.0f, 1.0f};
    mat.specular = sr::LinearColor{0.7f, 0.7f, 0.7f};
    const sr::ClipVertex shiny = sr::lambertVS(in, vu);
    SR_ASSERT_NEAR(shiny.v.color.x, 1.2f, 1e-5f);   // 0.5 + 0.7：高光确实在 VS 结果里

    // 法线走 normalMatrix 而非 model：scale(2,1,1) 下 n=(1,1,0)/√2 ⇒ N_world=(1,2,0)/√5。
    // 着色用 N_world·L = 2/√5 ≈ 0.8944；拿原始 in.normal 算会得 1/√2 ≈ 0.7071，差 0.187 ⇒ 红。
    sr::VertexUniforms vs = identityVu();
    vs.model = sr::Mat4::scale({2.0f, 1.0f, 1.0f});
    SR_ASSERT(sr::makeNormalMatrix(vs.model, vs.normalMatrix));
    sr::MaterialShade plain;   // Ks 0 / Ns 0：隔离漫反射
    vs.lights = &rig;
    vs.mat = &plain;
    sr::VSIn sn;
    sn.position = sr::Vec3{0.0f, 0.0f, 0.0f};
    sn.normal = sr::normalize(sr::Vec3{1.0f, 1.0f, 0.0f});
    sn.color = sr::Vec4{1.0f, 1.0f, 1.0f, 1.0f};
    const sr::ClipVertex sc = sr::lambertVS(sn, vs);
    SR_ASSERT_NEAR(sc.v.color.x, 0.894427191f, 1e-5f);
    expectVec3Near(sc.v.worldNormal, 0.447213595f, 0.894427191f, 0.0f, 1e-5f);

    // unlit：lights == nullptr ⇒ v.color = in.color（不是写死白，也不是黑）。
    vu.lights = nullptr;
    const sr::ClipVertex unlit = sr::lambertVS(in, vu);
    SR_ASSERT_NEAR(unlit.v.color.x, 0.5f, 1e-6f);
    SR_ASSERT_NEAR(unlit.v.color.y, 0.5f, 1e-6f);
    SR_ASSERT_NEAR(unlit.v.color.z, 0.5f, 1e-6f);
    SR_ASSERT_NEAR(unlit.v.color.w, 1.0f, 1e-6f);
    SR_ASSERT_NEAR(unlit.clip.w, ref.clip.w, 1e-6f);   // 变换不受早退影响
    // mat == nullptr 同样按 unlit 早退：没有材质可读，不发明默认材质（不是第四种模式）。
    vu.lights = &rig;
    vu.mat = nullptr;
    const sr::ClipVertex noMat = sr::lambertVS(in, vu);
    SR_ASSERT_NEAR(noMat.v.color.x, 0.5f, 1e-6f);
}

SR_TEST(gouraud_albedo_is_vertex_color_times_diffuse) {
    // D17：albedo = 顶点色 × Kd（都在线性空间）。这兑现 main.cpp:161 的承诺
    // （"M6 的光照 shader 还要它"）—— 顶点色不再是休眠字段。
    // 夹具按简报：N·L=1、intensity=1、ambient 0、点光关、Ks=0 ⇒ 输出 = albedo 本身。
    sr::VertexUniforms vu = identityVu();
    sr::LightRig rig = keyOnlyRig({0.0f, 1.0f, 0.0f});
    sr::MaterialShade mat;   // Kd 默认 (1,1,1)
    vu.lights = &rig;
    vu.mat = &mat;
    sr::VSIn in;
    in.position = sr::Vec3{0.0f, 0.0f, 0.0f};
    in.normal = sr::Vec3{0.0f, 1.0f, 0.0f};
    in.color = sr::Vec4{0.5f, 0.5f, 0.5f, 1.0f};

    const sr::ClipVertex a = sr::lambertVS(in, vu);
    expectColorNear(sr::LinearColor{a.v.color.x, a.v.color.y, a.v.color.z},
                    0.5f, 0.5f, 0.5f, 1e-5f);
    mat.diffuse = sr::LinearColor{0.5f, 0.5f, 0.5f};
    const sr::ClipVertex b = sr::lambertVS(in, vu);
    expectColorNear(sr::LinearColor{b.v.color.x, b.v.color.y, b.v.color.z},
                    0.25f, 0.25f, 0.25f, 1e-5f);
    // **两次都断言**（简报原话）：只改一个系数无法区分"谁被乘了"。
    // 忘乘 Kd ⇒ 第二次仍是 0.5；忘乘顶点色 ⇒ 第一次 1.0、第二次 0.5。都红。

    // 三通道互异：通道对调 / "只取其中一个因子"的变异必须红（夹具判别力，Ruling 67）。
    mat.diffuse = sr::LinearColor{0.9f, 0.7f, 0.5f};
    in.color = sr::Vec4{0.8f, 0.6f, 0.4f, 1.0f};
    const sr::ClipVertex c = sr::lambertVS(in, vu);
    expectColorNear(sr::LinearColor{c.v.color.x, c.v.color.y, c.v.color.z},
                    0.72f, 0.42f, 0.20f, 1e-5f);
}

SR_TEST(gouraud_ignores_the_texture_and_says_so_in_the_shader_choice) {
    // Review Focus 5 / D17：VS 不采样贴图 ⇒ 切到 Gouraud 时贴图"消失"。这是设计不是 bug；
    // 解释必须在读者会撞见的两处：builtin_shaders.h 的 shader 选择注释 + M6.6 的 HUD 行。
    // 这里钉行为半句：挂任何贴图 / 改 baseColor，Gouraud 输出逐字节相同；
    // 且与贴图通路**真的不同**（贴图确实消失了，不是"两条通路都没采到东西"的对称夹具）。
    const sr::Mesh quad = gouraudQuad();
    sr::LightRig rig = keyOnlyRig({0.0f, 0.0f, 1.0f});   // L = (0,0,1)
    sr::MaterialShade mat;                                // Kd 1 / Ks 0 / Ns 0
    sr::VertexUniforms vu = identityVu();
    vu.lights = &rig;
    vu.mat = &mat;

    const sr::Texture checker =
        sr::makeTexture(sr::makeCheckerImage(8, 8, sr::CheckerPalette{}), true);
    const sr::Texture gray = sr::makeTexture(makeGrayImage(64, 64, grayXPlus1), true);

    sr::FragmentUniforms fuA = baseFu();   // 挂棋盘
    fuA.texture = &checker;
    sr::FragmentUniforms fuB = baseFu();   // 不挂贴图
    sr::FragmentUniforms fuC = baseFu();   // 挂另一张贴图
    fuC.texture = &gray;
    sr::FragmentUniforms fuD = fuA;        // baseColor 改掉：FS 也不许读它
    fuD.baseColor = sr::LinearColor{1.0f, 0.0f, 0.0f};

    Canvas a{kW, kH}, b{kW, kH}, c{kW, kH}, d{kW, kH}, e{kW, kH};
    sr::FrameStats sa, sb, sc, sd, se;
    drawQuad(a, quad, sr::lambertVS, sr::lambertPerVertexFS, vu, fuA, sa);
    drawQuad(b, quad, sr::lambertVS, sr::lambertPerVertexFS, vu, fuB, sb);
    drawQuad(c, quad, sr::lambertVS, sr::lambertPerVertexFS, vu, fuC, sc);
    drawQuad(d, quad, sr::lambertVS, sr::lambertPerVertexFS, vu, fuD, sd);
    // 对照：同一几何同一贴图的 unlit 贴图通路（transformVS + texturedFS）。
    drawQuad(e, quad, sr::transformVS, sr::texturedFS, vu, fuA, se);

    SR_ASSERT(sa.fragmentsWritten > 0);
    SR_ASSERT(se.fragmentsWritten > 0);
    SR_ASSERT(a.colorMem == b.colorMem);
    SR_ASSERT(a.colorMem == c.colorMem);
    SR_ASSERT(a.colorMem == d.colorMem);

    // 判别力 ①：Gouraud 与贴图通路必须真的不同（贴图消失是可见事件）。
    int diffAE = 0;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            if (a.rt.color.readPacked(x, y) != e.rt.color.readPacked(x, y)) ++diffAE;
    SR_ASSERT(diffAE > 100);
    // 判别力 ②：两通路各自都不是平涂 —— Gouraud 有光照梯度，贴图通路有棋盘。
    // （stub VS 直通顶点色时 Gouraud 全白，下面右半像素的闭式期望会红。）
    SR_ASSERT(a.rt.color.readPacked(10, 32) != a.rt.color.readPacked(54, 32));
    SR_ASSERT(e.rt.color.readPacked(10, 32) != e.rt.color.readPacked(54, 32));

    // 闭式期望：四边形左半 N·L=1、右半 0.5，w 全 1 ⇒ 亮度沿 x 仿射。
    // 屏幕 x：左缘 6.4、右缘 57.6（NDC ±0.8 → 64px）。像素中心 10.5 / 54.5：
    //   f(x) = 1 - 0.5*(x-6.4)/51.2 ⇒ f(10.5)=0.9599609, f(54.5)=0.5302734。
    // 容差 0.01：8-bit sRGB 量化在 1.0 附近的线性半步长 ≈ **0.0045**（P99 改正，旧值 0.0031 错）：
    //   d(linear)/d(sRGB) = (2.4/1.055)·((s+0.055)/1.055)^1.4，s=1 时 = 2.4/1.055 ≈ 2.2749；
    //   × 半步 0.5/255 ⇒ 2.2749/510 ≈ 0.00446。分段接缝以下导数 1/12.92 更小，故 0.0045 是全值域上界。
    //   仍远小于两值之差 0.43，容差 0.01 的结论不变 —— 但为容差辩护的数字本身必须对（P99）。
    const sr::LinearColor left = sr::decodeSRGB(a.rt.color.readPacked(10, 32));
    const sr::LinearColor right = sr::decodeSRGB(a.rt.color.readPacked(54, 32));
    expectColorNear(left, 0.9599609f, 0.9599609f, 0.9599609f, 0.01f);
    expectColorNear(right, 0.5302734f, 0.5302734f, 0.5302734f, 0.01f);
}

// ---- M6.3 Step 4: 插值的是**结果**，不是法线 ----------------------------------

SR_TEST(gouraud_interpolates_the_result_not_the_normal) {
    // 三个顶点法线两两相差 90°（简报的"相差 90°"夹具取其三顶点形态）。
    // L=(1,0,0) ⇒ 三顶点亮度 (1,0,0)；重心（w 全相等 ⇒ 算术平均）= 1/3。
    // "插值法线再算光照"给 normalize((1,1,1))·L = 1/√3 ≈ 0.5774 —— 差 0.244，
    // 远大于 1e-5 容差：夹具判别力自检在下面显式断言，对称夹具不许存在。
    sr::VertexUniforms vu = identityVu();
    sr::LightRig rig = keyOnlyRig({1.0f, 0.0f, 0.0f});
    sr::MaterialShade mat;   // Kd 1 / Ks 0 / Ns 0 / Ka 0
    vu.lights = &rig;
    vu.mat = &mat;
    sr::VSIn va, vb, vc;
    va.position = sr::Vec3{-0.6f, -0.6f, 0.5f};  va.normal = sr::Vec3{1, 0, 0};
    vb.position = sr::Vec3{0.6f, -0.6f, 0.5f};   vb.normal = sr::Vec3{0, 1, 0};
    vc.position = sr::Vec3{0.0f, 0.6f, 0.5f};    vc.normal = sr::Vec3{0, 0, 1};
    va.color = vb.color = vc.color = sr::Vec4{1, 1, 1, 1};
    const sr::ClipVertex ca = sr::lambertVS(va, vu);
    const sr::ClipVertex cb = sr::lambertVS(vb, vu);
    const sr::ClipVertex cc = sr::lambertVS(vc, vu);
    expectColorNear(sr::LinearColor{ca.v.color.x, ca.v.color.y, ca.v.color.z},
                    1.0f, 1.0f, 1.0f, 1e-5f);
    expectColorNear(sr::LinearColor{cb.v.color.x, cb.v.color.y, cb.v.color.z},
                    0.0f, 0.0f, 0.0f, 1e-5f);
    expectColorNear(sr::LinearColor{cc.v.color.x, cc.v.color.y, cc.v.color.z},
                    0.0f, 0.0f, 0.0f, 1e-5f);

    // 走生产的 clip→screen→setup→interpolate 通路（不读 8-bit 像素：量化 ~1/255
    // 比 1e-5 容差大两个数量级，浮点插值结果必须在编码前断言）。
    Canvas c{kW, kH};
    const sr::Viewport vp = sr::Viewport::fromColorTarget(c.rt.color);
    sr::ScreenVertex sv[3] = {vp.project(ca), vp.project(cb), vp.project(cc)};
    sr::CullState noCull;
    noCull.backfaceCull = false;
    sr::TriangleSetup setup;
    sr::SetupResult res;
    SR_ASSERT(sr::setupTriangle(sv, c.rt.color, noCull, setup, res));
    const float cx = (sv[0].pos.x + sv[1].pos.x + sv[2].pos.x) / 3.0f;
    const float cy = (sv[0].pos.y + sv[1].pos.y + sv[2].pos.y) / 3.0f;
    const sr::Interpolated ip = sr::interpolateAt(setup, cx, cy);

    // w 全相等 ⇒ 重心 = 算术平均（equal_w_makes_the_centroid_the_arithmetic_mean）。
    const float meanR = (ca.v.color.x + cb.v.color.x + cc.v.color.x) / 3.0f;
    SR_ASSERT_NEAR(ip.v.color.x, meanR, 1e-5f);
    SR_ASSERT_NEAR(ip.v.color.y, meanR, 1e-5f);
    SR_ASSERT_NEAR(ip.v.color.z, meanR, 1e-5f);

    sr::FragmentInput fi;
    fi.screen = sr::Vec2{cx, cy};
    fi.depth = ip.depth;
    fi.v = ip.v;
    sr::FragmentUniforms fu = baseFu();
    fu.lights = &rig;   // 给"FS 重算光照"的变异留好可编译的上下文（它必须红）
    fu.mat = &mat;
    sr::FragmentOutput fo;
    SR_ASSERT(sr::lambertPerVertexFS(fi, fu, fo));
    expectColorNear(fo.color, 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f, 1e-5f);

    // 反例："插值法线（再归一化）后算光照"。插值把法线长度缩到 ~0.577，
    // 归一化后 N·L = 1/√3 —— 与 Gouraud 的 1/3 差 0.244。
    const sr::Vec3 nInterp = sr::normalize(ip.v.worldNormal);
    const sr::LinearColor wrong =
        sr::shadeBlinnPhong(nInterp, ip.v.worldPos, rig, mat, sr::LinearColor{1, 1, 1});
    SR_ASSERT(std::fabs(fo.color.r - wrong.r) > 0.1f);
    // 夹具判别力自检：两个答案必须差得远大于容差，否则本测试对称、毫无判别力。
    SR_ASSERT(std::fabs(meanR - wrong.r) > 0.1f);
    SR_ASSERT(std::fabs(wrong.r - 0.577350269f) < 1e-4f);   // 反例本身的闭式值
}

// ---- M6.4 Step 1: 逐像素 Blinn-Phong ------------------------------------------

SR_TEST(textured_model_keeps_the_texture_under_phong) {
    // §8.2 #12 / D16：逐像素模式**仍采样贴图**，输出必须随纹素变化。
    // 夹具：四顶点法线全 (0,0,1)、L=(0,0,1) ⇒ N·L 处处 1，亮度差只来自贴图。
    // Kd = 0.5（≠1，D26）：双重乘算给 0.25 而不是 0.5，差一倍，不可能 missed；
    // Kd==1 的夹具对"谁乘了 Kd"是空断言（M6.3 评审的原话）。
    // r/g/b 互异（makeChannelDistinctImage）：通道对调变异必须红（Ruling 67）。
    const sr::Mesh quad = litFlatQuad();
    sr::LightRig rig = keyOnlyRig({0.0f, 0.0f, 1.0f});
    sr::MaterialShade mat;                                // Ka 0 / Ks 0 / Ns 0
    mat.diffuse = sr::LinearColor{0.5f, 0.5f, 0.5f};
    sr::VertexUniforms vu = identityVu();
    vu.lights = &rig;
    vu.mat = &mat;
    const sr::Texture tex = sr::makeTexture(makeChannelDistinctImage(64, 64), true);
    sr::FragmentUniforms fu = baseFu();                   // ClampEdge + Nearest + 无 mip
    fu.texture = &tex;
    fu.lights = &rig;
    fu.mat = &mat;
    // baseColor ≠ 白：lit 通路不许读它（albedo = texel / 白，tint 是 unlit 的旋钮，D27 语境）。
    fu.baseColor = sr::LinearColor{1.0f, 0.5f, 0.25f};

    Canvas a{kW, kH};
    sr::FrameStats sa;
    drawQuad(a, quad, sr::transformVS, sr::blinnPhongPerPixelFS, vu, fu, sa);
    SR_ASSERT(sa.fragmentsWritten > 1000);

    // 纹素定位（闭式推导）：屏幕 x∈[6.4,57.6] ↔ u∈[0,1]、y∈[6.4,57.6] ↔ v∈[1,0]（v 轴朝上）。
    // 像素中心 (10.5,10.5)：u = 4.1/51.2 = 0.080078125 ⇒ 列 floor(5.125) = 5；
    //                        v = 47.1/51.2 = 0.919921875 ⇒ 行 floor(58.875) = 58。
    // (20.5,10.5)：u = 14.1/51.2 = 0.275390625 ⇒ 列 floor(17.625) = 17；行同 58。
    // 图公式 r = x*4+1、g = y*4+1、b = 128 ⇒ A=(21,233,128)、B=(69,233,128)。
    // 期望 = srgb8ToLinear(byte) × Kd × N·L(=1)。srgb8ToLinear 是 256 项 LUT（color.h），
    // 两工具链逐位一致；解码曲线本身由 test_color 钉，这里钉的是**接线**：
    // 采到对的纹素、Kd 恰好乘一次、通道没对调。
    const sr::LinearColor expA{sr::srgb8ToLinear(21) * 0.5f, sr::srgb8ToLinear(233) * 0.5f,
                               sr::srgb8ToLinear(128) * 0.5f};
    const sr::LinearColor expB{sr::srgb8ToLinear(69) * 0.5f, sr::srgb8ToLinear(233) * 0.5f,
                               sr::srgb8ToLinear(128) * 0.5f};
    const sr::LinearColor gotA = sr::decodeSRGB(a.rt.color.readPacked(10, 10));
    const sr::LinearColor gotB = sr::decodeSRGB(a.rt.color.readPacked(20, 10));
    // 容差 0.01 的算术：8-bit 量化的线性半步长 = (dlinear/ds)/255/2，
    // dlinear/ds = (2.4/1.055)·((s+0.055)/1.055)^1.4 ≤ 2.4/1.055 ≈ 2.275（s=1 处最大）
    // ⇒ 半步长 ≤ 2.275/510 ≈ 0.00446 < 0.01（全值域一致成立，不挑采样点）。
    // 夹具判别力：两像素 r 通道差 = (srgb8ToLinear(69)−srgb8ToLinear(21))×0.5 ≈ 0.026 ≫ 0.01。
    expectColorNear(gotA, expA.r, expA.g, expA.b, 0.01f);
    expectColorNear(gotB, expB.r, expB.g, expB.b, 0.01f);
    // **变化存在**的证明（简报：不许只断言单值）：两个采到不同纹素的像素必须不同。
    SR_ASSERT(a.rt.color.readPacked(10, 10) != a.rt.color.readPacked(20, 10));
    SR_ASSERT(std::fabs(gotA.r - gotB.r) > 0.02f);
    SR_ASSERT(std::fabs(expA.r - expB.r) > 0.02f);   // 夹具自检：期望值本身差得开

    // 无贴图对照：albedo = 白 ⇒ 输出 = Kd = 0.5 灰（不是 baseColor 的 tint）。
    // 与贴图帧同一像素必须不同 ⇒ 贴图真的参与了（杀"albedo 恒白"变异）。
    sr::FragmentUniforms fuPlain = fu;
    fuPlain.texture = nullptr;
    Canvas b{kW, kH};
    sr::FrameStats sb;
    drawQuad(b, quad, sr::transformVS, sr::blinnPhongPerPixelFS, vu, fuPlain, sb);
    const sr::LinearColor plain = sr::decodeSRGB(b.rt.color.readPacked(10, 10));
    expectColorNear(plain, 0.5f, 0.5f, 0.5f, 0.01f);   // 半步长上界 0.00446 < 0.01
    SR_ASSERT(b.rt.color.readPacked(10, 10) != a.rt.color.readPacked(10, 10));
}

SR_TEST(phong_highlight_is_tighter_than_gouraud_on_the_same_rig) {
    // 为 M6.8 #7b 铺路：同一 rig/材质/几何，两模式各渲一次，比高光带半高宽。
    // UV 球的极点在 ±y（mesh.cpp），而深度测试留下 z 更小的半球 ⇒ 眼与 key 光都放 −z：
    // −z 赤道点 (0,0,−r) 在 stacks=6 / slices=8 时恰是网格顶点（phi=90°、theta=270°），
    // 该顶点 N = V = L = (0,0,−1) ⇒ N·H = 1 ⇒ 两模式峰值都 = Ks = 0.35（D19：不饱和）。
    // Kd=0 / Ka=0 / ambient=0 ⇒ 输出 = 纯高光，半高宽不受漫反射底噪污染。
    const sr::Mesh sphere = sr::makeUVSphere(0.8f, 6, 8);
    sr::LightRig rig;
    rig.keyDirWorld = sr::Vec3{0.0f, 0.0f, 1.0f};   // 照向 +z ⇒ 朝光源 = (0,0,−1)
    rig.keyColor = sr::LinearColor{1.0f, 1.0f, 1.0f};
    rig.keyIntensity = 1.0f;
    rig.pointIntensity = 0.0f;
    rig.ambient = sr::LinearColor{0.0f, 0.0f, 0.0f};
    rig.eyeWorld = sr::Vec3{0.0f, 0.0f, -5.0f};
    sr::MaterialShade mat;
    mat.diffuse = sr::LinearColor{0.0f, 0.0f, 0.0f};
    mat.specular = sr::LinearColor{0.35f, 0.35f, 0.35f};
    mat.shininess = 64.0f;
    sr::VertexUniforms vu = identityVu();
    vu.lights = &rig;
    vu.mat = &mat;
    sr::FragmentUniforms fu = baseFu();
    fu.lights = &rig;
    fu.mat = &mat;

    Canvas p{kW, kH}, g{kW, kH};
    sr::FrameStats sp, sg;
    drawQuad(p, sphere, sr::transformVS, sr::blinnPhongPerPixelFS, vu, fu, sp);
    drawQuad(g, sphere, sr::lambertVS, sr::lambertPerVertexFS, vu, fu, sg);
    SR_ASSERT(sp.fragmentsWritten > 500);
    SR_ASSERT(sg.fragmentsWritten > 500);

    float peakP = 0.0f, peakG = 0.0f;
    const int wP = highlightWidthAtHalfMax(p, peakP);
    const int wG = highlightWidthAtHalfMax(g, peakG);
    // 峰值容差 0.03 的两个误差源（算术）：① 量化半步长 ≤ 0.00446（上一条的推导）；
    // ② Gouraud 的峰在**顶点** (32,32)，像素中心 (32.5,32.5) 偏移 ~0.7px，向 12.8px 外
    //    的近零邻点线性插值 ⇒ 峰值下拉 ~0.35×0.7/12.8 ≈ 0.019。合计 < 0.03 ≪ 0.35。
    SR_ASSERT_NEAR(peakP, 0.35f, 0.03f);
    SR_ASSERT_NEAR(peakG, 0.35f, 0.03f);
    // 半高宽：逐像素的 N·H^64 ≥ 0.5 带 ≈ ±8.4° ⇒ ~7-8px；Gouraud 把顶点峰值线性摊到
    // 30° 外的近零邻点 ⇒ ~13-18px。margin 2px > 量化引起的阈值抖动（±1px）。
    SR_ASSERT(wP >= 3);
    SR_ASSERT(wG >= wP + 2);   // 逐像素更紧 —— 本测试的存在理由
    SR_ASSERT(wG <= 40);       // 不是平涂（pow(x,0)==1 的白饼变异给宽 64）
    int diff = 0;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            if (p.rt.color.readPacked(x, y) != g.rt.color.readPacked(x, y)) ++diff;
    SR_ASSERT(diff > 50);      // 两模式真的不同（不是"都没高光"的对称夹具）
}

SR_TEST(phong_reads_the_interpolated_normal_and_position_not_the_vertex_ones) {
    // ---- 子夹具 A：点光 + 相同法线 ⇒ 亮度差只可能来自**插值后的 P** ----
    // 大三角形（~1380px），三顶点法线全 (0,0,1)，点光 (−1.2,−0.6,0.9)：
    // 距 A(−0.9,−0.6,0.5) 为 0.5 ⇒ N·L = 0.4/0.5 = 0.8；距 B(0.9,−0.6,0.5) 为 √4.57
    // ⇒ N·L ≈ 0.187112。逐像素在 AB 中点 P=(0,−0.6,0.5)：N·L = 0.4/√1.6 ≈ 0.316228
    // ⇒ ×Kd0.5 = 0.158114；逐顶点（Gouraud）插值两顶点结果 ⇒ (0.8+0.187112)/2×0.5
    // = 0.246778。差 0.0887，对 1e-5 容差是四个数量级 —— 不是 1e-6 的装饰夹具。
    sr::Mesh tri;
    tri.positions = {{-0.9f, -0.6f, 0.5f}, {0.9f, -0.6f, 0.5f}, {0.0f, 0.9f, 0.5f}};
    tri.normals = {{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}};
    tri.uvs = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 1.0f}};
    tri.colors = {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}};
    tri.indices = {0, 1, 2};
    tri.recomputeBounds();

    sr::LightRig rig;                    // 只开点光（key 关、ambient 0、无衰减）
    rig.keyIntensity = 0.0f;
    rig.pointPosWorld = sr::Vec3{-1.2f, -0.6f, 0.9f};
    rig.pointColor = sr::LinearColor{1.0f, 1.0f, 1.0f};
    rig.pointIntensity = 1.0f;
    rig.attenuate = false;
    rig.ambient = sr::LinearColor{0.0f, 0.0f, 0.0f};
    rig.eyeWorld = sr::Vec3{0.0f, 0.0f, 5.0f};   // Ks=0 ⇒ V 无关
    sr::MaterialShade mat;
    mat.diffuse = sr::LinearColor{0.5f, 0.5f, 0.5f};   // Kd ≠ 1（D26）
    sr::VertexUniforms vu = identityVu();
    vu.lights = &rig;
    vu.mat = &mat;
    sr::FragmentUniforms fu = baseFu();
    fu.lights = &rig;
    fu.mat = &mat;

    // 整帧证据：左半与右半亮度必须不同（"逐顶点"实现给不出这种点光梯度）。
    Canvas frame{kW, kH};
    sr::FrameStats sf;
    drawQuad(frame, tri, sr::transformVS, sr::blinnPhongPerPixelFS, vu, fu, sf);
    SR_ASSERT(sf.fragmentsWritten > 300);
    const sr::LinearColor left = sr::decodeSRGB(frame.rt.color.readPacked(5, 50));
    const sr::LinearColor right = sr::decodeSRGB(frame.rt.color.readPacked(58, 50));
    SR_ASSERT(frame.rt.color.readPacked(5, 50) != frame.rt.color.readPacked(58, 50));
    SR_ASSERT(left.r - right.r > 0.15f);   // 闭式 ≈0.366 vs ≈0.097，差 ≈0.27 ≫ 量化 0.009

    // 解析证据：走生产 viewport→setup→interpolateAt 通路（w 全 1 ⇒ 插值是仿射的，
    // 屏幕中点 = 世界中点），在编码前断言浮点结果（8-bit 量化 ~1/255 比 1e-5 大两个量级）。
    sr::VSIn va, vb, vc;
    va.position = tri.positions[0];  va.normal = tri.normals[0];  va.uv = tri.uvs[0];
    vb.position = tri.positions[1];  vb.normal = tri.normals[1];  vb.uv = tri.uvs[1];
    vc.position = tri.positions[2];  vc.normal = tri.normals[2];  vc.uv = tri.uvs[2];
    const sr::ClipVertex ca = sr::transformVS(va, vu);
    const sr::ClipVertex cb = sr::transformVS(vb, vu);
    const sr::ClipVertex cc = sr::transformVS(vc, vu);
    Canvas c{kW, kH};
    const sr::Viewport vp = sr::Viewport::fromColorTarget(c.rt.color);
    sr::ScreenVertex sv[3] = {vp.project(ca), vp.project(cb), vp.project(cc)};
    sr::CullState noCull;
    noCull.backfaceCull = false;
    sr::TriangleSetup setup;
    sr::SetupResult res;
    SR_ASSERT(sr::setupTriangle(sv, c.rt.color, noCull, setup, res));
    const float mx = (sv[0].pos.x + sv[1].pos.x) / 2.0f;
    const float my = (sv[0].pos.y + sv[1].pos.y) / 2.0f;
    const sr::Interpolated ip = sr::interpolateAt(setup, mx, my);
    expectVec3Near(ip.v.worldPos, 0.0f, -0.6f, 0.5f, 1e-5f);   // P 是插值出来的中点
    expectVec3Near(ip.v.worldNormal, 0.0f, 0.0f, 1.0f, 1e-6f);

    sr::FragmentInput fi;
    fi.screen = sr::Vec2{mx, my};
    fi.depth = ip.depth;
    fi.v = ip.v;
    fi.gradU = sr::Vec2{setup.dudx, setup.dudy};
    fi.gradV = sr::Vec2{setup.dvdx, setup.dvdy};
    sr::FragmentOutput fo;
    SR_ASSERT(sr::blinnPhongPerPixelFS(fi, fu, fo));
    // 0.5 × 0.4/√1.6 = 0.158113883。只有 +×÷√（IEEE 正确舍入），无 pow ⇒
    // 两工具链差 ~1e-7；容差 1e-5 是其百倍，又比 Gouraud 差值 0.0887 小四个量级。
    expectColorNear(fo.color, 0.158113883f, 0.158113883f, 0.158113883f, 1e-5f);

    // 反例：**逐顶点**答案（生产 lambertVS 的顶点结果取平均 —— w 全 1 ⇒ 中点 = 算术平均）。
    const sr::ClipVertex la = sr::lambertVS(va, vu);
    const sr::ClipVertex lb = sr::lambertVS(vb, vu);
    SR_ASSERT_NEAR(la.v.color.x, 0.4f, 1e-5f);          // 0.5 × 0.8
    SR_ASSERT_NEAR(lb.v.color.x, 0.0935556f, 1e-5f);   // 0.5 × 0.4/√4.57 = 0.0935556
    const float gouraudMid = (la.v.color.x + lb.v.color.x) * 0.5f;
    SR_ASSERT_NEAR(gouraudMid, 0.2467778f, 1e-5f);
    // 夹具判别力自检：逐像素与逐顶点/两顶点答案都必须差得远大于容差。
    SR_ASSERT(std::fabs(fo.color.r - gouraudMid) > 0.05f);    // 实际差 0.0887
    SR_ASSERT(std::fabs(fo.color.r - la.v.color.x) > 0.1f);   // 顶点 A 答案 0.4
    SR_ASSERT(std::fabs(fo.color.r - lb.v.color.x) > 0.05f);  // 顶点 B 答案 0.0936

    // ---- 子夹具 B：插值把法线缩短 ⇒ FS 必须再归一化 ----
    // 三顶点法线两两 90°（M6.3 同形），L=(1,0,0)，Kd=0.5。重心插值 N=(1/3,1/3,1/3)，
    // 长度 1/√3 ≈ 0.57735 —— 先断言**长度本身**：归一化被丢掉时这里就是证据。
    // 归一化后 N·L = 1/√3 ⇒ 输出 0.288675；不归一化 N·L = 1/3 ⇒ 0.166667，差 0.122。
    // 点积对 N 线性 ⇒ "不归一化的逐像素"在方向光下恰 ≡ Gouraud（顶点均值 1/3）：
    // 这条断言同时杀"忘 normalize"与"退化成逐顶点"两个变异。
    sr::LightRig rig2 = keyOnlyRig({1.0f, 0.0f, 0.0f});
    sr::VertexUniforms vu2 = identityVu();
    vu2.lights = &rig2;
    vu2.mat = &mat;
    sr::VSIn na, nb, nc;
    na.position = va.position;  na.normal = sr::Vec3{1, 0, 0};  na.uv = va.uv;
    nb.position = vb.position;  nb.normal = sr::Vec3{0, 1, 0};  nb.uv = vb.uv;
    nc.position = vc.position;  nc.normal = sr::Vec3{0, 0, 1};  nc.uv = vc.uv;
    const sr::ClipVertex ca2 = sr::transformVS(na, vu2);
    const sr::ClipVertex cb2 = sr::transformVS(nb, vu2);
    const sr::ClipVertex cc2 = sr::transformVS(nc, vu2);
    sr::ScreenVertex sv2[3] = {vp.project(ca2), vp.project(cb2), vp.project(cc2)};
    sr::TriangleSetup setup2;
    sr::SetupResult res2;
    SR_ASSERT(sr::setupTriangle(sv2, c.rt.color, noCull, setup2, res2));
    const float cx = (sv2[0].pos.x + sv2[1].pos.x + sv2[2].pos.x) / 3.0f;
    const float cy = (sv2[0].pos.y + sv2[1].pos.y + sv2[2].pos.y) / 3.0f;
    const sr::Interpolated ip2 = sr::interpolateAt(setup2, cx, cy);
    const float nLen = sr::length(ip2.v.worldNormal);
    SR_ASSERT_NEAR(nLen, 0.577350269f, 1e-5f);   // 插值真的把长度缩到 1/√3
    SR_ASSERT(nLen < 0.9f);                      // 夹具判别力：不是单位长度
    sr::FragmentInput fi2;
    fi2.screen = sr::Vec2{cx, cy};
    fi2.depth = ip2.depth;
    fi2.v = ip2.v;
    fi2.gradU = sr::Vec2{setup2.dudx, setup2.dudy};
    fi2.gradV = sr::Vec2{setup2.dvdx, setup2.dvdy};
    sr::FragmentUniforms fu2 = baseFu();
    fu2.lights = &rig2;
    fu2.mat = &mat;
    sr::FragmentOutput fo2;
    SR_ASSERT(sr::blinnPhongPerPixelFS(fi2, fu2, fo2));
    expectColorNear(fo2.color, 0.288675135f, 0.288675135f, 0.288675135f, 1e-5f);
    SR_ASSERT(std::fabs(fo2.color.r - 1.0f / 6.0f) > 0.1f);   // 实际差 0.122
    SR_ASSERT(std::fabs(0.288675135f - 1.0f / 6.0f) > 0.1f);  // 两个闭式答案差得开
}

SR_TEST(phong_with_null_lights_falls_back_to_texturedFS_output) {
    // D27：lights == nullptr ⇒ 与 texturedFS **逐字节相同**。比较对象是 texturedFS 的
    // **实际输出**（同一输入两条通路各渲一帧），不是手算值 —— 等价钉在两条代码路径之间。
    // 无光路径用 fu.baseColor（M4 的 tint 旋钮），不是 mat->diffuse（光照材质）：
    // 夹具刻意 mat.diffuse ≠ baseColor（且 mat.ambient 是第三个值），"unlit 读材质"变异当场红。
    // 贴图是**缩小**表面（phongMipQuad + 64×32 纹理：rho≈10 ⇒ level 3 三线性）：
    // 丢梯度会退到 level 0，而 level 0 输出在本夹具上与 mip 输出必须不同（下方判别力自检）。
    // 注意分两条等式钉（第一轮变异验证抓到的缺口）：lights=nullptr 的等式走 unlit 委托，
    // 不经过 lit 分支的 sample() ⇒ 梯度使用还必须由 **lit 分支**的缩小等式钉（判别力③）。
    const sr::Mesh quad = phongMipQuad();
    const sr::Texture tex = sr::makeTexture(makeGrayImage(64, 32, grayXPlus1), true);
    sr::MaterialShade mat;
    mat.diffuse = sr::LinearColor{0.25f, 0.5f, 0.75f};   // ≠ baseColor
    mat.ambient = sr::LinearColor{0.75f, 0.25f, 0.5f};   // ≠ diffuse ≠ baseColor（Ruling 67 第二值）
    sr::FragmentUniforms fu = baseFu();
    fu.texture = &tex;
    fu.sampler = sr::SamplerState{sr::WrapMode::Repeat, sr::WrapMode::Repeat,
                                  sr::FilterMode::Trilinear, true};
    fu.baseColor = sr::LinearColor{1.0f, 0.5f, 0.25f};
    fu.mat = &mat;                     // lights 保持 nullptr：mat 在场也不许读
    const sr::VertexUniforms vu = identityVu();

    Canvas p{kW, kH}, t{kW, kH}, n{kW, kH};
    sr::FrameStats sp, st, sn;
    drawQuad(p, quad, sr::transformVS, sr::blinnPhongPerPixelFS, vu, fu, sp);
    drawQuad(t, quad, sr::transformVS, sr::texturedFS, vu, fu, st);
    SR_ASSERT(sp.fragmentsWritten > 1000);
    SR_ASSERT(st.fragmentsWritten > 1000);
    SR_ASSERT(p.colorMem == t.colorMem);   // D27 的等式：两条代码路径的实际输出

    // 判别力①：本夹具真的在缩小 —— useMip=false（= 梯度自由变体的输出，SamplerState
    // 注释：Trilinear+无 mip 实测 Lod{0,0}）必须与 mip 帧不同，否则上面的相等是空断言。
    sr::FragmentUniforms fuNoMip = fu;
    fuNoMip.sampler.useMip = false;
    drawQuad(n, quad, sr::transformVS, sr::texturedFS, vu, fuNoMip, sn);
    int diffTN = 0;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            if (t.rt.color.readPacked(x, y) != n.rt.color.readPacked(x, y)) ++diffTN;
    SR_ASSERT(diffTN > 100);

    // 判别力②：lights 非空但 mat == nullptr ⇒ 同样早退（M6.3 语义：不发明默认材质）。
    sr::LightRig rig = keyOnlyRig({0.0f, 0.0f, 1.0f});
    sr::FragmentUniforms fuNoMat = fu;
    fuNoMat.lights = &rig;
    fuNoMat.mat = nullptr;
    Canvas p2{kW, kH}, t2{kW, kH};
    sr::FrameStats sp2, st2;
    drawQuad(p2, quad, sr::transformVS, sr::blinnPhongPerPixelFS, vu, fuNoMat, sp2);
    drawQuad(t2, quad, sr::transformVS, sr::texturedFS, vu, fuNoMat, st2);
    SR_ASSERT(p2.colorMem == t2.colorMem);

    // 判别力③：**lit 分支**的梯度使用。上面的等式 lights=nullptr ⇒ 走 unlit 委托，
    // lit 分支的 sample() 一行都没跑 —— 丢梯度变异在那里不可能红（第一轮 M03 实测存活）。
    // 这里 Kd=1、N·L=1（quad 法线全 (0,0,1)，L=(0,0,1)）、Ks=0、Ka=0、ambient=0、
    // baseColor=白 ⇒ lit 输出 = 纹素本身 = texturedFS 输出，逐字节相等。
    // 逐字节（而非 ±1）在这里是良定的：四顶点法线相同 ⇒ 插值平面是常量 ⇒ N=(0,0,1) 精确、
    // ndl=1 精确，×1.0 与 +0.0 都是精确运算 ⇒ 两条通路对同一 TexelF 位模式给出相同输出
    // （MSVC 与 g++ 两侧实测均逐字节相等）。
    // 零梯度/错梯度会改 mip 级（判别力①已证 mip ≠ level 0）⇒ 等式红。
    sr::LightRig rigFlat = keyOnlyRig({0.0f, 0.0f, 1.0f});
    sr::MaterialShade kd1;               // 默认 Kd 1 / Ka 0 / Ks 0 / Ns 0
    sr::FragmentUniforms fuLit = fu;
    fuLit.lights = &rigFlat;
    fuLit.mat = &kd1;
    fuLit.baseColor = sr::LinearColor{1.0f, 1.0f, 1.0f};
    Canvas p4{kW, kH}, t4{kW, kH};
    sr::FrameStats sp4, st4;
    drawQuad(p4, quad, sr::transformVS, sr::blinnPhongPerPixelFS, vu, fuLit, sp4);
    drawQuad(t4, quad, sr::transformVS, sr::texturedFS, vu, fuLit, st4);
    SR_ASSERT(sp4.fragmentsWritten > 1000);
    SR_ASSERT(p4.colorMem == t4.colorMem);

    // 无贴图：两条通路都退回 baseColor（不是白、不是黑）。
    sr::FragmentUniforms fuPlain = fu;
    fuPlain.texture = nullptr;
    Canvas p3{kW, kH}, t3{kW, kH};
    sr::FrameStats sp3, st3;
    drawQuad(p3, quad, sr::transformVS, sr::blinnPhongPerPixelFS, vu, fuPlain, sp3);
    drawQuad(t3, quad, sr::transformVS, sr::texturedFS, vu, fuPlain, st3);
    SR_ASSERT(p3.colorMem == t3.colorMem);
    const sr::LinearColor tint = sr::decodeSRGB(p3.rt.color.readPacked(32, 32));
    expectColorNear(tint, 1.0f, 0.5f, 0.25f, 0.01f);   // 量化半步长上界 0.00446 < 0.01
}
