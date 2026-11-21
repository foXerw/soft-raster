#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "harness.h"
#include "math/transform.h"
#include "math/vec.h"
#include "render/builtin_shaders.h"
#include "render/color.h"
#include "render/pipeline.h"
#include "render/render_backend.h"
#include "render/sampler.h"
#include "render/stats.h"
#include "render/texture.h"
#include "scene/golden_scene.h"
#include "scene/mesh.h"
#include "test_util.h"

namespace {

// 这条文件测的是 **M4 的端到端**：一张程序棋盘贴图，经过 VS → 裁剪 → 视口 → Setup →
// 透视正确插值 → 三线性采样 → sRGB 编码，最后落在 320x180 帧里的哪个像素、是什么颜色。
// 判据不是"渲染出来的图看起来像贴图"，而是"uv(0,0) 那一格的红出现在我算出来的那个像素上"。
//
// 为什么靠颜色分类而不是逐字节相等：golden 场景用三线性 + mip，格心会与邻居混一点，
// 混合比重随姿态连续变化 ⇒ 逐字节断言会在权重上抖动。分类只回答"这是哪个角"，
// 而这正是本文件要判的东西（朝向 / 接线），亮度细节交给 assets/golden 的逐像素比对。

struct PaletteLin {
    sr::LinearColor origin, uAxis, vAxis, dark, light;
};

PaletteLin paletteLinear() {
    const sr::CheckerPalette p{};
    const auto lin = [](const uint8_t c[4]) {
        return sr::LinearColor{sr::srgb8ToLinear(c[0]), sr::srgb8ToLinear(c[1]),
                               sr::srgb8ToLinear(c[2])};
    };
    return PaletteLin{lin(p.origin), lin(p.uAxis), lin(p.vAxis), lin(p.dark), lin(p.light)};
}

float dist2(const sr::LinearColor& a, const sr::LinearColor& b) {
    const float dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
    return dr * dr + dg * dg + db * db;
}

// 0=origin(红) 1=uAxis(绿) 2=vAxis(蓝) 3=dark 4=light，-1=分不出（差距不到 2 倍）。
int classify(const sr::LinearColor& c) {
    static const PaletteLin p = paletteLinear();
    const sr::LinearColor entries[5] = {p.origin, p.uAxis, p.vAxis, p.dark, p.light};
    int best = 0;
    float bestD = dist2(c, entries[0]);
    for (int i = 1; i < 5; ++i) {
        const float d = dist2(c, entries[i]);
        if (d < bestD) { bestD = d; best = i; }
    }
    // 与第二名的距离必须拉开一倍以上：三线性混合出来的中间色不该被算成"某个角"。
    float second = 1e30f;
    for (int i = 0; i < 5; ++i) {
        if (i == best) continue;
        const float d = dist2(c, entries[i]);
        if (d < second) second = d;
    }
    if (second < bestD * 4.0f) return -1;   // 距离的平方 ⇒ 4 倍 = 距离 2 倍
    return best;
}

struct Face {
    sr::Vec3 normal;
    sr::Vec3 corner[4];
    sr::Vec2 cornerUv[4];   // **读出来**，不是假设：见下面 uvPoint 的理由
};

// 从 mesh 本身取面（位置**与 uv 都取**），而不是在测试里重抄一遍立方体几何：
// 抄错了这条测试就在测想象。makeCube(size, perFaceNormals=true) 的 appendQuad
// 每面独占 4 个顶点（4f..4f+3），面序 +X -X +Y -Y +Z -Z。
std::vector<Face> cubeFaces() {
    const sr::Mesh m = sr::makeCube(sr::kGoldenCubeSize, true);
    std::vector<Face> fs;
    if (m.positions.size() < 24 || m.normals.size() < 24 || m.uvs.size() < 24) {
        return fs;   // 形状不对就空手而归，别越界索引（Ruling 26）
    }
    for (int f = 0; f < 6; ++f) {
        Face face;
        face.normal = m.normals[static_cast<size_t>(f) * 4u];
        for (int k = 0; k < 4; ++k) {
            const size_t i = static_cast<size_t>(f) * 4u + static_cast<size_t>(k);
            face.corner[k] = m.positions[i];
            face.cornerUv[k] = m.uvs[i];
        }
        fs.push_back(face);
    }
    return fs;
}

sr::Vec3 worldOf(const sr::Vec3& object) {
    return sr::xyz(sr::goldenModel() * sr::makeVec4(object, 1.0f));
}

// 面上 uv=(tu,tv) 处的物体空间点。
// 权重从**这个面自己的 uv** 推，而不是假设"角 k 的 uv 是第 k 个"：
// 渲染器读的就是 mesh 里的 uv，测试若另外记一份顺序，两份会在某一天分岔
// —— 而分岔的症状是这条测试红，报的却是"采样错了"（uv 方向约定改过一次，见 Ruling 42）。
// 四个角的 uv 各是单位正方形的一个角，所以角 k 的权重就是它的两条基函数之积。
sr::Vec3 uvPoint(const Face& f, float tu, float tv) {
    sr::Vec3 acc{0.0f, 0.0f, 0.0f};
    for (int k = 0; k < 4; ++k) {
        const float wu = f.cornerUv[k].x > 0.5f ? tu : (1.0f - tu);
        const float wv = f.cornerUv[k].y > 0.5f ? tv : (1.0f - tv);
        acc = acc + f.corner[k] * (wu * wv);
    }
    return acc;
}

// 世界点 → 像素。NDC→像素这两行**故意手写**（含 y 翻转），不调用 sr::Viewport：
// 本文件要判的正是"uv 与屏幕的朝向关系"，拿被测代码的视口算期望值等于让它给自己打分。
// 视口自身的翻转由 test_viewport 与 M3 golden 守着（分工，不是漏洞）。
sr::Vec2 pixelOf(const sr::Vec3& world, int w, int h) {
    const sr::Vec4 clip = sr::goldenProjection(w, h) * (sr::goldenView() * sr::makeVec4(world, 1.0f));
    if (!(std::fabs(clip.w) > 1e-6f)) return sr::Vec2{-1e9f, -1e9f};   // 退化：让调用方的断言红
    const float invW = 1.0f / clip.w;
    const float ndcX = clip.x * invW;
    const float ndcY = clip.y * invW;
    return sr::Vec2{(ndcX * 0.5f + 0.5f) * static_cast<float>(w),
                    (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(h)};
}

// 最朝向相机的那个面。用"外法线·(eye-中心) 最大"这个语义判据，
// 而不是我口算"哪个面在前"——口算绕向/朝向在本项目已经错过两次（Ruling 20/21）。
int frontFaceIndex(const std::vector<Face>& fs) {
    const sr::Vec3 eye{0.0f, 0.0f, sr::kGoldenCamZ};
    int best = -1;
    float bestDot = -2.0f;
    for (size_t i = 0; i < fs.size(); ++i) {
        const sr::Vec3 center = worldOf((fs[i].corner[0] + fs[i].corner[1] + fs[i].corner[2] +
                                         fs[i].corner[3]) * 0.25f);
        const sr::Vec3 n = sr::normalize(sr::xyz(sr::goldenModel() * sr::makeVec4(fs[i].normal, 0.0f)));
        const sr::Vec3 toEye = sr::normalize(eye - center);
        const float d = n.x * toEye.x + n.y * toEye.y + n.z * toEye.z;
        if (d > bestDot) { bestDot = d; best = static_cast<int>(i); }
    }
    return best;
}

int classifyPixel(const Canvas& c, float px, float py) {
    const int x = static_cast<int>(std::floor(px));
    const int y = static_cast<int>(std::floor(py));
    if (x < 0 || y < 0 || x >= c.rt.color.width || y >= c.rt.color.height) return -2;  // 出画
    return classify(sr::decodeSRGB(c.rt.color.readPacked(x, y)));
}

const char* className(int k) {
    switch (k) {
        case 0: return "origin(红)";
        case 1: return "uAxis(绿)";
        case 2: return "vAxis(蓝)";
        case 3: return "dark";
        case 4: return "light";
        case -2: return "<出画>";
        default: return "<分不清>";
    }
}

void expectClass(const Canvas& c, const sr::Vec2 px, int want, const char* what) {
    const int got = classifyPixel(c, px.x, px.y);
    if (got != want) {
        std::printf("[golden-tex] %s 在像素 (%.1f,%.1f) 得到 %s，期望 %s\n", what,
                    static_cast<double>(px.x), static_cast<double>(px.y), className(got),
                    className(want));
    }
    SR_ASSERT_EQ(got, want);
}

// 格心的 uv：第 k 格占 [k/cells, (k+1)/cells]，中心是 (k+0.5)/cells。
constexpr float cellCenterUv(int cell) {
    return (static_cast<float>(cell) + 0.5f) / static_cast<float>(sr::kGoldenCheckerCells);
}

}  // namespace

SR_TEST(golden_checker_texture_has_the_shape_the_scene_claims) {
    const sr::Texture& t = sr::goldenCheckerTexture();
    // 尺寸写成字面量而不是"cells*cellPx"：后者对任何 cellPx 都恒真，等于没断言
    // （4.3 那族"夹具在某个操作下不变"的坑）。4x4 格、每格 32 像素 ⇒ 128 见方。
    SR_ASSERT_EQ(t.width, 128);
    SR_ASSERT_EQ(t.height, 128);
    // 128x128 ⇒ 链是 128,64,32,16,8,4,2,1 = 8 级（Ruling 24：每维下限 1，链以 1x1 收尾）。
    SR_ASSERT_EQ(t.levelCount(), 8);
    SR_ASSERT(t.srgb);   // 它是**颜色数据**，采样时要解码到线性（Ruling 27）
    // 单例：两次取到的是同一个对象（贴图与 mip 链只算一次）。
    SR_ASSERT(&sr::goldenCheckerTexture() == &t);
}

SR_TEST(the_uv_origin_marker_lands_where_uv_zero_zero_projects) {
    Canvas c{sr::kGoldenWidth, sr::kGoldenHeight};
    sr::FrameStats stats;
    sr::renderGoldenSceneTextured(c.rt, &stats);

    const std::vector<Face> fs = cubeFaces();
    SR_ASSERT_EQ(fs.size(), 6u);
    if (fs.empty()) return;
    const int front = frontFaceIndex(fs);
    SR_ASSERT(front >= 0 && front < 6);
    if (front < 0 || front >= static_cast<int>(fs.size())) return;
    const Face& f = fs[static_cast<size_t>(front)];

    const int last = sr::kGoldenCheckerCells - 1;
    const float lo = cellCenterUv(0);
    const float hi = cellCenterUv(last);
    // 三个标记角 + 一个素角。四条一起才把"朝向"钉死：任何一次 u/v 对调或翻转，
    // 都会把某个标记色搬到另一个角上（这正是 test_texture_gen 里那条
    // "正方形的任何对称都不保持这张图"要保证能被看见的前提）。
    expectClass(c, pixelOf(worldOf(uvPoint(f, lo, lo)), sr::kGoldenWidth, sr::kGoldenHeight), 0,
                "uv(0,0) 格心");
    expectClass(c, pixelOf(worldOf(uvPoint(f, hi, lo)), sr::kGoldenWidth, sr::kGoldenHeight), 1,
                "uv(1,0) 格心");
    expectClass(c, pixelOf(worldOf(uvPoint(f, lo, hi)), sr::kGoldenWidth, sr::kGoldenHeight), 2,
                "uv(0,1) 格心");
    expectClass(c, pixelOf(worldOf(uvPoint(f, hi, hi)), sr::kGoldenWidth, sr::kGoldenHeight), 3,
                "uv(1,1) 格心（素棋盘 dark）");
}

SR_TEST(textured_frame_carries_the_checker_over_a_wide_swatch_not_one_lucky_pixel) {
    // 一个像素对上不算证据（可能是背景色的巧合）。这里在正面中心到各标记角之间
    // 各取一小块 3x3，要求**全部 9 个像素**都分类成同一个角 ⇒ 排除"蒙对一个点"。
    Canvas c{sr::kGoldenWidth, sr::kGoldenHeight};
    sr::FrameStats stats;
    sr::renderGoldenSceneTextured(c.rt, &stats);
    const std::vector<Face> fs = cubeFaces();
    if (fs.empty()) { SR_ASSERT_FALSE(fs.empty()); return; }
    const int front = frontFaceIndex(fs);
    if (front < 0) { SR_ASSERT(front >= 0); return; }
    const Face& f = fs[static_cast<size_t>(front)];
    const float lo = cellCenterUv(0);

    const sr::Vec2 p = pixelOf(worldOf(uvPoint(f, lo, lo)), sr::kGoldenWidth, sr::kGoldenHeight);
    int same = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (classifyPixel(c, p.x + dx, p.y + dy) == 0) ++same;
        }
    }
    if (same != 9) std::printf("[golden-tex] 原点标记的 3x3 邻域只有 %d/9 个像素是红色\n", same);
    SR_ASSERT_EQ(same, 9);
}

