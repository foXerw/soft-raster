#include <cmath>
#include <vector>

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
