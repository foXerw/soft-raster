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

    Vec3 eye() const {
        const float cp = std::cos(pitch), sp = std::sin(pitch);
        return target + Vec3{distance * cp * std::sin(yaw), distance * sp, distance * cp * std::cos(yaw)};
    }

    Mat4 view() const { return makeLookAt(eye(), target, Vec3{0, 1, 0}); }

    void orbit(float dxPixels, float dyPixels) {
        // 每像素 0.01 弧度：一屏拖动约转半圈，手感上"跟手"。
        yaw += dxPixels * 0.01f;
        pitch += dyPixels * 0.01f;      // 屏幕 y 朝下：向下拖 ⇒ dy 为正 ⇒ 仰角降低，
                                        // 也就是"把物体往上翻"，与大多数查看器一致。
        constexpr float kLimit = 1.5533f;   // 89°
        if (pitch > kLimit) pitch = kLimit;
        if (pitch < -kLimit) pitch = -kLimit;
    }

    void dolly(float wheelSteps) {
        distance *= (wheelSteps > 0.0f) ? 0.9f : 1.0f / 0.9f;
        if (distance < 0.05f) distance = 0.05f;
        if (distance > 1000.0f) distance = 1000.0f;
    }
};

}  // namespace sr
