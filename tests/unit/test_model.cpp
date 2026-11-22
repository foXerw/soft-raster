#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "harness.h"
#include "loader/bmp_writer.h"
#include "loader/model.h"
#include "loader/tga_writer.h"   // 装配测试用生产写出器造 TGA 夹具（不是锚 2，不拿写侧当 oracle）
#include "math/mat.h"
#include "render/builtin_shaders.h"
#include "render/color.h"
#include "render/sampler.h"
#include "render/texture.h"
#include "test_util.h"

// M5.4：装配层测的就是 IO —— 用真文件（临时目录里放 obj/mtl/tga），不是内联字符串。
// 内联文本夹具是 parseObj/parseMtl（core，D2）的事；这一层要验的是路径解析、去重、
// 警告/错误分界、解码器分派，这些只有真文件才走得到。

namespace {

namespace fs = std::filesystem;

// 临时目录 RAII：建在 sr_tmp_bmp/（main.cpp 已统一建好、且 gitignore）下，析构清掉。
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

sr::ImageRGBA8 solidImage(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    sr::ImageRGBA8 img;
    img.width = w;
    img.height = h;
    img.rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0);
    for (int i = 0; i < w * h; ++i) {
        img.rgba[static_cast<size_t>(i) * 4u + 0] = r;
        img.rgba[static_cast<size_t>(i) * 4u + 1] = g;
        img.rgba[static_cast<size_t>(i) * 4u + 2] = b;
        img.rgba[static_cast<size_t>(i) * 4u + 3] = a;
    }
    return img;
}

bool writeSolidTga(const fs::path& p, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    std::string err;
    return sr::writeTga(p, solidImage(w, h, r, g, b, a), err);
}

// 两个三角形、两个材质组（A 在前 B 在后）的最小 OBJ。去重/分派测试共用它。
const char* kTwoMatObj =
    "mtllib m.mtl\n"
    "v 0 0 0\n"
    "v 1 0 0\n"
    "v 0 1 0\n"
    "v 1 1 0\n"
    "usemtl A\n"
    "f 1 2 3\n"
    "usemtl B\n"
    "f 2 4 3\n";

}  // namespace

// ---- Step 1: loadModel 的失败测试 ------------------------------------------

SR_TEST(model_loads_an_obj_with_a_sibling_mtl_and_tga) {
    TempDir dir("m54_sibling");
    writeText(dir.p / "a.obj",
              "mtllib m.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl mat\nf 1 2 3\n");
    writeText(dir.p / "m.mtl", "newmtl mat\nKd 1 1 1\nmap_Kd tex.tga\n");
    SR_ASSERT(writeSolidTga(dir.p / "tex.tga", 2, 2, 200, 100, 50, 255));

    sr::LoadedModel m;
    std::string err;
    const bool ok = sr::loadModel(dir.p / "a.obj", m, err);
    SR_ASSERT(ok);
    if (!ok) std::printf("[model] sibling 加载失败: %s\n", err.c_str());
    SR_ASSERT_EQ(m.model.mesh.triangleCount(), 1);
    SR_ASSERT_EQ(m.model.materials.size(), 1);
    SR_ASSERT_EQ(m.textures.size(), 1);
    SR_ASSERT_EQ(m.stats.triangles, 1);
    if (m.texturePerMaterial.size() == 1) SR_ASSERT_EQ(m.texturePerMaterial[0], 0);
    if (!m.textures.empty()) {
        SR_ASSERT_EQ(m.textures[0].width, 2);
        SR_ASSERT(m.textures[0].srgb);   // 颜色数据 ⇒ 采样时解码（D5）
    }
}

