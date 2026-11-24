// M6.5：debugNormalFS + planView（D23 的两种机制分界）+ 法线视图的接线级证明。
//
// 分界（D23，写在 sink.h 与 app/view_mode.h）：能逐像素判定的视图走普通 FS
// （法线读的是本像素插值出来的 worldNormal，只有 FS 拿得到）；需要整张缓冲的
// 视图走帧末 sink（深度读整张深度缓冲、灰度读已写好的颜色）。两套机制并存是设计。
//
// 已知 wart（简报 Step 1 要求写明而不是让人猜）：debugNormalFS 输出**线性**
// (N+1)*0.5，而颜色目标出口恒定做 encodeSRGB（render_backend.cpp 的 writePacked
// 那一行）。encodeAnalytic 是 IEC 分段曲线（color.cpp，接缝 0.0031308，幂段
// 1.055·v^(1/2.4) − 0.055），所以线性 0.5 编码成 ≈0.735357 → 字节 **188**：
//   1.055 × 0.5^(1/2.4) − 0.055 = 1.055 × 0.749154 − 0.055 = 0.735357
//   0.735357 × 255 = 187.516 ⇒ lround = 188。
// 不是 128（那是"没编码"），不是 186（gamma-2.2 纯幂 0.5^(1/2.2)×255 ≈ 186.08），
// 也不是 132（计划第一版把解码方向当成了编码方向）。这是**输出侧**单一编码点
// 的结果，不是 bug；Texture::srgb = false 管的是**输入侧**要不要解码（采样时
// 纹素解不解码），两件事别混（M4 notes §3 的区分）。
// 测量纪律：188 与下文所有字节常数都先跑一次打印实际值确认后才写死
// （测量输出记录在 task-M6.5-report.md §3）。
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "harness.h"
#include "app/view_mode.h"
#include "loader/model.h"
#include "math/mat.h"
#include "math/transform.h"
#include "render/builtin_shaders.h"
#include "render/color.h"
#include "render/lights.h"
#include "render/render_backend.h"
#include "render/sink.h"
#include "scene/mesh.h"
#include "test_util.h"

namespace {

namespace fs = std::filesystem;

constexpr int kW = 64;
constexpr int kH = 64;
constexpr uint32_t kBg = 0xFF000000u;

int byteR(uint32_t p) { return static_cast<int>((p >> 16) & 0xFFu); }
int byteG(uint32_t p) { return static_cast<int>((p >> 8) & 0xFFu); }
int byteB(uint32_t p) { return static_cast<int>(p & 0xFFu); }

// TempDir / writeText：与 test_lit_shaders.cpp 同形的文件局部工具。
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
    std::ofstream f(p, std::ios::binary);
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
}

// 四顶点法线全 (0,0,1) 的四边形：常量法线场 ⇒ 插值不改方向（单位向量的常量
// 加权平均仍是它自己），闭式期望在任意内点像素成立。绕向与 test_lit_shaders
// 的 litFlatQuad 相同（NDC CCW ⇒ top-down CW，默认剔除下可见）。
sr::Mesh normalQuad() {
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

// 渲一帧（剔除关掉，与 test_lit_shaders 的 drawQuad 同形）。
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

void expectColorNear(const sr::LinearColor& c, float r, float g, float b, float tol) {
    SR_ASSERT_NEAR(c.r, r, tol);
    SR_ASSERT_NEAR(c.g, g, tol);
    SR_ASSERT_NEAR(c.b, b, tol);
}

}  // namespace

// ---- Step 1: 编码契约（线性 (N+1)/2 + 出口恰好一次 gamma）--------------------

