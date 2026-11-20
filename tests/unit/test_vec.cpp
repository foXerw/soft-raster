#include "math/vec.h"

#include <cmath>

#include "harness.h"

using sr::Vec2;
using sr::Vec3;
using sr::Vec4;

SR_TEST(vec3_dot_matches_hand_values) {
    SR_ASSERT_NEAR(sr::dot(Vec3{1, 2, 3}, Vec3{4, -5, 6}), 4 - 10 + 18.0f, 1e-6f);
    SR_ASSERT_NEAR(sr::dot(Vec3{1, 0, 0}, Vec3{0, 1, 0}), 0.0f, 1e-6f);
    SR_ASSERT_NEAR(sr::dot(Vec3{2, 0, 0}, Vec3{3, 0, 0}), 6.0f, 1e-6f);
}

SR_TEST(vec3_cross_follows_the_right_hand_rule) {
    const Vec3 x = sr::cross(Vec3{1, 0, 0}, Vec3{0, 1, 0});
    SR_ASSERT_NEAR(x.x, 0.0f, 1e-6f);
    SR_ASSERT_NEAR(x.y, 0.0f, 1e-6f);
    SR_ASSERT_NEAR(x.z, 1.0f, 1e-6f);
    // 反序必须反号：这一条能抓住"叉积公式写成了转置"的错。
    const Vec3 y = sr::cross(Vec3{0, 1, 0}, Vec3{1, 0, 0});
    SR_ASSERT_NEAR(y.z, -1.0f, 1e-6f);
    // 与自身垂直性：cross 的结果必须与两个输入都正交。
    const Vec3 a{0.3f, -0.7f, 1.2f}, b{2.1f, 0.4f, -0.9f};
    const Vec3 c = sr::cross(a, b);
    SR_ASSERT_NEAR(sr::dot(c, a), 0.0f, 1e-5f);
    SR_ASSERT_NEAR(sr::dot(c, b), 0.0f, 1e-5f);
}

SR_TEST(vec2_cross_scalar_returns_the_z_component) {
    // 屏幕空间的边函数要用它：结果的正负就是"点在边的哪一侧"。
    SR_ASSERT_NEAR(sr::cross(Vec2{1, 0}, Vec2{0, 1}), 1.0f, 1e-6f);
    SR_ASSERT_NEAR(sr::cross(Vec2{0, 1}, Vec2{1, 0}), -1.0f, 1e-6f);
    SR_ASSERT_NEAR(sr::cross(Vec2{2, 3}, Vec2{4, 5}), 2 * 5 - 3 * 4.0f, 1e-6f);
}

SR_TEST(normalize_produces_unit_length) {
    const Vec3 n = sr::normalize(Vec3{3, 0, 0});
    SR_ASSERT_NEAR(n.x, 1.0f, 1e-6f);
    SR_ASSERT_NEAR(sr::length(Vec3{1, 2, 2}), 3.0f, 1e-6f);
    SR_ASSERT_NEAR(sr::lengthSquared(Vec3{1, 2, 2}), 9.0f, 1e-6f);
    const Vec3 d = sr::normalize(Vec3{0.5f, -0.25f, 0.125f});
    SR_ASSERT_NEAR(sr::length(d), 1.0f, 1e-6f);
}

SR_TEST(normalize_of_zero_vector_is_zero_not_nan) {
    // 这条约定是接口的一部分：热路径里 NaN 会顺着插值污染一整块区域，
    // 而且症状是"某个区域偶发变黑"，几乎不可能定位回一次退化法线。
    const Vec3 z = sr::normalize(Vec3{0, 0, 0});
    SR_ASSERT_NEAR(z.x, 0.0f, 0.0f);
    SR_ASSERT_NEAR(z.y, 0.0f, 0.0f);
    SR_ASSERT_NEAR(z.z, 0.0f, 0.0f);
    SR_ASSERT(std::isfinite(z.x) && std::isfinite(z.y) && std::isfinite(z.z));
    // 极短向量也走同一条路，避免放大浮点噪声。
    const Vec3 tiny = sr::normalize(Vec3{1e-12f, 0, 0});
    SR_ASSERT(std::isfinite(tiny.x));
}

