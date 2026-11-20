#include "render/interpolate.h"

#include <cmath>

#include "harness.h"
#include "math/vec.h"
#include "render/pipeline.h"
#include "render/triangle_setup.h"
#include "test_util.h"

namespace {

sr::ScreenVertex sv(float x, float y, float depth, float w, float u) {
    sr::ScreenVertex s;
    s.pos = sr::Vec2{x, y};
    s.depth = depth;
    s.w = w;
    s.invW = 1.0f / w;
    s.v.uv = sr::Vec2{u, 0};
    s.v.color = sr::Vec4{u, 0, 0, 1};
    return s;
}

// 这些测试的三角形是随手取的顶点顺序，绕向不可控，所以一律关掉剔除：
// 插值的正确性与绕向无关（绕向只决定"画不画"，不决定"画成什么颜色"）。
sr::CullState noCull() {
    sr::CullState c;
    c.backfaceCull = false;
    return c;
}

sr::TriangleSetup makeSetup(const sr::ScreenVertex in[3]) {
    Canvas c{32, 32};
    sr::TriangleSetup s;
    sr::SetupResult res;
    const bool ok = sr::setupTriangle(in, c.rt.color, noCull(), s, res);
    SR_ASSERT(ok);
    return s;
}

}  // namespace

SR_TEST(interpolation_reproduces_vertex_values_for_every_component) {
    const sr::ScreenVertex in[3] = {sv(2, 3, 0.25f, 1.0f, 10), sv(20, 4, 0.5f, 2.0f, 20),
                                    sv(6, 25, 0.75f, 4.0f, 30)};
    const sr::TriangleSetup s = makeSetup(in);
    for (int k = 0; k < 3; ++k) {
        const sr::Interpolated got = sr::interpolateAt(s, in[k].pos.x, in[k].pos.y);
        for (int f = 0; f < sr::kVaryingFloatCount; ++f) {
            SR_ASSERT_NEAR(sr::varyingFloats(got.v)[f], sr::varyingFloats(in[k].v)[f], 1e-3f);
        }
        SR_ASSERT_NEAR(got.depth, in[k].depth, 1e-4f);
        SR_ASSERT_NEAR(got.invW, in[k].invW, 1e-5f);
    }
}

SR_TEST(equal_w_makes_the_centroid_the_arithmetic_mean) {
    // 只有 w 相等时"重心 = 算术平均"才成立。这条测试存在的意义是它**划出了那条边界**：
    // 下一测试会显示 w 不等时它不成立。
    const sr::ScreenVertex in[3] = {sv(2, 2, 0.5f, 1.0f, 10), sv(26, 2, 0.5f, 1.0f, 20),
                                    sv(2, 26, 0.5f, 1.0f, 30)};
    const sr::TriangleSetup s = makeSetup(in);
    const float cx = (2 + 26 + 2) / 3.0f, cy = (2 + 2 + 26) / 3.0f;
    const sr::Interpolated got = sr::interpolateAt(s, cx, cy);
    SR_ASSERT_NEAR(got.v.uv.x, 20.0f, 1e-3f);
}

SR_TEST(perspective_correct_u_at_edge_midpoint) {
    // 本项目最重要的一条数值测试（Review Focus 2）。
    // 三角形：v0 在 w=1 处（近），v1 在 w=10 处（远 10 倍），u 从 0 变到 1。
    //   planeInvW：c = 1，n0 = (0.1 - 1)/10 = -0.09，n1 = 0  ⇒ invW(x) = 1 - 0.09x
    //   planeU   ：c = 0，n0 = (0.1 - 0)/10 = 0.01，n1 = 0   ⇒ uOverW(x) = 0.01x
    // 在边 v0v1 的屏幕中点 x = 5：invW = 0.55，uOverW = 0.05 ⇒ u = 0.05/0.55 = 1/11。
    // 线性（不除 w）插值会给 0.5 —— 差 5.5 倍，所以下面第二条断言证明这条测试有区分力。
    const sr::ScreenVertex in[3] = {sv(0, 0, 0.1f, 1.0f, 0), sv(10, 0, 0.9f, 10.0f, 1),
                                    sv(0, 10, 0.1f, 1.0f, 0)};
    const sr::TriangleSetup s = makeSetup(in);
    const sr::Interpolated got = sr::interpolateAt(s, 5.0f, 0.0f);
    SR_ASSERT_NEAR(got.v.uv.x, 1.0f / 11.0f, 1e-5f);
    SR_ASSERT(std::fabs(got.v.uv.x - 0.5f) > 0.4f);
    SR_ASSERT_NEAR(got.invW, 0.55f, 1e-5f);
}

SR_TEST(depth_is_affine_in_screen_space) {
    // depth = ndcZ*0.5+0.5，而 ndcZ 在屏幕空间里是仿射的（z_view 与 w 成正比），
    // 所以深度**不需要**透视校正：直接用平面方程插。
    // 这条测试的意义是让"深度用线性插值"这件事变成一条被验证的断言，而不是注释里的口头承诺。
    const sr::ScreenVertex in[3] = {sv(2, 2, 0.2f, 1.0f, 0), sv(26, 2, 0.6f, 5.0f, 0),
                                    sv(2, 26, 1.0f, 2.0f, 0)};
    const sr::TriangleSetup s = makeSetup(in);
    const sr::Interpolated mid = sr::interpolateAt(s, 14.0f, 2.0f);
    SR_ASSERT_NEAR(mid.depth, 0.4f, 1e-4f);   // 0.2 与 0.6 的屏幕中点
}

SR_TEST(inv_w_near_zero_is_clamped_and_counted) {
    // 极端 w 会让 invW 趋零，除法放大浮点噪声成 inf。
    // 症状：一小片区域出现 NaN 颜色，顺着 blend 污染一整块 —— 所以必须 clamp 并计数。
    const sr::ScreenVertex in[3] = {sv(0, 0, 0.5f, 1.0f, 0), sv(10, 0, 0.5f, 1e9f, 1),
                                    sv(0, 10, 0.5f, 1.0f, 0)};
    Canvas c{32, 32};
    sr::TriangleSetup s;
    sr::SetupResult res;
    SR_ASSERT(sr::setupTriangle(in, c.rt.color, noCull(), s, res));

    // 采样点取在远顶点上（w = 1e9 ⇒ invW ≈ 1e-9 < 1e-8），才会真的触发夹住。
    // 第一版取 (9.9, 0.1)，那里 invW 还有 0.01，测不到这条分支 —— 夹住计数为 0 才暴露出来。
    int64_t clamped = 0;
    const sr::Interpolated got = sr::interpolateAt(s, 10.0f, 0.0f, &clamped);
    SR_ASSERT(std::isfinite(got.v.uv.x));
    SR_ASSERT(std::isfinite(got.invW));
    SR_ASSERT(clamped >= 1);
}

SR_TEST(interpolate_inv_w_helper_matches_the_full_path) {
    const sr::ScreenVertex in[3] = {sv(1, 1, 0.3f, 2.0f, 5), sv(25, 3, 0.4f, 3.0f, 6),
                                    sv(3, 27, 0.5f, 1.5f, 7)};
    const sr::TriangleSetup s = makeSetup(in);
    const float px = 9.0f, py = 9.0f;
    SR_ASSERT_NEAR(sr::interpolateInvW(s, px, py), sr::interpolateAt(s, px, py).invW, 1e-6f);
}