SR_TEST(normal_view_encodes_N_to_half_float_and_is_gamma_encoded_once) {
    sr::FragmentUniforms fu;   // lights/mat/texture 全 nullptr：调试视图不许依赖光照状态
    sr::FragmentInput fi;
    sr::FragmentOutput fo;

    // N=(0,0,1) ⇒ 线性 (0.5,0.5,1) ⇒ 字节 (188,188,255)。188 的推导见文件头。
    fi.v.worldNormal = sr::Vec3{0.0f, 0.0f, 1.0f};
    SR_ASSERT(sr::debugNormalFS(fi, fu, fo));
    expectColorNear(fo.color, 0.5f, 0.5f, 1.0f, 1e-6f);
    const uint32_t p = sr::encodeSRGB(fo.color);   // 颜色目标出口的那一次编码
    SR_ASSERT_EQ(byteR(p), 188);
    SR_ASSERT_EQ(byteG(p), 188);
    SR_ASSERT_EQ(byteB(p), 255);
    // "恰好一次"的两个反例（都离 188 很远，上面的钉住同时杀它们）：
    //   不编码 ⇒ 128；FS 预编码、出口再编一次 ⇒ 把已编码值 188/255≈0.7373 当线性
    //   再编码 ⇒ linearToSrgb8(188/255) ≈ 223。（注意 linearToSrgb8(srgb8ToLinear(188))
    //   是解码→编码的**往返**，≈188 本身，不是双重编码 —— 测量阶段抓到过这个混淆。）
    // 这里把两个反例值算出来自检差得开（不是常数断言，C4127）。
    const uint8_t noEncode = static_cast<uint8_t>(std::lround(0.5f * 255.0f));
    const uint8_t doubleEncode = sr::linearToSrgb8(188.0f / 255.0f);
    SR_ASSERT(noEncode != 188);
    SR_ASSERT(doubleEncode != 188);

    // N=(1,0,0) ⇒ (1,0.5,0.5) ⇒ (255,188,188)：三通道互异情形，通道对调变异必须红。
    fi.v.worldNormal = sr::Vec3{1.0f, 0.0f, 0.0f};
    SR_ASSERT(sr::debugNormalFS(fi, fu, fo));
    const uint32_t px = sr::encodeSRGB(fo.color);
    SR_ASSERT_EQ(byteR(px), 255);
    SR_ASSERT_EQ(byteG(px), 188);
    SR_ASSERT_EQ(byteB(px), 188);

    // N=(0,-1,0) ⇒ (0.5,0,0.5) ⇒ (188,0,188)：负半轴映到 0，
    // 杀 "(1−N)/2"（符号反）与 "abs(N)"（丢符号）两个编码变异。
    fi.v.worldNormal = sr::Vec3{0.0f, -1.0f, 0.0f};
    SR_ASSERT(sr::debugNormalFS(fi, fu, fo));
    const uint32_t pn = sr::encodeSRGB(fo.color);
    SR_ASSERT_EQ(byteR(pn), 188);
    SR_ASSERT_EQ(byteG(pn), 0);
    SR_ASSERT_EQ(byteB(pn), 188);

    // 非单位法线（插值会缩短：单位向量的加权平均是弦不是弧）：(1/3,1/3,1/3)
    // 长度 1/√3 ⇒ normalize 后各 1/√3 ⇒ 线性 (1+1/√3)/2 ≈ 0.788675135 ⇒ 字节 230。
    // 丢掉 normalize ⇒ 线性 2/3 ⇒ 字节 213 —— 差 17 字节，变异必须红（夹具判别力：
    // 单位法线夹具对"丢 normalize"是盲的，这条非单位夹具才是它的落点）。
    fi.v.worldNormal = sr::Vec3{1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f};
    SR_ASSERT(sr::debugNormalFS(fi, fu, fo));
    expectColorNear(fo.color, 0.788675135f, 0.788675135f, 0.788675135f, 1e-5f);
    const uint32_t ps = sr::encodeSRGB(fo.color);
    SR_ASSERT_EQ(byteR(ps), 230);
    SR_ASSERT_EQ(byteG(ps), 230);
    SR_ASSERT_EQ(byteB(ps), 230);
    const uint8_t unnormalized = sr::linearToSrgb8(2.0f / 3.0f);
    SR_ASSERT(unnormalized != byteR(ps));   // 两个答案差得开（实测 213 ≠ 230）

    // 零向量约定（vec.h）：退化法线 ⇒ normalize 返回零向量 ⇒ 中灰 (0.5,0.5,0.5)
    // ⇒ (188,188,188)。**不是黑也不是 NaN** —— 中灰正是黑/NaN 会被误认成的颜色，
    // 所以约定必须钉住：NaN 经 encodeSRGB 的 clamp01 会变字节 0（黑），与中灰可区分。
    fi.v.worldNormal = sr::Vec3{0.0f, 0.0f, 0.0f};
    SR_ASSERT(sr::debugNormalFS(fi, fu, fo));
    const uint32_t pz = sr::encodeSRGB(fo.color);
    SR_ASSERT_EQ(byteR(pz), 188);
    SR_ASSERT_EQ(byteG(pz), 188);
    SR_ASSERT_EQ(byteB(pz), 188);

    // lights 独立性：挂上 rig/mat 重算 ⇒ 输出逐字节相同（调试视图不读 u）。
    sr::LightRig rig;
    rig.keyColor = sr::LinearColor{1.0f, 0.0f, 0.0f};
    rig.keyIntensity = 3.0f;
    sr::MaterialShade ms;
    ms.diffuse = sr::LinearColor{0.25f, 0.5f, 0.75f};
    sr::FragmentUniforms fuLit = fu;
    fuLit.lights = &rig;
    fuLit.mat = &ms;
    fi.v.worldNormal = sr::Vec3{0.0f, 0.0f, 1.0f};
    sr::FragmentOutput foLit;
    SR_ASSERT(sr::debugNormalFS(fi, fuLit, foLit));
    SR_ASSERT_EQ(sr::encodeSRGB(foLit.color), p);
}