SR_TEST(model_resolves_map_kd_relative_to_the_mtl_directory) {
    // 贴图在 mtl 旁边（sub/）而非 obj 旁边：mtllib sub/m.mtl，map_Kd tex.tga 必须相对 sub/ 解析。
    // 若实现相对 OBJ 目录解析，会去找 dir/tex.tga（不存在）⇒ 错误 ⇒ 本条红。
    TempDir dir("m54_mtldir");
    std::error_code ec;
    fs::create_directories(dir.p / "sub", ec);
    writeText(dir.p / "a.obj",
              "mtllib sub/m.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl mat\nf 1 2 3\n");
    writeText(dir.p / "sub" / "m.mtl", "newmtl mat\nKd 1 1 1\nmap_Kd tex.tga\n");
    SR_ASSERT(writeSolidTga(dir.p / "sub" / "tex.tga", 2, 2, 10, 20, 30, 255));

    sr::LoadedModel m;
    std::string err;
    const bool ok = sr::loadModel(dir.p / "a.obj", m, err);
    SR_ASSERT(ok);
    if (!ok) std::printf("[model] mtl-dir 解析失败: %s\n", err.c_str());
    SR_ASSERT_EQ(m.textures.size(), 1);
}

SR_TEST(model_missing_texture_file_is_an_error_naming_it) {
    // 收紧 3：map_Kd 指向不存在的文件 ⇒ 错误（不降级），err 必须带那个文件名。
    TempDir dir("m54_missingtex");
    writeText(dir.p / "a.obj",
              "mtllib m.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl mat\nf 1 2 3\n");
    writeText(dir.p / "m.mtl", "newmtl mat\nKd 1 1 1\nmap_Kd missing.tga\n");

    sr::LoadedModel m;
    std::string err;
    const bool ok = sr::loadModel(dir.p / "a.obj", m, err);
    SR_ASSERT_FALSE(ok);
    SR_ASSERT_FALSE(err.empty());
    if (err.find("missing.tga") == std::string::npos)
        std::printf("[model] 缺贴图的 err 是：%s\n", err.c_str());
    SR_ASSERT(err.find("missing.tga") != std::string::npos);
    SR_ASSERT(m.textures.empty());   // 失败不留半成品
}

SR_TEST(model_missing_mtl_file_is_a_warning_and_still_draws) {
    // 收紧 3：只发了 obj 没发 mtl ⇒ 警告 + 几何照常显示（defaultMaterial），不是错误。
    TempDir dir("m54_missingmtl");
    writeText(dir.p / "a.obj",
              "mtllib absent.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl X\nf 1 2 3\n");

    sr::LoadedModel m;
    std::string err;
    const bool ok = sr::loadModel(dir.p / "a.obj", m, err);
    SR_ASSERT(ok);
    if (!ok) std::printf("[model] 缺 mtl 不应失败: %s\n", err.c_str());
    SR_ASSERT_EQ(m.model.mesh.triangleCount(), 1);   // 几何无损
    SR_ASSERT(m.model.materials.size() >= 1);        // 有兜底材质
    SR_ASSERT(m.textures.empty());
}

SR_TEST(model_without_mtllib_gets_one_default_material) {
    // 纯 OBJ：能画，纯色白（defaultMaterial 的 Kd 是 1 1 1，不是 pipeline 的 0.8 灰）。
    TempDir dir("m54_nomtl");
    writeText(dir.p / "a.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");

    sr::LoadedModel m;
    std::string err;
    const bool ok = sr::loadModel(dir.p / "a.obj", m, err);
    SR_ASSERT(ok);
    SR_ASSERT_EQ(m.model.materials.size(), 1);
    SR_ASSERT(m.textures.empty());
    if (m.texturePerMaterial.size() == 1) SR_ASSERT_EQ(m.texturePerMaterial[0], -1);
    if (!m.model.materials.empty()) {
        SR_ASSERT_NEAR(m.model.materials[0].diffuse.r, 1.0f, 1e-6f);
        SR_ASSERT_NEAR(m.model.materials[0].diffuse.g, 1.0f, 1e-6f);
        SR_ASSERT_NEAR(m.model.materials[0].diffuse.b, 1.0f, 1e-6f);
    }
}