SR_TEST(the_texture_reads_right_side_up_on_screen) {
    // 全栈 top-down 的**屏幕侧**保证（Ruling 42）：uv(0,0) 那一格（红标记）必须出现在
    // uv(0,1) 那一格（蓝标记）的**上方**，而 uv(1,0)（绿标记）在红的**右方**。
    // 与 test_mesh 的 cube_uv_follows_each_faces_own_up_and_right 是一对：
    // 那条管生成器，这条管"最终画面上读起来对不对"。
    //
    // 三条分类断言不是重复 marker_lands：它们保证这个"上下左右"的比较**不是空的** ——
    // 若预测的像素位置根本不在标记格上，排序可能仍然成立（背景也分上下），那就是假通过。
    // 10 像素的余量也不是精度要求：正面在 320x180 里跨约 50 像素，真接反了差的是整个面高。
    Canvas c{sr::kGoldenWidth, sr::kGoldenHeight};
    sr::FrameStats stats;
    sr::renderGoldenSceneTextured(c.rt, &stats);
    const std::vector<Face> fs = cubeFaces();
    if (fs.empty()) { SR_ASSERT_FALSE(fs.empty()); return; }
    const int front = frontFaceIndex(fs);
    if (front < 0) { SR_ASSERT(front >= 0); return; }
    const Face& f = fs[static_cast<size_t>(front)];

    const float lo = cellCenterUv(0);
    const float hi = cellCenterUv(sr::kGoldenCheckerCells - 1);
    const sr::Vec2 red = pixelOf(worldOf(uvPoint(f, lo, lo)), sr::kGoldenWidth, sr::kGoldenHeight);
    const sr::Vec2 green = pixelOf(worldOf(uvPoint(f, hi, lo)), sr::kGoldenWidth, sr::kGoldenHeight);
    const sr::Vec2 blue = pixelOf(worldOf(uvPoint(f, lo, hi)), sr::kGoldenWidth, sr::kGoldenHeight);
    expectClass(c, red, 0, "uv(0,0) 格心");
    expectClass(c, green, 1, "uv(1,0) 格心");
    expectClass(c, blue, 2, "uv(0,1) 格心");

    // v 轴在屏幕上向下 ⇒ v 小的（红）y 更小
    SR_ASSERT(red.y + 10.0f < blue.y);
    // u 轴在屏幕上向右 ⇒ u 小的（红）x 更小
    SR_ASSERT(red.x + 10.0f < green.x);
}

