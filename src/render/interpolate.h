#pragma once

#include <cmath>
#include <cstdint>

#include "render/triangle_setup.h"
#include "render/vertex.h"

namespace sr {

struct Interpolated {
    Varyings v;
    float depth = 0.0f;
    float invW = 0.0f;
};

// 屏幕空间插值。整个函数只有三行实质内容，而它是透视正确性的全部落点：
//
//   invW = planeInvW(x, y)                 // 1/w 在屏幕空间是仿射的
//   attr = planeAttr(x, y) / invW          // planeAttr 建的是 attr/w，同样是仿射的
//   depth = planeDepth(x, y)               // 深度不需要除：ndcZ 本身就仿射
//
// 第二条是"为什么透视正确插值必须除以 w"的答案：屏幕空间里线性变化的是 attr/w 而不是 attr。
// 直接用 planeAttr 的值当 attr，症状是纹理/颜色被拉向三角形中心，且随视角变化 ——
// 由 perspective_correct_u_at_edge_midpoint 钉住（1/11 而不是 0.5）。
//
// header-only：这个函数每个像素调用一次，任何非内联的调用都会成为 M9 的障碍（C5）。
inline Interpolated interpolateAt(const TriangleSetup& setup, float px, float py,
                                  int64_t* clampedCount = nullptr) {
    Interpolated out;
    out.depth = setup.planeDepth.sample(px, py);

    float invW = setup.planeInvW.sample(px, py);
    // invW → 0 意味着 w → ∞：除法会把浮点噪声放大成 inf/NaN，然后顺着颜色污染一整块区域。
    // 这里保号地夹住并计数，而不是静默返回垃圾值：计数会在 HUD 上出现，
    // 于是"某个三角形几乎平行于视线"这件事是可见的，不是隐藏的。
    constexpr float kMinInvW = 1e-8f;
    if (!(std::fabs(invW) > kMinInvW)) {
        if (clampedCount != nullptr) ++*clampedCount;
        invW = (invW < 0.0f) ? -kMinInvW : kMinInvW;
    }
    out.invW = invW;
    const float w = 1.0f / invW;

    // Varyings 必须是 12 个连续 float（vertex.h 的 static_assert），
    // 所以这里可以按分量索引写；换成反射或逐字段拷贝会让 M9 的 SIMD 无从下手。
    float* dst = reinterpret_cast<float*>(&out.v);
    for (int f = 0; f < kVaryingFloatCount; ++f) {
        dst[f] = setup.planeAttr[f].sample(px, py) * w;
    }
    return out;
}

inline float interpolateInvW(const TriangleSetup& setup, float px, float py) {
    return setup.planeInvW.sample(px, py);
}

}  // namespace sr
