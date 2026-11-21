#include <cmath>

#include "harness.h"
#include "math/transform.h"
#include "render/builtin_shaders.h"
#include "render/color.h"
#include "render/pipeline.h"
#include "render/render_backend.h"
#include "render/stats.h"
#include "scene/mesh.h"
#include "test_util.h"

namespace {

sr::PipelineState state(Canvas& c, sr::FrameStats& stats) {
    sr::PipelineState st;
    st.target = &c.rt;
    st.stats = &stats;
    st.vertexShader = sr::transformVS;
    st.fragmentShader = sr::vertexColorFS;
    st.cull.backfaceCull = false;
    st.depthTest = false;
    st.depthWrite = false;
    st.vu.model = sr::Mat4::identity();
    st.vu.view = sr::makeLookAt({0, 0, 3}, {0, 0, 0}, {0, 1, 0});
    st.vu.proj = sr::makePerspective(1.047f, 1.0f, 0.1f, 100.0f);
    return st;
}

// 一个正对相机的三角形，三个顶点分别是纯红/纯绿/纯蓝（线性空间）。
sr::Mesh rgbTriangle() {
    sr::Mesh m;
    m.positions = {{-1.2f, -1.2f, 0}, {1.2f, -1.2f, 0}, {0, 1.2f, 0}};
    m.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    m.uvs = {{0, 0}, {1, 0}, {0.5f, 1}};
    m.colors = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};
    m.indices = {0, 1, 2};
    m.recomputeBounds();
    return m;
}

}  // namespace

SR_TEST(centroid_color_is_the_mean_of_vertex_colors_in_linear_space) {
    // 比较必须在线性空间做。若在 sRGB 编码后的字节上取平均，会得到另一个数：
    // 0.5 线性 → 188 sRGB，而三个纯色的 sRGB 平均值 ≠ 平均后的 sRGB 值。
    // 这正是"光照/混合必须在线性空间做"的最小演示，所以它值得单独一条测试。
    Canvas c{128, 128};
    c.rt.color.clear(0xFF000000u);
    c.rt.depth.clear();
    sr::FrameStats stats;
    sr::BackendA backend;
    backend.draw(rgbTriangle(), state(c, stats));

    // 重心（屏幕空间，因为 w 处处相等 ⇒ 屏幕重心 == 世界重心投影）
    const int cx = 64, cy = 79;   // 三角形大致中心；用 w 相等的前提，重心公式可直接在屏幕上取
    const uint32_t got = c.rt.color.readPacked(cx, cy);
    SR_ASSERT(got != 0xFF000000u);   // 先确认这里真的画了东西
    const sr::LinearColor lin = sr::decodeSRGB(got);
    SR_ASSERT_NEAR(lin.r, 1.0f / 3.0f, 0.09f);
    SR_ASSERT_NEAR(lin.g, 1.0f / 3.0f, 0.09f);
    SR_ASSERT_NEAR(lin.b, 1.0f / 3.0f, 0.09f);
}

SR_TEST(color_varies_monotonically_along_an_edge) {
    // 沿底边（红→绿）逐像素检查：红色分量单调下降，绿色单调上升，且相邻差值不为零
    // —— 台阶（banding）的症状是"某一段完全相同，然后突然跳变"。
    Canvas c{128, 128};
    c.rt.color.clear(0xFF000000u);
    c.rt.depth.clear();
    sr::FrameStats stats;
    sr::BackendA backend;
    backend.draw(rgbTriangle(), state(c, stats));

    const int row = 100;   // 靠近底边的一行
    int prevR = -1, prevG = 999;
    int steps = 0;
    for (int x = 0; x < 128; ++x) {
        const uint32_t p = c.rt.color.readPacked(x, row);
        if (p == 0xFF000000u) continue;
        const sr::LinearColor lin = sr::decodeSRGB(p);
        const int r = static_cast<int>(lin.r * 255.0f);
        const int g = static_cast<int>(lin.g * 255.0f);
        if (prevR >= 0) {
            SR_ASSERT(r <= prevR + 1);      // 红单调不升
            SR_ASSERT(g >= prevG - 1);      // 绿单调不降
            if (r != prevR || g != prevG) ++steps;
        }
        prevR = r;
        prevG = g;
    }
    SR_ASSERT(steps > 20);   // 128 像素的渐变里至少应该有 20 次变化
}

SR_TEST(sphere_vertex_colors_have_no_visible_banding) {
    // 位置编码色的球体：赤道上相邻像素的差必须很小。
    // 法线未归一或经纬索引错位时，症状是赤道上出现一条明显的接缝或带状条纹。
    sr::Mesh sphere = sr::makeUVSphere(1.0f, 16, 24);
    sr::applyPositionColors(sphere);

    Canvas c{128, 128};
    c.rt.color.clear(0xFF000000u);
    c.rt.depth.clear();
    sr::FrameStats stats;
    sr::BackendA backend;
    sr::PipelineState st = state(c, stats);
    st.depthTest = true;
    st.depthWrite = true;
    backend.draw(sphere, st);

    const int row = 64;
    int maxDelta = 0;
    uint32_t prev = 0;
    bool havePrev = false;
    int samples = 0;
    for (int x = 20; x < 108; ++x) {
        const uint32_t p = c.rt.color.readPacked(x, row);
        if (p == 0xFF000000u) { havePrev = false; continue; }
        if (havePrev) {
            const int dr = std::abs(static_cast<int>((p >> 16) & 0xFF) - static_cast<int>((prev >> 16) & 0xFF));
            const int dg = std::abs(static_cast<int>((p >> 8) & 0xFF) - static_cast<int>((prev >> 8) & 0xFF));
            const int db = std::abs(static_cast<int>(p & 0xFF) - static_cast<int>(prev & 0xFF));
            if (dr > maxDelta) maxDelta = dr;
            if (dg > maxDelta) maxDelta = dg;
            if (db > maxDelta) maxDelta = db;
            ++samples;
        }
        prev = p;
        havePrev = true;
    }
    SR_ASSERT(samples > 40);
    SR_ASSERT(maxDelta <= 12);   // 一个 sRGB 台阶的可见阈值
}

SR_TEST(fragment_shader_receives_interpolated_not_first_vertex_values) {
    // 这条测试存在的唯一理由：3.2 之前 Backend A 把"第一个顶点的 varying"发给 FS（flat）。
    // 如果那次改动被回退，上面几条里至少这一条会立刻失败 —— 它检查的是
    // 同一个三角形内**两个不同像素的颜色不相同**，与任何具体数值无关。
    Canvas c{128, 128};
    c.rt.color.clear(0xFF000000u);
    sr::FrameStats stats;
    sr::BackendA backend;
    backend.draw(rgbTriangle(), state(c, stats));

    const uint32_t a = c.rt.color.readPacked(40, 100);
    const uint32_t b = c.rt.color.readPacked(90, 100);
    SR_ASSERT(a != 0xFF000000u);
    SR_ASSERT(b != 0xFF000000u);
    SR_ASSERT(a != b);
}