SR_TEST(cell_boundary_is_a_blend_so_the_filter_is_really_in_the_loop) {
    // 过滤（双线性/三线性）在格子边界上必须留下"混出来的中间调"。
    // 选 uv=(0.75, 0.25)：那是第 2 列与第 3 列的分界线，两侧的格子都是**素棋盘**
    // （(2,1) 奇⇒light、(3,1) 偶⇒dark），所以这条不会与角标记混为一谈。
    // Nearest 在这里会给出纯粹的 light 或 dark（分类器能分辨）；
    // 带过滤时两侧各占一半 ⇒ 分类器"分不清"，这正是期望值。
    //
    // **这条能分辨 Nearest 与"任何带过滤的组合"，但不能分辨 Bilinear 与 Trilinear**：
    // 4x4 格的贴图没有足够高的频率，mip 开与关在这个尺寸上几乎给不出不同的像素。
    // mip 的取舍因此不靠这条守，而靠 4.1/4.4/4.5 的 11 条 LOD/采样单测 +
    // M4 golden 的逐字节比对（改 useMip 会改变参考图 ⇒ 立刻红）+ 产品方在窗口里
    // 亲眼做的"关 mip 会闪"验收。写在这里是为了不让后来人误以为这条守了 mip。
    Canvas c{sr::kGoldenWidth, sr::kGoldenHeight};
    sr::FrameStats stats;
    sr::renderGoldenSceneTextured(c.rt, &stats);
    const std::vector<Face> fs = cubeFaces();
    if (fs.empty()) { SR_ASSERT_FALSE(fs.empty()); return; }
    const int front = frontFaceIndex(fs);
    if (front < 0) { SR_ASSERT(front >= 0); return; }
    const Face& f = fs[static_cast<size_t>(front)];

    const sr::Vec2 p = pixelOf(worldOf(uvPoint(f, 0.75f, 0.25f)), sr::kGoldenWidth, sr::kGoldenHeight);
    const int k = classifyPixel(c, p.x, p.y);
    const sr::LinearColor got = sr::decodeSRGB(c.rt.color.readPacked(
        static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y))));
    if (k != -1) {
        std::printf("[golden-tex] 格子边界被分类成 %s（lin %.3f,%.3f,%.3f），期望分不出\n",
                    className(k), static_cast<double>(got.r), static_cast<double>(got.g),
                    static_cast<double>(got.b));
    }
    SR_ASSERT_EQ(k, -1);
    // 而且它必须**落在 dark 与 light 之间**，不是任意一个怪颜色：两侧各占一半时
    // 线性值应接近 (dark+light)/2。（只断言"分不出"会放过"画成了背景色"。）
    const PaletteLin pal = paletteLinear();
    const float midR = 0.5f * (pal.dark.r + pal.light.r);
    const float midG = 0.5f * (pal.dark.g + pal.light.g);
    const float midB = 0.5f * (pal.dark.b + pal.light.b);
    SR_ASSERT_NEAR(got.r, midR, 0.25f);
    SR_ASSERT_NEAR(got.g, midG, 0.25f);
    SR_ASSERT_NEAR(got.b, midB, 0.25f);
}

