#pragma once

#include <cmath>

#include "math/mat.h"

namespace sr {

// 相机与投影的构造。两个约定写在这里，因为它们决定了后面所有阶段的符号：
//   1. 视图空间是右手系、朝 **-z** 看（OpenGL 约定）。
//   2. makePerspective 产出 clip.z ∈ [-w, w]，即 NDC z ∈ [-1, 1]；
//      映到 [0, 1] 的深度是 Viewport 阶段的事（设计文档 §7.1）。
// 换约定会牵动裁剪的六个平面、深度比较的方向、背面剔除的绕向，所以改这里之前先看测试。

// fovY 是**垂直**视场角（弧度）。aspect = width / height。
// 非正的 near/far 返回单位阵：调用方会看到"什么都没投影"，比除零产生的 inf/NaN 好定位。
inline Mat4 makePerspective(float fovYRadians, float aspect, float nearZ, float farZ) {
    if (!(fovYRadians > 0.0f) || !(aspect > 0.0f) || !(nearZ > 0.0f) || !(farZ > nearZ)) {
        return Mat4::identity();
    }
    const float f = 1.0f / std::tan(fovYRadians * 0.5f);   // = cot(fovY/2)
    Mat4 r{};
    r.m[0][0] = f / aspect;
    r.m[1][1] = f;
    // z' = -(F+N)/(F-N) * z - 2FN/(F-N)，配合 w' = -z 得到 NDC z ∈ [-1, 1]。
    r.m[2][2] = -(farZ + nearZ) / (farZ - nearZ);
    r.m[2][3] = -2.0f * farZ * nearZ / (farZ - nearZ);
    r.m[3][2] = -1.0f;   // w' = -z：透视除法的全部来源就是这一项
    return r;
}

// 相机在 eye、看向 target、up 给出"上"的方向。
// up 与视线平行时 cross 为零：退回一个与视线不平行且尽量"像 up"的方向，
// 而不是让 NaN 进矩阵（NaN 的症状是整个场景消失且没有任何报错）。
inline Mat4 makeLookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
    Vec3 z = normalize(eye - target);          // 视线反方向 = 视图空间的 +z
    if (lengthSquared(z) < 1e-16f) z = Vec3{0, 0, 1};
    Vec3 x = normalize(cross(up, z));
    if (lengthSquared(x) < 1e-12f) {
        // up 与 z 平行：换一个与 z 最不共线的轴。
        const Vec3 fallback = (std::fabs(z.y) > 0.9f) ? Vec3{1, 0, 0} : Vec3{0, 1, 0};
        x = normalize(cross(fallback, z));
    }
    const Vec3 y = cross(z, x);

    Mat4 r = Mat4::identity();
    r.m[0][0] = x.x; r.m[0][1] = x.y; r.m[0][2] = x.z; r.m[0][3] = -dot(x, eye);
    r.m[1][0] = y.x; r.m[1][1] = y.y; r.m[1][2] = y.z; r.m[1][3] = -dot(y, eye);
    r.m[2][0] = z.x; r.m[2][1] = z.y; r.m[2][2] = z.z; r.m[2][3] = -dot(z, eye);
    return r;
}

// 法线矩阵 = transpose(inverse(model 的 3x3))。奇异 ⇒ false 且不写 out（与 tryInvert 同约定）。
//
// 为什么是 inverse-transpose 而不是"惯例"：法线的定义性质是**垂直于表面切向**。
// 非均匀缩放会同时改变切向与朴素变换后的法线，二者不再垂直 ——
// 例：scale(2,1,1) 把切向 (1,-1,0) 变成 (2,-1,0)，把法线 (1,1,0)/√2 朴素地变成
// (2,1,0)/√2，点积从 0 变成 3/√2。要求 (M⁻¹)ᵀn · Mt = nᵀ M⁻¹ M t = nᵀt = 0，
// 唯一满足它的变换就是 (M⁻¹)ᵀ。可执行证明见
// normal_matrix_undoes_non_uniform_scale_for_normals（那条断言就是这个几何事实本身）。
//
// 三个零件（upper3x3AsMat4 / tryInvert / transposed）都已在 mat.h 里且各有测试，
// 这里只做组合，不重写任何数学。先取 3x3 再求逆：平移不参与法线变换，
// 而 tryInvert 的相对阈值按整个 4x4 的量级算，带着平移列进去会污染那个量级。
inline bool makeNormalMatrix(const Mat4& model, Mat4& out) {
    const Mat4 u3 = model.upper3x3AsMat4();
    Mat4 inv;
    if (!Mat4::tryInvert(u3, inv)) return false;   // 奇异：out 保持被调用前的值
    out = inv.transposed();
    return true;
}

}  // namespace sr
