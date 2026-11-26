#pragma once

#include <cmath>

#include "math/vec.h"

namespace sr {

// 4x4 齐次变换矩阵，**行主序**存储，配合**列向量右乘**：p' = M * p。
//
// 于是 m[row][col]，而 result[r][c] = sum_k a[r][k] * b[k][c]。
// 组合顺序读起来是"从右往左应用"：`proj * view * model * p` 先 model、再 view、再 proj，
// 这与几乎所有教科书一致，也因此 `d * c` 与 `c * d` 的结果不同必须由测试守住
// （见 test_mat.cpp 里"矩阵乘法不可交换"那组断言，症状是所有物体按缩放系数漂移）。

struct Mat4 {
    float m[4][4]{};

    static Mat4 identity();
    static Mat4 translation(const Vec3& t);
    static Mat4 scale(const Vec3& s);
    static Mat4 rotationX(float radians);
    static Mat4 rotationY(float radians);
    static Mat4 rotationZ(float radians);

    // 一般 4x4 求逆。失败（奇异或接近奇异）时返回 false 并且**不写 out**：
    // 调用方（法线矩阵、相机 fit）拿到 false 就该报错，而不是拿着一堆 NaN 继续画。
    static bool tryInvert(const Mat4& in, Mat4& out);

    Mat4 transposed() const;
    Mat4 upper3x3AsMat4() const;   // 法线矩阵的基底：丢掉平移列与最后一行

    Mat4 operator*(const Mat4& other) const;
    Vec4 operator*(const Vec4& v) const;
};

inline Mat4 Mat4::identity() {
    Mat4 r{};
    r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
    return r;
}

inline Mat4 Mat4::translation(const Vec3& t) {
    Mat4 r = identity();
    r.m[0][3] = t.x;
    r.m[1][3] = t.y;
    r.m[2][3] = t.z;
    return r;
}

inline Mat4 Mat4::scale(const Vec3& s) {
    Mat4 r{};
    r.m[0][0] = s.x;
    r.m[1][1] = s.y;
    r.m[2][2] = s.z;
    r.m[3][3] = 1.0f;
    return r;
}

// 右手系、绕 +轴逆时针（从轴的正方向看回去）。
inline Mat4 Mat4::rotationX(float a) {
    const float c = std::cos(a), s = std::sin(a);
    Mat4 r = identity();
    r.m[1][1] = c; r.m[1][2] = -s;
    r.m[2][1] = s; r.m[2][2] = c;
    return r;
}

inline Mat4 Mat4::rotationY(float a) {
    const float c = std::cos(a), s = std::sin(a);
    Mat4 r = identity();
    r.m[0][0] = c; r.m[0][2] = s;
    r.m[2][0] = -s; r.m[2][2] = c;
    return r;
}

inline Mat4 Mat4::rotationZ(float a) {
    const float c = std::cos(a), s = std::sin(a);
    Mat4 r = identity();
    r.m[0][0] = c; r.m[0][1] = -s;
    r.m[1][0] = s; r.m[1][1] = c;
    return r;
}

inline Mat4 Mat4::operator*(const Mat4& b) const {
    Mat4 r{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            r.m[row][col] = m[row][0] * b.m[0][col] + m[row][1] * b.m[1][col] +
                            m[row][2] * b.m[2][col] + m[row][3] * b.m[3][col];
        }
    }
    return r;
}

inline Vec4 Mat4::operator*(const Vec4& v) const {
    return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z + m[0][3] * v.w,
            m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z + m[1][3] * v.w,
            m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z + m[2][3] * v.w,
            m[3][0] * v.x + m[3][1] * v.y + m[3][2] * v.z + m[3][3] * v.w};
}

inline Mat4 Mat4::transposed() const {
    Mat4 r{};
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col) r.m[col][row] = m[row][col];
    return r;
}

inline Mat4 Mat4::upper3x3AsMat4() const {
    Mat4 r{};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) r.m[row][col] = m[row][col];
    r.m[3][3] = 1.0f;
    return r;
}

namespace {

// 3x3 行列式，按第一行展开。
inline float det3(const float m[3][3]) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
           m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

// (r, c) 的余子式：删掉第 r 行第 c 列后的 3x3 行列式，带 (-1)^(r+c) 符号。
inline float cofactor(const float m[4][4], int r, int c) {
    float sub[3][3];
    int si = 0;
    for (int i = 0; i < 4; ++i) {
        if (i == r) continue;
        int sj = 0;
        for (int j = 0; j < 4; ++j) {
            if (j == c) continue;
            sub[si][sj++] = m[i][j];
        }
        ++si;
    }
    const float d = det3(sub);
    return (((r + c) & 1) != 0) ? -d : d;
}

}  // namespace

// 按定义求逆：A⁻¹ = adj(A) / det(A)，其中 adj(A) = 余子式矩阵的**转置**。
//
// 为什么不用"24 项展开"的快写法：那种写法是抄来的索引表，抄错一个下标时
// 症状是"逆矩阵大致对但某些元素偏一点"，测试里表现为 a*inv 离单位阵差 0.2~0.9 ——
// 这个坑本任务已经踩过一次。求逆每帧最多调用几次（法线矩阵、相机 fit），
// 这里要的是**能核对**，不是快。
inline bool Mat4::tryInvert(const Mat4& in, Mat4& out) {
    float cof[4][4];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) cof[r][c] = cofactor(in.m, r, c);

    // 沿第一行展开行列式。
    const float det = in.m[0][0] * cof[0][0] + in.m[0][1] * cof[0][1] + in.m[0][2] * cof[0][2] +
                      in.m[0][3] * cof[0][3];

    // 阈值取相对量级而不是固定 1e-8：det 随单位的三次方缩放，固定阈值于是跟着单位走。
    // 实测（float）：scale(1e3,1e3,1e-6) 的 det = 1.0 > 1e-8，而它的条件数是 1e9——
    // 固定阈值会放这种"数学上可逆、数值上不该求逆"的阵过去；1e-8·maxAbs³ = 10 把它拒掉。
    // 这条由 test_mat.cpp 的 mat4_tryInvert_rejects_singular_and_leaves_output_untouched 钉住。
    //
    // 反向的边界也要写清楚，别把"相对阈值"读成"与单位无关"：maxAbs 至少是 m[3][3] 的那个 1，
    // 所以小坐标端阈值退化回 1e-8，悬崖落在 s³ = 1e-8 即 s ≈ 2.154e-3——
    // 均匀缩到千分之一以下的合法模型会被判奇异（实测 scale(3e-3) 可逆、scale(1e-3) 被拒）。
    // makeNormalMatrix 也在这条悬崖下面，因为它复用同一个阈值（transform.h:63-64 讲的就是这个量级污染）。
    float maxAbs = 0.0f;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) maxAbs = (std::fabs(in.m[i][j]) > maxAbs) ? std::fabs(in.m[i][j]) : maxAbs;
    const float eps = 1e-8f * maxAbs * maxAbs * maxAbs;
    if (!(std::fabs(det) > eps)) return false;   // 奇异：out 保持被调用前的值

    const float invDet = 1.0f / det;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out.m[r][c] = cof[c][r] * invDet;   // 注意这里的转置
    return true;
}

}  // namespace sr
