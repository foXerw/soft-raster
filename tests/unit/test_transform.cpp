#include "math/transform.h"

#include <cmath>

#include "harness.h"
#include "math/vec.h"

namespace {

constexpr float kPi = 3.14159265358979323846f;

void expectVec3Near(const sr::Vec3& a, float x, float y, float z, float tol) {
    SR_ASSERT_NEAR(a.x, x, tol);
    SR_ASSERT_NEAR(a.y, y, tol);
    SR_ASSERT_NEAR(a.z, z, tol);
}

}  // namespace

SR_TEST(lookAt_puts_the_eye_at_the_origin) {
    const sr::Mat4 v = sr::makeLookAt({0, 0, 5}, {0, 0, 0}, {0, 1, 0});
    expectVec3Near(sr::xyz(v * sr::makeVec4({0, 0, 5}, 1.0f)), 0, 0, 0, 1e-5f);
}

SR_TEST(lookAt_looks_along_minus_z) {
    const sr::Mat4 v = sr::makeLookAt({0, 0, 5}, {0, 0, 0}, {0, 1, 0});
    // 世界原点（相机正前方 5 米）在视图空间里是 (0, 0, -5)：右手系、朝 -z 看。
    expectVec3Near(sr::xyz(v * sr::makeVec4({0, 0, 0}, 1.0f)), 0, 0, -5, 1e-5f);
    // up 保持朝上：世界 {0,1,0} 在相机正前方 5 米、高 1 米处
    // （注意别写成 {0,1,5}：那个点与 eye 同深度，是在相机**旁边**，视图 z 会是 0）
    expectVec3Near(sr::xyz(v * sr::makeVec4({0, 1, 0}, 1.0f)), 0, 1, -5, 1e-5f);
    // 相机右侧（世界 +x）在视图空间里是 +x，同样取正前方 5 米的点
    expectVec3Near(sr::xyz(v * sr::makeVec4({1, 0, 0}, 1.0f)), 1, 0, -5, 1e-5f);
    // 而"旁边"的点视图 z 必须是 0：这条把上面那个容易写错的区分钉住
    expectVec3Near(sr::xyz(v * sr::makeVec4({0, 1, 5}, 1.0f)), 0, 1, 0, 1e-5f);
}

SR_TEST(lookAt_is_a_rigid_transform_for_orthonormal_up) {
    // 视图矩阵只做旋转 + 平移，所以两点之间的距离必须保持不变。
    const sr::Mat4 v = sr::makeLookAt({3, 2, 7}, {0, 1, 0}, {0, 1, 0});
    const sr::Vec3 a = sr::xyz(v * sr::makeVec4({1, 0, 0}, 1.0f));
    const sr::Vec3 b = sr::xyz(v * sr::makeVec4({0, 0, 1}, 1.0f));
    const float dView = sr::length(a - b);
    const float dWorld = sr::length(sr::Vec3{1, 0, 0} - sr::Vec3{0, 0, 1});
    SR_ASSERT_NEAR(dView, dWorld, 1e-5f);
}

SR_TEST(lookAt_with_degenerate_up_stays_finite) {
    // up 与视线平行时 cross 为零。约定：退回一个可用的 up，而不是让 NaN 进矩阵。
    // 症状对照：NaN 进 view 矩阵 ⇒ 整个场景消失，而且没有任何报错。
    const sr::Mat4 v = sr::makeLookAt({0, 5, 0}, {0, 0, 0}, {0, 1, 0});
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) SR_ASSERT(std::isfinite(v.m[r][c]));
    // 退化情况下仍然要把 eye 放到原点
    expectVec3Near(sr::xyz(v * sr::makeVec4({0, 5, 0}, 1.0f)), 0, 0, 0, 1e-5f);
}

SR_TEST(perspective_maps_near_and_far_to_ndc_z_bounds) {
    const sr::Mat4 p = sr::makePerspective(kPi / 2.0f, 1.0f, 1.0f, 10.0f);
    // 近平面中心：clip.z == -clip.w（NDC z = -1）
    const sr::Vec4 nearC = p * sr::Vec4{0, 0, -1, 1};
    SR_ASSERT_NEAR(nearC.z, -nearC.w, 1e-5f);
    SR_ASSERT_NEAR(nearC.w, 1.0f, 1e-5f);
    // 远平面中心：clip.z == +clip.w（NDC z = +1）
    const sr::Vec4 farC = p * sr::Vec4{0, 0, -10, 1};
    SR_ASSERT_NEAR(farC.z, farC.w, 1e-5f);
    SR_ASSERT_NEAR(farC.w, 10.0f, 1e-5f);
    // 中间某个深度必须落在两者之间，且 w 随距离增长（透视除法的来源）
    const sr::Vec4 midC = p * sr::Vec4{0, 0, -3, 1};
    SR_ASSERT(midC.z / midC.w > -1.0f && midC.z / midC.w < 1.0f);
    SR_ASSERT(midC.w > nearC.w && midC.w < farC.w);
}