SR_TEST(model_dedups_two_materials_sharing_one_image) {
    // 两个材质引用同一个文件（都写裸名）⇒ textures.size()==1 而 materials.size()==2。
    TempDir dir("m54_dedup");
    writeText(dir.p / "a.obj", kTwoMatObj);
    writeText(dir.p / "m.mtl",
              "newmtl A\nKd 1 1 1\nmap_Kd tex.tga\n"
              "newmtl B\nKd 1 1 1\nmap_Kd tex.tga\n");
    SR_ASSERT(writeSolidTga(dir.p / "tex.tga", 2, 2, 1, 2, 3, 255));

    sr::LoadedModel m;
    std::string err;
    const bool ok = sr::loadModel(dir.p / "a.obj", m, err);
    SR_ASSERT(ok);
    if (!ok) std::printf("[model] dedup 失败: %s\n", err.c_str());
    SR_ASSERT_EQ(m.model.materials.size(), 2);
    SR_ASSERT_EQ(m.textures.size(), 1);
    if (m.texturePerMaterial.size() == 2) {
        SR_ASSERT_EQ(m.texturePerMaterial[0], 0);
        SR_ASSERT_EQ(m.texturePerMaterial[1], 0);
    }
}

SR_TEST(model_canonicalizes_so_dot_slash_and_bare_name_share_one_texture) {
    // D11 修订：`tex.tga` 与 `./tex.tga` 指向同一文件 ⇒ weakly_canonical 去重 ⇒ 一条 mip 链。
    TempDir dir("m54_canon");
    writeText(dir.p / "a.obj", kTwoMatObj);
    writeText(dir.p / "m.mtl",
              "newmtl A\nKd 1 1 1\nmap_Kd tex.tga\n"
              "newmtl B\nKd 1 1 1\nmap_Kd ./tex.tga\n");
    SR_ASSERT(writeSolidTga(dir.p / "tex.tga", 2, 2, 9, 8, 7, 255));

    sr::LoadedModel m;
    std::string err;
    const bool ok = sr::loadModel(dir.p / "a.obj", m, err);
    SR_ASSERT(ok);
    if (!ok) std::printf("[model] canonical 失败: %s\n", err.c_str());
    SR_ASSERT_EQ(m.model.materials.size(), 2);
    SR_ASSERT_EQ(m.textures.size(), 1);   // 原始字符串不同，canonical 相同 ⇒ 去重
}

// ---- Step 4: drawModel 逐组提交 --------------------------------------------

namespace {

constexpr int kW = 64;
constexpr int kH = 64;
constexpr uint32_t kBg = 0xFF000000u;

// 单位矩阵 ⇒ OBJ 的 v 坐标直接当 clip 空间用（w=1），三角形落在可算的屏幕像素上
// （与 test_draw_range 同一手法）。这一层测的是"按组换材质状态"，不是相机。
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

}  // namespace

SR_TEST(draw_model_uses_one_draw_call_per_group) {
    // 提交计数靠 drawCalls，不靠数三角形（"1 个 draw 交 12 面"与"3 个 draw 各交 4 面"
    // 三角形数相同）。twomats 有三个材质组 ⇒ drawCalls==3；两贴图组共用一张图 ⇒ textures==1。
    // 用单位矩阵：盒子可能在画面外，但 drawCalls 数的是**提交**，与可见性无关。
    sr::LoadedModel m;
    std::string err;
    const fs::path obj = fs::path(SR_SOURCE_DIR) / "assets" / "obj" / "twomats.obj";
    const bool ok = sr::loadModel(obj, m, err);
    SR_ASSERT(ok);
    if (!ok) std::printf("[model] twomats 加载失败: %s\n", err.c_str());
    SR_ASSERT_EQ(m.model.groups.size(), 3);
    SR_ASSERT_EQ(m.model.materials.size(), 3);
    SR_ASSERT_EQ(m.textures.size(), 1);   // Ruling 47 + D11：../tga/checker.tga 去重

    Canvas c{kW, kH};
    c.rt.color.clear(kBg);
    sr::BackendA backend;
    sr::FrameStats s;
    sr::drawModel(backend, m, c.rt, identityVu(), baseFu(), &s);
    SR_ASSERT_EQ(s.drawCalls, 3);
}

