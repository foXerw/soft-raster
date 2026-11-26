#include <cmath>
#include <string>
#include <vector>

#include "app/cmdline.h"   // bench_flag 测试：cmdline.cpp 无 Win32（C4），被测试目标直接链接
#include "harness.h"
#include "math/transform.h"
#include "render/builtin_shaders.h"
#include "render/clipping.h"
#include "render/pipeline.h"
#include "render/render_backend.h"
#include "render/stats.h"
#include "scene/mesh.h"
#include "test_util.h"

namespace {

constexpr int kW = 160;
constexpr int kH = 90;

sr::VertexUniforms cubeUniforms(float rx, float ry) {
    sr::VertexUniforms u;
    u.model = sr::Mat4::rotationX(rx) * sr::Mat4::rotationY(ry);
    u.view = sr::makeLookAt({0, 0, 5}, {0, 0, 0}, {0, 1, 0});
    u.proj = sr::makePerspective(1.047f, static_cast<float>(kW) / static_cast<float>(kH), 0.1f, 100.0f);
    sr::Mat4 inv;
    u.normalMatrix = sr::Mat4::tryInvert(u.model, inv) ? inv.transposed().upper3x3AsMat4()
                                                       : sr::Mat4::identity();
    return u;
}

sr::PipelineState makeState(Canvas& c, sr::FrameStats& stats) {
    sr::PipelineState st;
    st.target = &c.rt;
    st.vertexShader = sr::transformVS;
    st.fragmentShader = sr::vertexColorFS;
    st.stats = &stats;
    st.cull.backfaceCull = true;
    st.cull.frontFaceIsClockwise = true;
    st.depthTest = false;
    st.depthWrite = false;
    return st;
}

// Ruling 84（防抖）：计时断言天然跨机器可变 —— 一次 160×90 的 draw 可能不到 1ms，
// lapMs() 的分辨率与调度抖动会让比值毫无意义，那种测试会在负载高的机器上闪红，
// 而闪红的测试教会人忽略红。所以先把 draw 重复到 msTotal ≥ 2.0ms 再判计时断言。
// 100000 次是死循环保险丝（本机实测远不到 100 次就够）。stats 跨 draw 累加，
// 与 merge() 的语义一致；depthTest/depthWrite 关着，重复 draw 不改变覆盖关系。
void drawUntilMeasured(sr::BackendA& backend, const sr::Mesh& cube, sr::PipelineState& st,
                       sr::FrameStats& stats) {
    for (int i = 0; i < 100000 && stats.msTotal < 2.0; ++i) backend.draw(cube, st);
}

}  // namespace

SR_TEST(cube_has_six_distinguishable_face_colors) {
    // painter 缺陷的测试要求"哪个面最后被画"能从像素颜色读出来，
    // 所以面色必须可区分 —— 顶点色因此从 M3 提前到这里落地。
    const sr::Mesh cube = sr::makeCube(2.0f, true);
    SR_ASSERT_EQ(cube.colors.size(), cube.positions.size());
    std::vector<uint32_t> distinct;
    for (size_t i = 0; i + 3 < cube.colors.size(); i += 4) {   // 每面 4 个顶点
        const uint32_t p = sr::encodeSRGB(sr::LinearColor{cube.colors[i].x, cube.colors[i].y,
                                                          cube.colors[i].z});
        bool already = false;
        for (const uint32_t d : distinct) already |= (d == p);
        SR_ASSERT_FALSE(already);
        distinct.push_back(p);
    }
    SR_ASSERT_EQ(static_cast<int>(distinct.size()), 6);
}

SR_TEST(rotated_cube_submits_culls_and_rasterizes_the_expected_counts) {
    Canvas c{kW, kH};
    sr::FrameStats stats;
    const sr::Mesh cube = sr::makeCube(2.0f, true);
    sr::BackendA backend;

    auto st = makeState(c, stats);
    st.vu = cubeUniforms(0.43f, 0.61f);
    c.rt.color.clear(0xFF000000u);
    backend.draw(cube, st);

    SR_ASSERT_EQ(stats.trianglesSubmitted, 12);
    SR_ASSERT_EQ(stats.trianglesBackfaceCulled, 6);
    SR_ASSERT_EQ(stats.trianglesRasterized, 6);
    SR_ASSERT_EQ(stats.trianglesDegenerate, 0);
    SR_ASSERT(stats.fragmentsWritten > 0);

    // 换个姿态重跑，三个数不变（凸体不变式）
    sr::FrameStats stats2;
    auto st2 = makeState(c, stats2);
    st2.vu = cubeUniforms(1.1f, -0.7f);
    backend.draw(cube, st2);
    SR_ASSERT_EQ(stats2.trianglesSubmitted, 12);
    SR_ASSERT_EQ(stats2.trianglesBackfaceCulled, 6);
    SR_ASSERT_EQ(stats2.trianglesRasterized, 6);

    // 剔除关掉：12 个都要进来
    sr::FrameStats stats3;
    auto st3 = makeState(c, stats3);
    st3.vu = cubeUniforms(0.43f, 0.61f);
    st3.cull.backfaceCull = false;
    backend.draw(cube, st3);
    SR_ASSERT_EQ(stats3.trianglesBackfaceCulled, 0);
    SR_ASSERT_EQ(stats3.trianglesRasterized, 12);
}