SR_TEST(perspective_maps_frustum_edges_to_ndc_one) {
    const sr::Mat4 p = sr::makePerspective(kPi / 2.0f, 1.0f, 1.0f, 10.0f);
    // fovY = 90° ⇒ 近平面上 y = ±1 的点映到 NDC y = ±1
    const sr::Vec4 top = p * sr::Vec4{0, 1, -1, 1};
    SR_ASSERT_NEAR(top.y / top.w, 1.0f, 1e-5f);
    const sr::Vec4 bottom = p * sr::Vec4{0, -1, -1, 1};
    SR_ASSERT_NEAR(bottom.y / bottom.w, -1.0f, 1e-5f);
    // aspect = 1 ⇒ x 方向同样
    const sr::Vec4 right = p * sr::Vec4{1, 0, -1, 1};
    SR_ASSERT_NEAR(right.x / right.w, 1.0f, 1e-5f);
}

SR_TEST(perspective_aspect_halves_the_horizontal_range) {
    const sr::Mat4 p = sr::makePerspective(kPi / 2.0f, 2.0f, 1.0f, 10.0f);
    // aspect = 2 ⇒ 水平视野是垂直的两倍：近平面上 x = ±2 才到 NDC x = ±1
    const sr::Vec4 right = p * sr::Vec4{2, 0, -1, 1};
    SR_ASSERT_NEAR(right.x / right.w, 1.0f, 1e-5f);
    const sr::Vec4 half = p * sr::Vec4{1, 0, -1, 1};
    SR_ASSERT_NEAR(half.x / half.w, 0.5f, 1e-5f);
}

SR_TEST(perspective_fov_shrinks_the_y_scale_monotonically) {
    const float s30 = sr::makePerspective(kPi / 6.0f, 1.0f, 1.0f, 10.0f).m[1][1];
    const float s60 = sr::makePerspective(kPi / 3.0f, 1.0f, 1.0f, 10.0f).m[1][1];
    const float s90 = sr::makePerspective(kPi / 2.0f, 1.0f, 1.0f, 10.0f).m[1][1];
    SR_ASSERT(s30 > s60);
    SR_ASSERT(s60 > s90);
    SR_ASSERT_NEAR(s90, 1.0f, 1e-6f);   // fovY=90° ⇒ cot(45°) = 1
}

SR_TEST(perspective_rejects_non_positive_planes) {
    // 约定：near <= 0 或 far <= near 直接返回单位阵（调用方会看到"什么都没投影"，
    // 比除零产生的 inf/NaN 好定位得多）。
    const sr::Mat4 bad = sr::makePerspective(kPi / 2.0f, 1.0f, 0.0f, 10.0f);
    SR_ASSERT_NEAR(bad.m[0][0], 1.0f, 1e-6f);
    SR_ASSERT_NEAR(bad.m[2][3], 0.0f, 1e-6f);
    const sr::Mat4 flipped = sr::makePerspective(kPi / 2.0f, 1.0f, 5.0f, 5.0f);
    SR_ASSERT_NEAR(flipped.m[0][0], 1.0f, 1e-6f);
}

SR_TEST(view_projection_chain_puts_a_point_in_front_of_camera_at_center) {
    // 端到端：view * proj 之后，相机正前方的点落在 NDC 中心，深度在 (0,1)。
    const sr::Mat4 vp = sr::makePerspective(kPi / 3.0f, 16.0f / 9.0f, 0.1f, 100.0f) *
                        sr::makeLookAt({0, 0, 4}, {0, 0, 0}, {0, 1, 0});
    const sr::Vec4 c = vp * sr::Vec4{0, 0, 0, 1};
    SR_ASSERT_NEAR(c.x / c.w, 0.0f, 1e-5f);
    SR_ASSERT_NEAR(c.y / c.w, 0.0f, 1e-5f);
    const float ndcZ = c.z / c.w;
    SR_ASSERT(ndcZ > -1.0f && ndcZ < 1.0f);
    // 相机后面的点 w < 0：裁剪必须把它丢掉（M2 的六面裁剪会用到这个性质）
    const sr::Vec4 behind = vp * sr::Vec4{0, 0, 8, 1};
    SR_ASSERT(behind.w < 0.0f);
}