SR_TEST(textured_golden_scene_reports_what_it_drew) {
    Canvas c{sr::kGoldenWidth, sr::kGoldenHeight};
    sr::FrameStats stats;
    sr::renderGoldenSceneTextured(c.rt, &stats);
    // 12 个三角形，一般姿态下正面 3 个面 ⇒ 6 个被画、6 个被剔除（Ruling 20 的那套推导）。
    SR_ASSERT_EQ(stats.trianglesSubmitted, 12);
    SR_ASSERT_EQ(stats.trianglesRasterized, 6);
    SR_ASSERT_EQ(stats.trianglesBackfaceCulled, 6);
    SR_ASSERT_EQ(stats.trianglesClippedOut, 0);
    SR_ASSERT(stats.fragmentsWritten > 1000);
    // 深度是开着的：正面之间不互相遮挡 ⇒ 被深度拒掉的应当只有 0（凸体 + 剔除）。
    SR_ASSERT_EQ(stats.fragmentsDepthRejected, 0);
}

SR_TEST(textured_golden_scene_is_deterministic) {
    // 参考图的前提是"同一个函数两次调用给出同一帧"。
    // 一个未初始化的 static、或每帧重建的贴图，都会让 golden 变成随机数发生器。
    Canvas a{sr::kGoldenWidth, sr::kGoldenHeight};
    Canvas b{sr::kGoldenWidth, sr::kGoldenHeight};
    sr::FrameStats sa, sb;
    sr::renderGoldenSceneTextured(a.rt, &sa);
    sr::renderGoldenSceneTextured(b.rt, &sb);
    int diffs = 0;
    for (int y = 0; y < sr::kGoldenHeight && diffs == 0; ++y) {
        for (int x = 0; x < sr::kGoldenWidth; ++x) {
            if (a.rt.color.readPacked(x, y) != b.rt.color.readPacked(x, y)) { ++diffs; break; }
        }
    }
    SR_ASSERT_EQ(diffs, 0);
    SR_ASSERT_EQ(sa.fragmentsWritten, sb.fragmentsWritten);
}