// ---- Step 1: D23 的路由表 ----------------------------------------------------

SR_TEST(planView_routes_normal_to_an_FS_and_depth_gray_to_sinks) {
    // 枚举值 = V 循环顺序（main.cpp 的 %4 依赖 static_cast<int>），钉住。
    SR_ASSERT_EQ(static_cast<int>(app::ViewMode::Color), 0);
    SR_ASSERT_EQ(static_cast<int>(app::ViewMode::Depth), 1);
    SR_ASSERT_EQ(static_cast<int>(app::ViewMode::Gray), 2);
    SR_ASSERT_EQ(static_cast<int>(app::ViewMode::Normal), 3);

    // Ruling 67：defaultFS 传两个不同的有效值（texturedFS / flatColorFS）各一遍 ——
    // "硬编码 FS"或"抄错字段"的变异在两个值下都必须红。
    const app::ViewPlan n1 = app::planView(app::ViewMode::Normal, sr::texturedFS);
    SR_ASSERT(n1.useFragmentShader);
    SR_ASSERT(n1.fragmentShader == sr::debugNormalFS);
    // 法线视图的 sink 必须是 Color（恒等）：再过后处理会把 (N+1)/2 编码改写掉。
    SR_ASSERT(n1.sink == sr::DebugViewSink::View::Color);
    SR_ASSERT(std::strcmp(n1.name, "NORMAL") == 0);
    const app::ViewPlan n2 = app::planView(app::ViewMode::Normal, sr::flatColorFS);
    SR_ASSERT(n2.useFragmentShader);
    SR_ASSERT(n2.fragmentShader == sr::debugNormalFS);   // Normal 无视 defaultFS
    SR_ASSERT(n2.sink == sr::DebugViewSink::View::Color);

    const app::ViewPlan d1 = app::planView(app::ViewMode::Depth, sr::texturedFS);
    SR_ASSERT_FALSE(d1.useFragmentShader);
    SR_ASSERT(d1.fragmentShader == sr::texturedFS);   // 原样透传，不是硬编码
    SR_ASSERT(d1.sink == sr::DebugViewSink::View::Depth);
    SR_ASSERT(std::strcmp(d1.name, "DEPTH") == 0);
    const app::ViewPlan d2 = app::planView(app::ViewMode::Depth, sr::flatColorFS);
    SR_ASSERT_FALSE(d2.useFragmentShader);
    SR_ASSERT(d2.fragmentShader == sr::flatColorFS);
    SR_ASSERT(d2.sink == sr::DebugViewSink::View::Depth);

    const app::ViewPlan g1 = app::planView(app::ViewMode::Gray, sr::texturedFS);
    SR_ASSERT_FALSE(g1.useFragmentShader);
    SR_ASSERT(g1.fragmentShader == sr::texturedFS);
    SR_ASSERT(g1.sink == sr::DebugViewSink::View::Gray);
    SR_ASSERT(std::strcmp(g1.name, "GRAY") == 0);
    const app::ViewPlan g2 = app::planView(app::ViewMode::Gray, sr::flatColorFS);
    SR_ASSERT_FALSE(g2.useFragmentShader);
    SR_ASSERT(g2.fragmentShader == sr::flatColorFS);
    SR_ASSERT(g2.sink == sr::DebugViewSink::View::Gray);

    // 夹具判别力自检：三个 sink 映射两两不同（对调变异可观测），名字两两不同。
    SR_ASSERT(d1.sink != g1.sink);
    SR_ASSERT(n1.sink != d1.sink);
    SR_ASSERT(std::strcmp(n1.name, d1.name) != 0);
    SR_ASSERT(std::strcmp(d1.name, g1.name) != 0);
}

