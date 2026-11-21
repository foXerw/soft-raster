#include "scene/camera.h"

#include <cmath>

#include "harness.h"
#include "math/vec.h"

namespace {
constexpr float kPi = 3.14159265358979323846f;
}

SR_TEST(orbit_camera_default_eyes_are_on_a_sphere) {
    sr::OrbitCamera c;
    c.distance = 5.0f;
    const sr::Vec3 e = c.eye();
    SR_ASSERT_NEAR(sr::length(e - c.target), 5.0f, 1e-5f);
    // 默认 pitch 是 0.35 而不是 0，所以 z = d·cos(pitch)：
    // 断言里忽略 pitch 会把"正确的实现"判成错的（本任务第一版就错了）。
    c.pitch = 0.0f;
    SR_ASSERT_NEAR(c.eye().z, 5.0f, 1e-5f);
}

SR_TEST(yaw_90_moves_the_eye_to_the_x_axis) {
    sr::OrbitCamera c;
    c.distance = 4.0f;
    c.yaw = kPi / 2.0f;
    c.pitch = 0.0f;
    SR_ASSERT_NEAR(c.eye().x, 4.0f, 1e-5f);
    SR_ASSERT_NEAR(c.eye().z, 0.0f, 1e-5f);
}

SR_TEST(pitch_is_clamped_so_the_camera_never_flips) {
    sr::OrbitCamera c;
    for (int i = 0; i < 10000; ++i) c.orbit(0.0f, 10.0f);
    SR_ASSERT(c.pitch <= 1.5534f);
    SR_ASSERT_NEAR(c.eye().y, c.distance * std::sin(c.pitch), 1e-4f);
    // 极点附近 up 与视线接近平行，view 矩阵不该出现非有限值
    const sr::Mat4 v = c.view();
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k) SR_ASSERT(std::isfinite(v.m[r][k]));
}

SR_TEST(view_matrix_puts_the_eye_at_the_origin_and_target_at_minus_distance) {
    sr::OrbitCamera c;
    c.yaw = 0.6f;
    c.pitch = -0.2f;
    c.distance = 3.0f;
    const sr::Mat4 v = c.view();
    const sr::Vec3 atEye = sr::xyz(v * sr::makeVec4(c.eye(), 1.0f));
    SR_ASSERT_NEAR(atEye.x, 0.0f, 1e-4f);
    SR_ASSERT_NEAR(atEye.y, 0.0f, 1e-4f);
    SR_ASSERT_NEAR(atEye.z, 0.0f, 1e-4f);
    const sr::Vec3 atTarget = sr::xyz(v * sr::makeVec4(c.target, 1.0f));
    SR_ASSERT_NEAR(atTarget.z, -3.0f, 1e-4f);
}

SR_TEST(dolly_is_multiplicative_and_bounded) {
    sr::OrbitCamera c;
    c.distance = 10.0f;
    c.dolly(1.0f);
    SR_ASSERT_NEAR(c.distance, 9.0f, 1e-5f);
    c.dolly(-1.0f);
    SR_ASSERT_NEAR(c.distance, 10.0f, 1e-4f);
    for (int i = 0; i < 200; ++i) c.dolly(1.0f);
    SR_ASSERT(c.distance >= 0.05f);
    for (int i = 0; i < 400; ++i) c.dolly(-1.0f);
    SR_ASSERT(c.distance <= 1000.0f);
}

SR_TEST(dragging_right_turns_the_object_right) {
    // 手感方向：往右拖 ⇒ 物体向右转 ⇒ 相机绕到物体左侧 ⇒ yaw **减小**。
    // 第一版写成 yaw += dx，方向是反的（产品方反馈"拖动方向和鼠标方向相反"）。
    // 这条断言之所以重要：它不能靠"跟着实现写"来通过，必须靠上面那段投影验证。
    sr::OrbitCamera c;
    c.yaw = 0.0f;
    c.orbit(100.0f, 0.0f);
    SR_ASSERT(c.yaw < 0.0f);
    SR_ASSERT(c.eye().x < 0.0f);        // 相机到了 -x 一侧
    SR_ASSERT_NEAR(c.eye().x, c.distance * std::cos(c.pitch) * std::sin(c.yaw), 1e-5f);
}

SR_TEST(dragging_down_brings_the_top_toward_the_viewer) {
    // 垂直轴与水平轴符号不同，理由见 camera.h：往下拖时相机要升到上方。
    // 如果这里也写成 -=，用户会觉得"上下反了"，而它其实是对的。
    sr::OrbitCamera c;
    c.pitch = 0.0f;
    c.orbit(0.0f, 100.0f);
    SR_ASSERT(c.pitch > 0.0f);
    SR_ASSERT(c.eye().y > 0.0f);
}