SR_TEST(draw_model_paints_each_group_with_its_own_diffuse) {
    // 两个三角形两种 Kd（绿/红），分居屏幕两角。最容易写错的是"循环永远填 materials[0]"
    // ⇒ 两个三角形同色 ⇒ 其中一个角断言红。中心像素各归其色才说明按组换了 baseColor。
    TempDir dir("m54_draw_diffuse");
    writeText(dir.p / "a.obj",
              "mtllib m.mtl\n"
              "v 0.1 0.1 0.5\nv 0.9 0.1 0.5\nv 0.5 0.9 0.5\n"
              "v -0.9 -0.9 0.5\nv -0.1 -0.9 0.5\nv -0.5 -0.1 0.5\n"
              "usemtl green\nf 1 2 3\n"
              "usemtl red\nf 4 5 6\n");
    writeText(dir.p / "m.mtl", "newmtl green\nKd 0 1 0\nnewmtl red\nKd 1 0 0\n");
    sr::LoadedModel m;
    std::string err;
    SR_ASSERT(sr::loadModel(dir.p / "a.obj", m, err));

    Canvas c{kW, kH};
    c.rt.color.clear(kBg);
    sr::BackendA backend;
    sr::FrameStats s;
    sr::drawModel(backend, m, c.rt, identityVu(), baseFu(), &s);
    SR_ASSERT_EQ(s.drawCalls, 2);
    SR_ASSERT_EQ(c.rt.color.readPacked(48, 20), sr::encodeSRGB(sr::LinearColor{0, 1, 0}));
    SR_ASSERT_EQ(c.rt.color.readPacked(16, 52), sr::encodeSRGB(sr::LinearColor{1, 0, 0}));
}

SR_TEST(draw_model_falls_back_to_diffuse_when_no_texture) {
    // 无 map_Kd 的组 ⇒ texturedFS 退回 baseColor 纯色（Ruling 32），不是黑。
    TempDir dir("m54_draw_flat");
    writeText(dir.p / "a.obj",
              "mtllib m.mtl\nv -0.5 -0.5 0.5\nv 0.5 -0.5 0.5\nv 0 0.5 0.5\n"
              "usemtl blue\nf 1 2 3\n");
    writeText(dir.p / "m.mtl", "newmtl blue\nKd 0 0 1\n");
    sr::LoadedModel m;
    std::string err;
    SR_ASSERT(sr::loadModel(dir.p / "a.obj", m, err));
    SR_ASSERT(m.textures.empty());

    Canvas c{kW, kH};
    c.rt.color.clear(kBg);
    sr::BackendA backend;
    sr::FrameStats s;
    sr::drawModel(backend, m, c.rt, identityVu(), baseFu(), &s);
    const uint32_t px = c.rt.color.readPacked(32, 37);
    SR_ASSERT_EQ(px, sr::encodeSRGB(sr::LinearColor{0, 0, 1}));
    SR_ASSERT(px != kBg);   // 不是黑/背景
}