SR_TEST(textured_golden_frame_is_not_the_m3_frame) {
    // 这条守的是"新函数真的接了纹理"，以及"两张参考图不会互相冒充"。
    // 若 renderGoldenSceneTextured 哪天退化成 renderGoldenScene 的一个别名
    // （或者忘了 fu.texture = ...，于是 texturedFS 退回 baseColor），
    // 上面那几条标记角断言会红，但这条能说出"差了多少像素"，归因快得多。
    Canvas m3{sr::kGoldenWidth, sr::kGoldenHeight};
    Canvas m4{sr::kGoldenWidth, sr::kGoldenHeight};
    sr::FrameStats s3, s4;
    sr::renderGoldenScene(m3.rt, &s3);
    sr::renderGoldenSceneTextured(m4.rt, &s4);

    int diffs = 0, hudDiffs = 0;
    for (int y = 0; y < sr::kGoldenHeight; ++y) {
        for (int x = 0; x < sr::kGoldenWidth; ++x) {
            if (m3.rt.color.readPacked(x, y) == m4.rt.color.readPacked(x, y)) continue;
            ++diffs;
            // HUD 在第一行 y∈[3,11)、x∈[3,171)：两张图的标题必须不同（M3 / M4），
            // 否则"拿错参考图"的症状是 golden 全绿。
            if (y < 12 && x < 200) ++hudDiffs;
        }
    }
    std::printf("[golden-tex] 与 M3 帧相差 %d 个像素，其中 HUD 区 %d 个\n", diffs, hudDiffs);
    SR_ASSERT(diffs > 1500);      // 立方体那一块几乎每个像素都换了颜色
    SR_ASSERT(hudDiffs > 0);      // 标题文字确实不是 M3 那一行
}