SR_TEST(axis_aligned_cube_reports_edge_on_faces_as_degenerate_not_backfacing) {
    // 正对姿态下只有 +Z 面朝向相机（2 个三角形），其余 5 个面的外法线都背离相机
    // （4 个侧面的 dot 是 -1 而不是 0 —— 它们不是"棱对"，是"背对"；
    //  第一版以为侧面是棱对、会走 Degenerate，那是正交投影的直觉，透视下不成立）。
    Canvas c{kW, kH};
    sr::FrameStats stats;
    const sr::Mesh cube = sr::makeCube(2.0f, true);
    sr::BackendA backend;
    auto st = makeState(c, stats);
    st.vu = cubeUniforms(0.0f, 0.0f);
    backend.draw(cube, st);

    SR_ASSERT_EQ(stats.trianglesRasterized, 2);      // 只有 +Z 面
    SR_ASSERT_EQ(stats.trianglesBackfaceCulled, 10); // 其余 5 个面 × 2
    SR_ASSERT_EQ(stats.trianglesDegenerate, 0);
}

SR_TEST(no_depth_no_cull_shows_the_painter_artifact) {
    // 与 2.5 的计划条目对应：把"缺陷"pin 住，M3 打开深度后同一场景必须翻转。
    // 场景刻意用**正对姿态 + 关掉剔除**：凸体在剔除开着时本来就不会穿帮
    // （三个可见面只在棱上相接），所以"没有深度就有穿帮"必须靠关掉剔除来演示 ——
    // 这件事本身值得记一笔：**背面剔除本身就是一种遮挡求解**。
    Canvas c{kW, kH};
    sr::FrameStats stats;
    const sr::Mesh cube = sr::makeCube(2.0f, true);
    sr::BackendA backend;

    auto st = makeState(c, stats);
    st.vu = cubeUniforms(0.0f, 0.0f);
    st.cull.backfaceCull = false;
    st.depthTest = false;
    c.rt.color.clear(0xFF000000u);
    backend.draw(cube, st);

    const int cx = kW / 2, cy = kH / 2;
    // 面序固定 +X,-X,+Y,-Y,+Z,-Z：最后画的是 -Z（背面），它盖住了 +Z（正面）。
    // 面 k 占顶点 4k..4k+3，所以 +Z（第 4 面）从 colors[16] 起，-Z（第 5 面）从 colors[20] 起。
    const uint32_t frontColor = sr::encodeSRGB(
        sr::LinearColor{cube.colors[16].x, cube.colors[16].y, cube.colors[16].z});
    const uint32_t backColor = sr::encodeSRGB(
        sr::LinearColor{cube.colors[20].x, cube.colors[20].y, cube.colors[20].z});
    SR_ASSERT(frontColor != backColor);
    SR_ASSERT_EQ(c.rt.color.readPacked(cx, cy), backColor);    // 缺陷被 pin 住
    SR_ASSERT(c.rt.color.readPacked(cx, cy) != frontColor);
}

SR_TEST(camera_inside_the_cube_produces_no_out_of_bounds_or_nan_pixels) {
    // 六面裁剪 + Setup 的联合回归：相机进到模型内部时，"面弹出"的症状
    // 应该表现为大量被裁剪的三角形，而不是越界写入或 NaN 像素。
    Canvas c{kW, kH};
    sr::FrameStats stats;
    const sr::Mesh cube = sr::makeCube(2.0f, true);
    sr::BackendA backend;

    sr::PipelineState st = makeState(c, stats);
    st.vu.model = sr::Mat4::identity();
    st.vu.view = sr::makeLookAt({0, 0, 0}, {0, 0.4f, 0.1f}, {0, 1, 0});
    st.vu.proj = sr::makePerspective(1.047f, static_cast<float>(kW) / static_cast<float>(kH), 0.01f,
                                     100.0f);
    st.cull.backfaceCull = false;
    c.rt.color.clear(0xFF0A0A0Au);
    backend.draw(cube, st);

    // 每个像素要么是背景色，要么是某个面的颜色；不能出现"读越界"的 0（ColorTarget 越界读返回 0）
    int nonBackground = 0;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const uint32_t p = c.rt.color.readPacked(x, y);
            SR_ASSERT(p != 0u);                 // 0 只可能来自越界读，说明有 bug
            if (p != 0xFF0A0A0Au) ++nonBackground;
        }
    }
    SR_ASSERT(nonBackground > 0);
    SR_ASSERT_EQ(stats.trianglesOffscreen, 0);  // 相机在盒子里：不该有三角形完全在外
    SR_ASSERT(stats.trianglesClippedOut >= 0);
}