SR_TEST(draw_model_keeps_texture_and_uv_for_textured_groups) {
    // 全屏四边形 + 2x1 贴图（左红右蓝）+ Nearest/ClampEdge/useMip=false。
    // 左侧像素必须是红、右侧必须是蓝 ⇒ 既证明 texture 指针接上了（不是纯色兜底），
    // 又证明 uv 沿四边形变化（不是整面同一个纹素）。Kd 白 ⇒ 像素就是纹素色。
    TempDir dir("m54_draw_tex");
    writeText(dir.p / "a.obj",
              "mtllib m.mtl\n"
              "v -0.9 -0.9 0.5\nv 0.9 -0.9 0.5\nv 0.9 0.9 0.5\nv -0.9 0.9 0.5\n"
              "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
              "usemtl tex\nf 1/1 2/2 3/3 4/4\n");
    writeText(dir.p / "m.mtl", "newmtl tex\nKd 1 1 1\nmap_Kd tex.tga\n");
    sr::ImageRGBA8 img;
    img.width = 2;
    img.height = 1;
    img.rgba = {255, 0, 0, 255, 0, 0, 255, 255};   // 左红右蓝
    std::string werr;
    SR_ASSERT(sr::writeTga(dir.p / "tex.tga", img, werr));

    sr::LoadedModel m;
    std::string err;
    SR_ASSERT(sr::loadModel(dir.p / "a.obj", m, err));
    SR_ASSERT_EQ(m.textures.size(), 1);

    Canvas c{kW, kH};
    c.rt.color.clear(kBg);
    sr::BackendA backend;
    sr::FrameStats s;
    sr::drawModel(backend, m, c.rt, identityVu(), baseFu(), &s);
    SR_ASSERT_EQ(c.rt.color.readPacked(8, 32), sr::encodeSRGB(sr::LinearColor{1, 0, 0}));
    SR_ASSERT_EQ(c.rt.color.readPacked(56, 32), sr::encodeSRGB(sr::LinearColor{0, 0, 1}));
}

// ---- Step 5: sRGB 双重解码守卫（Review Focus 5）----------------------------

SR_TEST(srgb_file_texture_and_linear_kd_land_on_the_same_brightness_as_a_white_tint) {
    // 全 0.5 灰的 TGA（字节 128）+ Kd 1 1 1：采出来必须 ≈ 0.2140（解码**一次**）。
    // 谁哪天在加载时又解了一次，字节会被压到 ~55，采样再解一次 ⇒ ≈ 0.033，本条立刻红
    // （而不是安静地整体偏暗一档）。对照：全 1.0 的程序图 + 白 Kd ⇒ ≈ 1.0。
    TempDir dir("m54_srgb");
    writeText(dir.p / "a.obj",
              "mtllib m.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl g\nf 1 2 3\n");
    writeText(dir.p / "m.mtl", "newmtl g\nKd 1 1 1\nmap_Kd gray.tga\n");
    SR_ASSERT(writeSolidTga(dir.p / "gray.tga", 2, 2, 128, 128, 128, 255));

    sr::LoadedModel m;
    std::string err;
    SR_ASSERT(sr::loadModel(dir.p / "a.obj", m, err));
    SR_ASSERT_EQ(m.textures.size(), 1);
    if (m.textures.empty()) return;
    SR_ASSERT(m.textures[0].srgb);   // 颜色数据 ⇒ 标志为真，解码留给采样时（D5）
    const sr::TexelF t = sr::sampleNearest(m.textures[0], sr::WrapMode::ClampEdge,
                                           sr::WrapMode::ClampEdge, 0.5f, 0.5f);
    SR_ASSERT_NEAR(t.r, 0.2140f, 0.01f);   // 一次解码；双重解码会落到 ~0.033

    const sr::Texture white = sr::makeTexture(solidImage(2, 2, 255, 255, 255, 255), true);
    const sr::TexelF w = sr::sampleNearest(white, sr::WrapMode::ClampEdge,
                                           sr::WrapMode::ClampEdge, 0.5f, 0.5f);
    SR_ASSERT_NEAR(w.r, 1.0f, 1e-3f);
    // Kd 1 1 1 解析成线性白（两端精确），tint 不压暗。
    if (!m.model.materials.empty())
        SR_ASSERT_NEAR(m.model.materials[0].diffuse.r, 1.0f, 1e-6f);
}

// ---- Step 8: 中文目录 + 带空格贴图名走完整条装配 ----------------------------

