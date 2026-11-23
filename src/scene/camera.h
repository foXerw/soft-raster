#pragma once

#include <cmath>

#include "math/mat.h"
#include "math/transform.h"
#include "math/vec.h"

namespace sr {

// 轨道相机：绕一个目标点转，距离可推拉。M7 的正式形态，这里先落地"拖拽旋转"这一件事。
//
// 约定：yaw 绕世界 +y，pitch 是仰角并夹在 ±89°（越过极点时 up 与视线接近平行，
// makeLookAt 会退回备用 up，画面会突然翻转 —— 夹住比处理翻转更省事也更符合直觉）。
struct OrbitCamera {
    float yaw = 0.0f;
    float pitch = 0.35f;
    float distance = 5.0f;
    Vec3 target{0, 0, 0};
    // dolly 的夹钳区间（Ruling 73）。**默认值就是旧常数** ⇒ 非 fit 路径（程序立方体、
    // test_camera 既有用例）行为一字不变 —— 刻意不让 M5.6 顺带改 M2 以来的相机语义。
    // fit 是 distance 的第二个写者：SceneState::refit 按模型尺度设这两个字段，否则
    // mm 级 / 千单位级模型第一次滚轮就被旧常数 snap（"某些模型一动就飞走"）。
    float minDistance = 0.05f;
    float maxDistance = 1000.0f;

    Vec3 eye() const {
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        return target + Vec3{distance * cp * std::sin(yaw), distance * sp, distance * cp * std::cos(yaw)};
    }

    Mat4 view() const { return makeLookAt(eye(), target, Vec3{0, 1, 0}); }

    // "抓住表面"的约定：光标下的那个点应当跟着光标走。两个轴的符号因此不同，
    // 这不是笔误，而是"相机绕物体转"与"物体跟着手转"之间的换算在两个轴上方向相反：
    //   水平：往右拖 ⇒ 物体向右转 ⇒ 相机必须绕到物体的**左**侧 ⇒ yaw 减小。
    //         （验证：yaw 从 0 减小，eye 移到 -x；此时世界 +z 上的点投影到画面右侧 ✓）
    //   垂直：往下拖 ⇒ 物体前表面向下走、顶部转过来 ⇒ 相机必须升到物体**上方** ⇒ pitch 增大。
    //         （验证：eye 上移并俯视原点，原点高度的点落到画面下方 ✓）
    // 第一版两个轴都写成 +=，水平方向因此是反的（产品方反馈）。
    void orbit(float dxPixels, float dyPixels) {
        // 每像素 0.01 弧度：一屏拖动约转半圈，手感上"跟手"。
        yaw -= dxPixels * 0.01f;
        pitch += dyPixels * 0.01f;
        constexpr float kLimit = 1.5533f;   // 89°
        if (pitch > kLimit) pitch = kLimit;
        if (pitch < -kLimit) pitch = -kLimit;
    }

    void dolly(float wheelSteps) {
        distance *= (wheelSteps > 0.0f) ? 0.9f : 1.0f / 0.9f;
        // 夹钳读**字段**而不是常数（Ruling 73）：fit 过的相机带着自己尺度的区间。
        if (distance < minDistance) distance = minDistance;
        if (distance > maxDistance) distance = maxDistance;
    }
};

}  // namespace sr
