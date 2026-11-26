#include "render/plane_eq.h"

#include <cmath>
#include <limits>

#include "harness.h"
#include "math/vec.h"

using sr::EdgeFunc;
using sr::PlaneEq;
using sr::Vec2;

SR_TEST(plane_eq_reproduces_the_three_vertex_values) {
    const Vec2 p0{2, 3}, p1{9, 1}, p2{5, 8};
    const PlaneEq f = PlaneEq::fromTriangle(p0, 10.0f, p1, 20.0f, p2, 30.0f);
    SR_ASSERT_NEAR(f.sample(p0.x, p0.y), 10.0f, 1e-4f);
    SR_ASSERT_NEAR(f.sample(p1.x, p1.y), 20.0f, 1e-4f);
    SR_ASSERT_NEAR(f.sample(p2.x, p2.y), 30.0f, 1e-4f);
}

SR_TEST(plane_eq_is_affine_so_the_centroid_is_the_average) {
    const Vec2 p0{0, 0}, p1{6, 0}, p2{0, 6};
    const PlaneEq f = PlaneEq::fromTriangle(p0, 1.0f, p1, 2.0f, p2, 3.0f);
    const Vec2 centroid{2, 2};
    SR_ASSERT_NEAR(f.sample(centroid.x, centroid.y), 2.0f, 1e-5f);
    // 沿一条边的中点：属性必须是两端点的算术平均
    SR_ASSERT_NEAR(f.sample(3.0f, 0.0f), 1.5f, 1e-5f);
    SR_ASSERT_NEAR(f.sample(0.0f, 3.0f), 2.0f, 1e-5f);
}

SR_TEST(plane_eq_on_a_degenerate_triangle_is_reported_not_solved) {
    // 共线三点无法确定一个平面。约定：返回 false 语义由调用方处理，
    // 这里用 isDegenerate 显式表达，而不是让 n0/n1 变成 inf。
    const Vec2 a{0, 0}, b{2, 2}, c{4, 4};
    const PlaneEq f = PlaneEq::fromTriangle(a, 1.0f, b, 2.0f, c, 3.0f);
    SR_ASSERT(f.degenerate);
    SR_ASSERT(std::isfinite(f.n0) && std::isfinite(f.n1) && std::isfinite(f.c));
}

SR_TEST(plane_eq_on_a_nan_triangle_is_degenerate_not_nan) {
    // 这条是给 M4.7 的守卫重写做**钉住**用的：为了压掉 MSVC 的 C4723（potential divide by 0），
    // 最省事的改法是把 `!(fabs(det) > eps)` 写成 `fabs(det) < eps` —— 但那样 NaN 会**漏过去**
    // 做除法，症状是一个 NaN 像素顺着 blend 污染一整块区域。
    // 所以"NaN 必须走退化分支"要有一条测试钉住，否则压告警的那次改动会静默改掉语义。
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const PlaneEq f = PlaneEq::fromTriangle(Vec2{nan, 0.0f}, 1.0f, Vec2{1.0f, 1.0f}, 2.0f,
                                            Vec2{0.0f, 1.0f}, 3.0f);
    SR_ASSERT(f.degenerate);
    SR_ASSERT(std::isfinite(f.n0) && std::isfinite(f.n1) && std::isfinite(f.c));
    SR_ASSERT(std::isfinite(f.sample(0.5f, 0.5f)));
}

SR_TEST(edge_function_is_positive_inside_for_a_clockwise_triangle) {
    // 这个夹具 (0,0)-(10,0)-(0,10) 视觉上顺时针 ⇒ area2 > 0 ⇒ 按 triangle_setup.cpp:49 它是**背面**；
    // 但边函数与正反面无关：fromEdge 默认 n = {-e.y, e.x}（plane_eq.h:52）让这个绕向的内部恒为正。
    const Vec2 v0{0, 0}, v1{10, 0}, v2{0, 10};      // 视觉上顺时针（y 朝下）
    const EdgeFunc e01 = EdgeFunc::fromEdge(v0, v1);
    const EdgeFunc e12 = EdgeFunc::fromEdge(v1, v2);
    const EdgeFunc e20 = EdgeFunc::fromEdge(v2, v0);
    const Vec2 inside{2, 2};
    SR_ASSERT(e01.evaluate(inside) >= 0.0f);
    SR_ASSERT(e12.evaluate(inside) >= 0.0f);
    SR_ASSERT(e20.evaluate(inside) >= 0.0f);

    // 每条边外侧的点必须让**对应的那一条**变负。注意边的编号：
    // e01 是顶边、e12 是斜边（v1→v2）、e20 是左边（v2→v0）。写错编号的症状是
    // "测试失败但实现是对的"，这在本任务的第一版就发生过。
    SR_ASSERT(e12.evaluate(Vec2{9, 9}) < 0.0f);     // 越过斜边 x+y=10
    SR_ASSERT(e01.evaluate(Vec2{5, -1}) < 0.0f);    // 越过顶边 y=0
    SR_ASSERT(e20.evaluate(Vec2{-1, 5}) < 0.0f);    // 越过左边 x=0
}

SR_TEST(edge_function_is_exactly_zero_on_the_edge_line) {
    const EdgeFunc e = EdgeFunc::fromEdge(Vec2{1, 2}, Vec2{7, 9});
    SR_ASSERT_NEAR(e.evaluate(Vec2{1, 2}), 0.0f, 1e-6f);
    SR_ASSERT_NEAR(e.evaluate(Vec2{7, 9}), 0.0f, 1e-6f);
    SR_ASSERT_NEAR(e.evaluate(Vec2{4, 5.5f}), 0.0f, 1e-5f);   // 线段中点
}

SR_TEST(edge_function_classifies_top_and_left_edges) {
    // 顶边：水平，且第三顶点在它**下方**（top-down 里 y 更大）。
    const EdgeFunc top = EdgeFunc::fromEdge(Vec2{0, 0}, Vec2{4, 0});
    SR_ASSERT(top.edgeIsTopOrLeft());
    // 同一条边如果第三顶点在上方，就不是顶边。
        // 同一条边、反向遍历 ⇒ 内侧法线朝上 ⇒ 不再是顶边
    const EdgeFunc topRev = EdgeFunc::fromEdge(Vec2{4, 0}, Vec2{0, 0});
    SR_ASSERT_FALSE(topRev.edgeIsTopOrLeft());
    // 底边（水平但第三顶点在上方）不算 top。
    const EdgeFunc bottom = EdgeFunc::fromEdge(Vec2{4, 4}, Vec2{0, 4});
    SR_ASSERT_FALSE(bottom.edgeIsTopOrLeft());
}