SR_TEST(model_loads_from_a_chinese_directory_with_a_spaced_texture_name) {
#ifndef _MSC_VER
    // C1′：宽字符临时目录的行为只在 MSVC 主路径稳定（与 test_image_stb 同一条理由）。
    std::printf("[skip] 中文路径用例只在 MSVC 判定（C1′）\n");
    return;
#else
    // D12 / G-M5.5：中文**目录** + `map_Kd -s 1 1 1 my brick.png`（选项 token + 含空格文件名）。
    // 目录用宽字符 fs::path 传给 loadModel（ifstream(path) 宽字符打开）；目录内的 obj/mtl/贴图
    // 文件名是 ASCII。**OBJ 文本里嵌的 UTF-8 文件名 → path 的编码转换是 M5.6 的 nativePath**
    // （D18：全仓库唯一的编码转换处），不在本任务范围 —— 所以这里 mtllib/map_Kd 都用 ASCII 名。
    // 后缀 .png ⇒ Ruling 63 分派到第三方解码器；本项目没有 PNG 编码器（设计 §3 非目标），
    // 夹具实际是 BMP 字节命名 .png（与 test_image_stb.cpp 的中文路径用例同一先例）——
    // 第三方按**内容**嗅探格式，这条测的是路径/选项/分派，不是 PNG 编码。
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(L"sr_tmp_bmp") / L"中文模型目录_m54";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        std::printf("[skip] 无法创建中文目录: %s\n", ec.message().c_str());
        return;
    }
    writeText(dir / L"a.obj",
              "mtllib m.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nusemtl brick\nf 1 2 3\n");
    writeText(dir / L"m.mtl", "newmtl brick\nKd 1 1 1\nmap_Kd -s 1 1 1 my brick.png\n");

    // 先写 BMP 到 ASCII 路径，再把字节搬进中文目录的 "my brick.png"（宽字符 ofstream）。
    std::vector<uint8_t> mem(4 * 2 * 4, 0);
    sr::ColorTarget ct{reinterpret_cast<uint32_t*>(mem.data()), 4, 2, 4 * 4};
    for (int x = 0; x < 4; ++x) {
        ct.writePacked(x, 0, sr::packBGRA(255, 0, 0, 255));   // 上行红
        ct.writePacked(x, 1, sr::packBGRA(0, 0, 255, 255));   // 下行蓝
    }
    std::string berr;
    SR_ASSERT(sr::writeBmp("sr_tmp_bmp/m54_cn_src.bmp", ct, berr));
    const fs::path texPath = dir / L"my brick.png";
    {
        std::ifstream in("sr_tmp_bmp/m54_cn_src.bmp", std::ios::binary);
        std::ofstream out(texPath, std::ios::binary);
        out << in.rdbuf();
        SR_ASSERT(static_cast<bool>(out));
    }

    sr::LoadedModel m;
    std::string err;
    const bool ok = sr::loadModel(dir / L"a.obj", m, err);
    SR_ASSERT(ok);
    if (!ok) std::printf("[model] 中文目录装配失败: %s\n", err.c_str());
    SR_ASSERT_EQ(m.textures.size(), 1);
    if (!m.textures.empty()) {
        SR_ASSERT_EQ(m.textures[0].width, 4);
        SR_ASSERT_EQ(m.textures[0].height, 2);
        // 含空格文件名被正确解析（没被 -s 的参数吃掉），且第三方分派走通。
        const uint8_t* top = m.textures[0].texel(0, 0, 0);
        SR_ASSERT(top != nullptr);
        if (top != nullptr) {
            SR_ASSERT_EQ(top[0], 255);   // 上行红（top-down + BGR→RGB）
            SR_ASSERT_EQ(top[2], 0);
        }
    }
    fs::remove_all(dir, ec);
    fs::remove("sr_tmp_bmp/m54_cn_src.bmp", ec);
#endif
}