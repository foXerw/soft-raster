#pragma once

#include <cmath>

namespace sr {

// 项目里最小的那层数学：三个向量类型 + 自由函数，全部 inline。
//
// 为什么是自由函数而不是成员：`dot(a, b)` 与 `cross(a, b)` 读起来对称，
// 而 `a.dot(b)` 会诱导人写出 `a.cross(b).dot(c)` 这种链式表达式，
// 在推导阶段（法线、边函数、平面方程）反而更容易看错。
//
// 为什么没有 SIMD 类型：M9 的向量化发生在光栅化 inner loop（一次 8 个像素），
// 不是发生在"一次 3 个分量"的向量运算上。用 float[3] 的结构体更容易验证正确性，
// 而编译器在 Release 下本来就能自动向量化这些短小函数。

struct Vec2 {
    float x{}, y{};
};

struct Vec3 {
    float x{}, y{}, z{};
};

struct Vec4 {
    float x{}, y{}, z{}, w{};
};

// ---- Vec2 ----------------------------------------------------------------

inline Vec2 operator+(const Vec2& a, const Vec2& b) { return {a.x + b.x, a.y + b.y}; }
inline Vec2 operator-(const Vec2& a, const Vec2& b) { return {a.x - b.x, a.y - b.y}; }
inline Vec2 operator*(const Vec2& a, float s) { return {a.x * s, a.y * s}; }
inline Vec2 operator*(float s, const Vec2& a) { return a * s; }
inline Vec2 operator/(const Vec2& a, float s) { return {a.x / s, a.y / s}; }
inline float dot(const Vec2& a, const Vec2& b) { return a.x * b.x + a.y * b.y; }

// 二维"叉积"返回标量 = 3D 叉积的 z 分量。
// 符号就是"点在有向边的哪一侧"，这是 EdgeFunc 的全部基础：
// 在 y 朝下的 top-down 屏幕空间里，内侧法线是 n = {-e.y, e.x}（实现见 plane_eq.h:52，
// 验证例见 plane_eq.h:32-34）。{e.y, -e.x} 是教科书 y 朝上的写法，符号正好相反 ——
// 抄错的症状是"整个三角形一个像素都不画"。
inline float cross(const Vec2& a, const Vec2& b) { return a.x * b.y - a.y * b.x; }

// ---- Vec3 ----------------------------------------------------------------

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator-(const Vec3& a) { return {-a.x, -a.y, -a.z}; }
inline Vec3 operator*(const Vec3& a, const Vec3& b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline Vec3 operator*(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(float s, const Vec3& a) { return a * s; }
inline Vec3 operator/(const Vec3& a, float s) { return {a.x / s, a.y / s, a.z / s}; }

inline float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float lengthSquared(const Vec3& a) { return dot(a, a); }
inline float length(const Vec3& a) { return std::sqrt(dot(a, a)); }

// 零向量约定：长度平方 < 1e-16 时返回零向量，而不是除以 0 得到 NaN。
//
// 这不是"防御式编程"的洁癖。退化法线（OBJ 里两个重合顶点、或零面积面）在真实模型里
// 一定会出现，而 NaN 一旦进入光照就会污染整块区域，症状是"某片区域偶发变黑"——
// 那种 bug 的定位成本远高于这一行 if。约定由测试 normalize_of_zero_vector_is_zero_not_nan 钉住。
inline Vec3 normalize(const Vec3& a) {
    const float l2 = dot(a, a);
    if (!(l2 > 1e-16f)) return {0.0f, 0.0f, 0.0f};
    const float inv = 1.0f / std::sqrt(l2);
    return {a.x * inv, a.y * inv, a.z * inv};
}

// ---- Vec4 ----------------------------------------------------------------

inline Vec4 operator+(const Vec4& a, const Vec4& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
inline Vec4 operator-(const Vec4& a, const Vec4& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}
inline Vec4 operator*(const Vec4& a, float s) { return {a.x * s, a.y * s, a.z * s, a.w * s}; }
inline Vec4 operator*(float s, const Vec4& a) { return a * s; }
inline float dot(const Vec4& a, const Vec4& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

// 位置用 w=1（会被平移），方向用 w=0（不会被平移）。矩阵乘法的最后一列因此能正确作用。
inline Vec4 makeVec4(const Vec3& v, float w) { return {v.x, v.y, v.z, w}; }
inline Vec3 xyz(const Vec4& v) { return {v.x, v.y, v.z}; }

// 透视除法：clip → NDC。裁剪保证视锥内顶点 w ≥ 0（kPlaneNegW 的 d ≥ 0 把边界算作"在内"）；w == 0 未被测试覆盖。
inline Vec3 perspectiveDivide(const Vec4& v) {
    const float inv = 1.0f / v.w;
    return {v.x * inv, v.y * inv, v.z * inv};
}

// ---- 公共插值 ------------------------------------------------------------

// 裁剪阶段与属性插值都用它。注意：对 ClipVertex 的 clip 与**未除的** v 必须用**同一个** t，
// 这是透视正确性的来源（推导见 docs/notes/M2 §2，实现见 render/clipping.cpp:10-18）。
inline Vec2 lerp(const Vec2& a, const Vec2& b, float t) { return a + (b - a) * t; }
inline Vec3 lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }
inline Vec4 lerp(const Vec4& a, const Vec4& b, float t) { return a + (b - a) * t; }

}  // namespace sr
