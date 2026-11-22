#pragma once

#include <cmath>

#include "math/bbox.h"
#include "scene/camera.h"

namespace sr {

// 应用的默认垂直 fov（P24：全仓库唯一的 60° 写法）。fitCamera 的输出只有在
// "投影用的角 = fit 用的角"时才正确 —— 两处各写一份就会各自漂移，症状恰好是
// "某些模型装不下"，把 fit 存在的全部理由掏空。所以 makePerspective 与 fitCamera
// 必须取同一个常量（app 的 renderFrame 与 SceneState::refit 都从这里取）。
// 数值保持 main.cpp 原表达式 60.0f*3.14159265f/180.0f 不变：它与 π/3 的 float 值
// 逐位相同（0x3f860a92，两侧实测），无 --obj 的画面因此逐字节不动。
// golden_scene.h 的 kGoldenFovYRadians = 1.047f 是**golden 常量**（D15）：数值相近
// 但不相等，故意不统一 —— 动它会让 M3/M4 参考图漂移，那是"重构改变了一切"的反面。
inline constexpr float kDefaultFovYRadians = 60.0f * 3.14159265f / 180.0f;

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
    // 输入守卫（Ruling 49 的延伸：退化输入 ⇒ 不动作）。fovY=0 或 aspect=0 ⇒ halfFov=0 ⇒
    // distance=inf ⇒ eye=inf，而下游三道 fail-safe 全拦不住：normalize 的零守卫放行
    // （l2=inf > 1e-16，vec.h:69）后 inf*0=NaN；makeLookAt 的兜底不触发（NaN < 1e-16 为假，
    // transform.h:37）⇒ NaN 视图矩阵 ⇒ 整帧消失。负 aspect ⇒ 负 distance，eye 关于 target
    // 点反射（视角转到背面；视图矩阵仍是正常旋转，图像并没有奇偶镜像）。fovY=NaN 直接全链 NaN。
    // 这里是唯一拦截点；!(x > 0.0f) 形式顺带挡 NaN
    // （照 makePerspective 的写法，transform.h:18）。对任何合法输入零行为变化。
    if (!(fovYRadians > 0.0f) || !(aspect > 0.0f)) {
        return FittedCamera{base};
    }
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
    // 已知限制：distance = r/sin(θ/2) 超过 far = r*10 当且仅当 sin(θ/2) < 0.1，
    // 即 θ < 2*asin(0.1) ≈ 0.20033 rad ≈ 11.478°——fov 小于这个角时远角会被 far 裁掉。
    // 今天 fov 恒 60° 不可达，不加 clamp（任何 clamp 都会为不可达输入改变 spec 钉死的合法输入行为）；
    // fov 一旦成为可调参数就要补一条特征化测试。
    out.farPlane = r * 10.0f;
    return out;
}

}  // namespace sr