SR_TEST(planView_falls_back_to_the_default_FS_for_color_view) {
    const app::ViewPlan c1 = app::planView(app::ViewMode::Color, sr::texturedFS);
    SR_ASSERT_FALSE(c1.useFragmentShader);
    SR_ASSERT(c1.fragmentShader == sr::texturedFS);
    SR_ASSERT(c1.sink == sr::DebugViewSink::View::Color);
    SR_ASSERT(std::strcmp(c1.name, "COLOR") == 0);
    // 第二个有效值：fallback 必须是"传进来的那个"，不是写死的 texturedFS。
    const app::ViewPlan c2 = app::planView(app::ViewMode::Color, sr::flatColorFS);
    SR_ASSERT_FALSE(c2.useFragmentShader);
    SR_ASSERT(c2.fragmentShader == sr::flatColorFS);
    SR_ASSERT(c2.sink == sr::DebugViewSink::View::Color);

    // Color sink 是恒等（法线视图共用它）：present 不改一个字节。
    Canvas c{4, 4};
    c.rt.color.clear(0xFF112233u);
    sr::DebugViewSink sink;
    sink.view = c1.sink;
    sink.present(c.rt);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) SR_ASSERT_EQ(c.rt.color.readPacked(x, y), 0xFF112233u);
}

// ---- 接线级证明：像素编码的是**旋转后**的世界法线 ------------------------------