SR_TEST(varying_reaches_the_fragment_shader_unchanged_for_flat_faces) {
    // M2 阶段 FS 拿到的是"第一个顶点的 varying"（flat），所以一个面内所有像素
    // 必须颜色完全一致。M3 打开插值后这条测试会被"面内颜色渐变"取代 ——
    // 现在把它写下来，是为了让 M3 的改动必须显式地推翻它，而不是悄悄改变行为。
    Canvas c{kW, kH};
    sr::FrameStats stats;
    const sr::Mesh cube = sr::makeCube(2.0f, true);
    sr::BackendA backend;
    auto st = makeState(c, stats);
    st.vu = cubeUniforms(0.0f, 0.0f);
    c.rt.color.clear(0xFF000000u);
    backend.draw(cube, st);

    const uint32_t faceColor = sr::encodeSRGB(
        sr::LinearColor{cube.colors[16].x, cube.colors[16].y, cube.colors[16].z});   // +Z 面
    int seen = 0;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            if (c.rt.color.readPacked(x, y) != 0xFF000000u) {
                SR_ASSERT_EQ(c.rt.color.readPacked(x, y), faceColor);
                ++seen;
            }
    SR_ASSERT(seen > 100);   // 正对姿态下 +Z 面应该盖住相当一片区域
}

// ---- M6.7（D21）：分阶段计时 -------------------------------------------------

SR_TEST(stage_timers_are_written) {
    // §8.1 #7：msVertex/msClip/msSetup/msRaster 在 M0–M6.6 是"声明了、merge() 也求和、
    // 但没人写"的休眠接口（render_backend 只写 msTotal）。一次 draw 之后四个字段必须
    // 全部 > 0。按 R84 重复到 msTotal ≥ 2.0ms 再判（单次 draw 的 lap 可能小于时钟分辨率）。
    Canvas c{kW, kH};
    sr::FrameStats stats;
    const sr::Mesh cube = sr::makeCube(2.0f, true);
    sr::BackendA backend;
    auto st = makeState(c, stats);
    st.vu = cubeUniforms(0.43f, 0.61f);
    drawUntilMeasured(backend, cube, st, stats);
    SR_ASSERT(stats.msTotal >= 2.0);   // R84 前提：时间不够时下面的断言没有意义
    SR_ASSERT(stats.msVertex > 0.0);
    SR_ASSERT(stats.msClip > 0.0);
    SR_ASSERT(stats.msSetup > 0.0);
    SR_ASSERT(stats.msRaster > 0.0);
}

SR_TEST(stage_timers_partition_the_total) {
    // 本任务的牙齿：只断言 > 0 挡不住"四个字段都写同一个数"的糊弄写法（比如各写一份
    // msTotal ⇒ 求和 = 4×msTotal）。求和与 msTotal 的关系才证明它们**划分**了工作：
    // 四个 lap 来自同一只秒表（lapMs 读一次时钟并重启），求和 telescoping 成"最后一次
    // lap 的时刻 − 起点"，必然 ≤ msTotal；下界 0.5 容忍机器抖动与未归段的循环开销。
    // 实测比值（本机 Intel Core Ultra 9 275HX，2026-11-26，各 3 次运行）：
    //   MSVC Debug 0.9996–0.9997；g++ (MSYS2 UCRT64, -O0) Debug 0.9996–0.9997；
    //   MSVC /O2 Release 0.9994–0.9997。
    // 四段份额（Debug MSVC）：VS ~2% / clip ~1.5% / setup ~3.5% / raster ~93%。
    // 删掉任一段的写入 ⇒ 本条与 stage_timers_are_written 都红在"该段 > 0"（实测：
    // 删 msRaster 时比值断言仍绿 —— 未 lap 的时间大多流进后续 lap，比值抓不到它）。
    // 比值断言独有的牙齿是"四段写同一个数"（各写一份 msTotal ⇒ ratio≈4，而各段 > 0
    // 全绿）这类糊弄，见 M6.7 报告的变异表。
    Canvas c{kW, kH};
    sr::FrameStats stats;
    const sr::Mesh cube = sr::makeCube(2.0f, true);
    sr::BackendA backend;
    auto st = makeState(c, stats);
    st.vu = cubeUniforms(0.43f, 0.61f);
    drawUntilMeasured(backend, cube, st, stats);
    SR_ASSERT(stats.msTotal >= 2.0);
    SR_ASSERT(stats.msVertex > 0.0);
    SR_ASSERT(stats.msClip > 0.0);
    SR_ASSERT(stats.msSetup > 0.0);
    SR_ASSERT(stats.msRaster > 0.0);
    const double sum = stats.msVertex + stats.msClip + stats.msSetup + stats.msRaster;
    const double ratio = sum / stats.msTotal;
    SR_ASSERT(ratio >= 0.5);
    SR_ASSERT(ratio <= 1.05);
}

