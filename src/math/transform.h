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

}  // namespace sr
