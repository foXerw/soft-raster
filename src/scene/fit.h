#pragma once

#include <cmath>

#include "math/bbox.h"
#include "scene/camera.h"

namespace sr {

// fit 的结果：相机 + 配套的近远平面。near/far 的默认值就是退化输入时退回的"调用方默认"。
struct FittedCamera {
    OrbitCamera camera;
    float nearPlane = 0.1f;
    float farPlane = 100.0f;
};

// 包围球 fit（设计 §11 + D16）：让 bounds 整个落进视锥，模型单位是 mm 还是 m 都立刻可见（G-M5.1）。
//
// 为什么是 sin 而不是 tan：界是**球**。半径 r 的球在距离 d 处的轮廓半角是 asin(r/d)，
// 取 d = r/sin(θ/2) 让轮廓恰好张满 θ/2；而 r 是 AABB 半对角线（bbox.h 的 radius()），
// 8 个角都在球上或球内，于是整个盒子必然在视锥里。tan 对应的是"平面张角"，
// 用在球上会差一个 1/cos，把盒子裁掉一圈。
//
// 为什么取 min(fovX, fovY)：竖屏（aspect < 1）时水平 fov 更紧，只用 fovY 会左右裁切。
//
// 只改 distance / target / near / far；yaw 与 pitch 从 base 继承——按 F 的人不是为了被重置视角。
// fit 只发生在调用方自己的相机里，model 保持单位阵，golden 的姿态常量（golden_scene.h）继续有效（D15）。
inline FittedCamera fitCamera(const BBox& bounds, float fovYRadians, float aspect, const OrbitCamera& base) {
    const float r = bounds.radius();
    // Ruling 49：退化输入的安全响应是**不动作**。r = 0 时公式给出 distance = near = far = 0，
    // 投影矩阵除以 (far - near) 会得到 NaN/inf ⇒ 整帧什么都画不出来（本项目的 makePerspective
    // 对非正 near/far 退回单位阵，症状同样是整帧消失）——比"相机没动"糟糕得多。
    // 所以原样返回 base + 结构体默认 near/far，不做任何"小正数 distance"替代。
    if (bounds.empty() || r < 1e-6f) {
        return FittedCamera{base};
    }
    const float fovX = 2.0f * std::atan(std::tan(fovYRadians * 0.5f) * aspect);
    const float halfFov = 0.5f * (fovX < fovYRadians ? fovX : fovYRadians);
    FittedCamera out;
    out.camera = base;
    out.camera.distance = r / std::sin(halfFov);
    out.camera.target = bounds.center();
    out.nearPlane = r / 1000.0f;
    out.farPlane = r * 10.0f;
    return out;
}

}  // namespace sr