SR_TEST(bench_flag_runs_the_requested_number_of_frames) {
    // D21：--bench 60 曾经只设 bench/offscreen 而**不设 frames** ⇒ 实际只跑 1 帧，
    // 终端还打印 M0 占位文案。"我以为在测 60 帧"比没有测量更糟。
    const char* argv[] = {"soft-raster", "--bench", "60"};
    app::AppOptions o;
    std::string err;
    const bool ok = app::parseCmdline(3, const_cast<char**>(argv), o, err);
    SR_ASSERT(ok);
    SR_ASSERT_EQ(o.bench, 60);
    SR_ASSERT_EQ(o.frames, 60);   // 修复前这里是 1 ⇒ RED 在帧数
    SR_ASSERT(o.offscreen);
}

SR_TEST(stage_timers_accumulate_over_a_clipped_polygon_fan) {
    // P109：扇形重三角化会**多次**进入 rasterOne，msSetup/msRaster 用 `+=` 累加。
    // 立方体夹具每个输入三角形只产生一个子三角形，`=` 与 `+=` 不可区分（只有读码验证）。
    // 这里造一个被裁成 >=5 顶点凸多边形的单三角形（>=3 个子三角形），让累加真被跑到：
    // 若改成 `=`（覆盖），msSetup/msRaster 只记最后一个子三角形 ⇒ 四段之和远小于 msTotal
    // ⇒ partition 比值掉到 0.5 以下 ⇒ 红。前提先断言子三角形数 >=3。
    // 判别力主要在 msRaster（占 ~99%）：实测**只**把 msSetup 一处改 `=` 比值不崩（setup 份额
    // 仅 ~3.5%，丢前面子三角形的 setup 不足以跌破 0.5）；把 msRaster（或两处）改 `=` 才崩。
    // 真实的"+= 改 ="重构触两处，被杀；单改 msSetup 不红是份额使然，不是测试漏（M6.9 评审 Minor 3 补注）。
    sr::Mesh tri;
    tri.positions = {sr::Vec3{-10, -10, -5}, sr::Vec3{10, -10, -5}, sr::Vec3{0, 10, -5}};
    tri.indices = {0, 1, 2};
    tri.recomputeBounds();

    Canvas c{kW, kH};
    sr::BackendA backend;
    auto setup = [&](sr::FrameStats& st) {
        auto s = makeState(c, st);
        s.vu.model = sr::Mat4::identity();
        s.vu.view = sr::makeLookAt({0, 0, 0}, {0, 0, -1}, {0, 1, 0});
        s.vu.proj = sr::makePerspective(1.047f, static_cast<float>(kW) / static_cast<float>(kH),
                                        0.1f, 100.0f);
        s.vu.normalMatrix = sr::Mat4::identity();
        s.cull.backfaceCull = false;   // 裁剪多边形的每个子三角形都要进来
        return s;
    };

    // 前提：一次 draw 真的产生 >=3 个被光栅化的子三角形（否则本测试对 `=` 无判别力）。
    // 实测（MSVC Debug，本机）：该三角形被裁成 **6 顶点**凸多边形 ⇒ **4 个子三角形**、
    // 13200 片元 ⇒ 累加路径真被跑到。夹具密度/形状是量出来的，改几何须重测这条前提。
    sr::FrameStats count;
    auto sc = setup(count);
    backend.draw(tri, sc);
    SR_ASSERT(count.trianglesRasterized >= 3);

    sr::FrameStats stats;
    auto st = setup(stats);
    drawUntilMeasured(backend, tri, st, stats);
    SR_ASSERT(stats.msTotal >= 2.0);
    SR_ASSERT(stats.msVertex > 0.0);
    SR_ASSERT(stats.msClip > 0.0);
    SR_ASSERT(stats.msSetup > 0.0);
    SR_ASSERT(stats.msRaster > 0.0);
    const double sum = stats.msVertex + stats.msClip + stats.msSetup + stats.msRaster;
    const double ratio = sum / stats.msTotal;
    SR_ASSERT(ratio >= 0.5);   // `=` 覆盖会丢前面子三角形的段 ⇒ 比值崩 ⇒ 红
    SR_ASSERT(ratio <= 1.05);
}