SR_TEST(normal_view_wiring_encodes_the_rotated_world_normal) {
    // 派发词的"诚实断言"：不是只调 FS，而是走 planView → PipelineState → BackendA
    // 的真实光栅化（VS 的 normalMatrix → 透视正确插值 → FS → encodeSRGB 出口）。
    // 在场景级把 normalMatrix 退回单位阵（M6.0 回归的形状）必须在这里红 ——
    // 这是 M6.0 修复的像素级接线证明（Ruling 85：单测证数学，视图证接线）。
    // app 的最后一跳（g_viewMode → renderFrame 的 FS/sink 选择）在 Win32 main.cpp，
    // 不进 tests 目标，留给 Step 7 的肉眼门（报告里写明钉在哪一层）。
    const sr::Mesh quad = normalQuad();
    const sr::FragmentUniforms fu;   // lights/mat 全 nullptr：视图不依赖光

    // model = 绕 Y 转 30°：N_object=(0,0,1) ⇒ N_world=(sin30,0,cos30)=(0.5,0,0.8660254)。
    // view/proj 单位阵 ⇒ 顶点坐标直当 clip 用（test_lit_shaders 同手法）。
    sr::VertexUniforms vu;
    vu.model = vu.view = vu.proj = sr::Mat4::identity();
    vu.model = sr::Mat4::rotationY(0.5235987756f);   // π/6
    SR_ASSERT(sr::setNormalMatrix(vu, vu.model, "test "));   // 生产帮手（P91）

    const app::ViewPlan plan = app::planView(app::ViewMode::Normal, sr::texturedFS);
    SR_ASSERT(plan.useFragmentShader);

    Canvas c{kW, kH};
    sr::FrameStats s;
    drawQuad(c, quad, sr::transformVS, plan.fragmentShader, vu, fu, s);
    SR_ASSERT(s.fragmentsWritten > 1000);
    // 常量法线场 ⇒ 插值不改方向 ⇒ 闭式期望 (N+1)/2 = (0.75, 0.5, 0.9330127)。
    // 容差 0.01：8-bit 量化线性半步长上界 0.00446（M6.4 报告的推导，全值域成立）。
    const sr::LinearColor got = sr::decodeSRGB(c.rt.color.readPacked(32, 32));
    expectColorNear(got, 0.75f, 0.5f, 0.9330127f, 0.01f);
    // g 通道 N.y=0 精确（rotationY 不碰 y，常量零场插值仍零）⇒ 字节 188 精确钉。
    SR_ASSERT_EQ(byteG(c.rt.color.readPacked(32, 32)), 188);

    // 反例：normalMatrix 退回单位阵 ⇒ N 保持 (0,0,1) ⇒ (0.5,0.5,1)。
    // r 通道 0.75 → 0.5，差 0.25 ≫ 0.01：场景级 identity 绕过必须红。
    // （测试代码允许写单位阵字面量；M6.9 的 grep 守卫只查生产侧。）
    sr::VertexUniforms vuId = vu;
    vuId.normalMatrix = sr::Mat4::identity();
    Canvas id{kW, kH};
    sr::FrameStats si;
    drawQuad(id, quad, sr::transformVS, plan.fragmentShader, vuId, fu, si);
    const sr::LinearColor gotId = sr::decodeSRGB(id.rt.color.readPacked(32, 32));
    expectColorNear(gotId, 0.5f, 0.5f, 1.0f, 0.01f);
    SR_ASSERT(c.colorMem != id.colorMem);
    SR_ASSERT(std::fabs(got.r - gotId.r) > 0.2f);   // 夹具判别力自检：两答案差得开

    // D23：法线视图的 sink 是恒等 —— 帧末 present(plan.sink) 不许改一个字节。
    const std::vector<uint8_t> before = c.colorMem;
    sr::DebugViewSink sink;
    sink.view = plan.sink;
    sink.present(c.rt);
    SR_ASSERT(c.colorMem == before);

    // lights 独立性（接线级）：挂 rig/mat 重渲 ⇒ 逐字节相同。
    sr::LightRig rig;
    rig.keyColor = sr::LinearColor{1.0f, 0.0f, 0.0f};
    rig.keyIntensity = 3.0f;
    sr::MaterialShade ms;
    sr::FragmentUniforms fuLit = fu;
    fuLit.lights = &rig;
    fuLit.mat = &ms;
    Canvas lit{kW, kH};
    sr::FrameStats sl;
    drawQuad(lit, quad, sr::transformVS, plan.fragmentShader, vu, fuLit, sl);
    SR_ASSERT(lit.colorMem == before);

    // 导入模型路径（drawModel 的 fsOverride 接缝）：带 vn 的 OBJ + 红色 Kd。
    // override = plan.fragmentShader ⇒ 像素编码法线 (0.5,0.5,1)；
    // 不 override ⇒ texturedFS ⇒ Kd 红。两者必须不同（override 真的生效）。
    TempDir dir("m65_wiring_obj");
    writeText(dir.p / "a.obj",
              "mtllib m.mtl\n"
              "v -0.8 -0.8 0.5\nv 0.8 -0.8 0.5\nv 0.8 0.8 0.5\nv -0.8 0.8 0.5\n"
              "vn 0 0 1\n"
              "usemtl red\nf 1//1 2//1 3//1\nf 1//1 3//1 4//1\n");
    writeText(dir.p / "m.mtl", "newmtl red\nKd 1 0 0\n");
    sr::LoadedModel m;
    std::string err;
    const bool ok = sr::loadModel(dir.p / "a.obj", m, err);
    SR_ASSERT(ok);
    if (!ok) {
        std::printf("[m65] wiring OBJ 夹具加载失败: %s\n", err.c_str());
        return;
    }
    sr::VertexUniforms vuObj;
    vuObj.model = vuObj.view = vuObj.proj = sr::Mat4::identity();
    SR_ASSERT(sr::setNormalMatrix(vuObj, vuObj.model, "test "));
    sr::FragmentUniforms base;
    base.baseColor = sr::LinearColor{1.0f, 1.0f, 1.0f};
    Canvas obj{kW, kH};
    obj.rt.color.clear(kBg);
    obj.rt.depth.clear();
    sr::BackendA backend;
    sr::FrameStats so;
    sr::drawModel(backend, m, obj.rt, vuObj, base, &so, plan.fragmentShader);
    SR_ASSERT_EQ(so.drawCalls, 1);
    const sr::LinearColor gotObj = sr::decodeSRGB(obj.rt.color.readPacked(32, 32));
    expectColorNear(gotObj, 0.5f, 0.5f, 1.0f, 0.01f);
    SR_ASSERT_EQ(byteG(obj.rt.color.readPacked(32, 32)), 188);
    SR_ASSERT_EQ(byteB(obj.rt.color.readPacked(32, 32)), 255);
    // 对照：默认（无 override）走 texturedFS ⇒ Kd 红（精确字节，无贴图无光照）。
    Canvas objDef{kW, kH};
    objDef.rt.color.clear(kBg);
    objDef.rt.depth.clear();
    sr::FrameStats sd;
    sr::drawModel(backend, m, objDef.rt, vuObj, base, &sd);
    SR_ASSERT_EQ(objDef.rt.color.readPacked(32, 32), sr::encodeSRGB(sr::LinearColor{1, 0, 0}));
    SR_ASSERT(obj.colorMem != objDef.colorMem);
}