SR_TEST(arithmetic_operators_componentwise_and_scalar) {
    const Vec3 a{1, 2, 3}, b{10, -20, 30};
    const Vec3 sum = a + b;
    SR_ASSERT_NEAR(sum.x, 11.0f, 1e-6f);
    SR_ASSERT_NEAR(sum.y, -18.0f, 1e-6f);
    SR_ASSERT_NEAR(sum.z, 33.0f, 1e-6f);

    const Vec3 diff = b - a;
    SR_ASSERT_NEAR(diff.x, 9.0f, 1e-6f);
    SR_ASSERT_NEAR(diff.z, 27.0f, 1e-6f);

    const Vec3 scaled = a * 2.0f;
    SR_ASSERT_NEAR(scaled.y, 4.0f, 1e-6f);
    const Vec3 prescaled = 2.0f * a;      // 标量在左边也要能用，否则着色代码写起来很别扭
    SR_ASSERT_NEAR(prescaled.y, 4.0f, 1e-6f);

    const Vec3 divided = b / 10.0f;
    SR_ASSERT_NEAR(divided.y, -2.0f, 1e-6f);

    const Vec3 neg = -a;
    SR_ASSERT_NEAR(neg.x, -1.0f, 1e-6f);

    const Vec3 hadamard = a * b;         // 分量乘积：调制纹理时要用
    SR_ASSERT_NEAR(hadamard.x, 10.0f, 1e-6f);
    SR_ASSERT_NEAR(hadamard.y, -40.0f, 1e-6f);
}

SR_TEST(vec2_and_vec4_have_the_same_shape_of_api) {
    const Vec2 s = Vec2{1, 2} + Vec2{3, 4};   // 注意：`Vec2 s{1,2} + ...` 会被解析成声明
    SR_ASSERT_NEAR(s.x, 4.0f, 1e-6f);
    SR_ASSERT_NEAR(s.y, 6.0f, 1e-6f);
    SR_ASSERT_NEAR(sr::dot(Vec2{1, 2}, Vec2{3, 4}), 11.0f, 1e-6f);

    const Vec4 p{1, 2, 3, 4};
    const Vec4 q = p * 2.0f;
    SR_ASSERT_NEAR(q.w, 8.0f, 1e-6f);
    SR_ASSERT_NEAR(sr::dot(p, q), 2 + 8 + 18 + 32.0f, 1e-6f);
}

SR_TEST(lerp_endpoints_and_midpoint) {
    const Vec3 a{1, 2, 3}, b{5, 6, 7};
    const Vec3 lo = sr::lerp(a, b, 0.0f);
    SR_ASSERT_NEAR(lo.x, 1.0f, 1e-6f);
    SR_ASSERT_NEAR(lo.z, 3.0f, 1e-6f);
    const Vec3 hi = sr::lerp(a, b, 1.0f);
    SR_ASSERT_NEAR(hi.x, 5.0f, 1e-6f);
    SR_ASSERT_NEAR(hi.z, 7.0f, 1e-6f);
    const Vec3 mid = sr::lerp(a, b, 0.5f);
    SR_ASSERT_NEAR(mid.x, 3.0f, 1e-6f);
    SR_ASSERT_NEAR(mid.y, 4.0f, 1e-6f);

    const Vec2 m2 = sr::lerp(Vec2{0, 0}, Vec2{4, 8}, 0.25f);
    SR_ASSERT_NEAR(m2.x, 1.0f, 1e-6f);
    SR_ASSERT_NEAR(m2.y, 2.0f, 1e-6f);

    const Vec4 m4 = sr::lerp(Vec4{0, 0, 0, 0}, Vec4{2, 4, 6, 8}, 0.5f);
    SR_ASSERT_NEAR(m4.w, 4.0f, 1e-6f);
}

SR_TEST(make_vec4_appends_w) {
    const Vec4 v = sr::makeVec4(Vec3{1, 2, 3}, 1.0f);   // 位置：w=1
    SR_ASSERT_NEAR(v.x, 1.0f, 1e-6f);
    SR_ASSERT_NEAR(v.w, 1.0f, 1e-6f);
    const Vec4 d = sr::makeVec4(Vec3{1, 2, 3}, 0.0f);   // 方向：w=0，平移不该影响它
    SR_ASSERT_NEAR(d.w, 0.0f, 1e-6f);
}

SR_TEST(vec3_xyz_helpers_from_vec4) {
    const Vec3 p = sr::perspectiveDivide(Vec4{2, 4, 6, 2});
    SR_ASSERT_NEAR(p.x, 1.0f, 1e-6f);
    SR_ASSERT_NEAR(p.y, 2.0f, 1e-6f);
    SR_ASSERT_NEAR(p.z, 3.0f, 1e-6f);
}
