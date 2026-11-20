#pragma once

#include "math/vec.h"

namespace sr {

// 管线的顶点数据形状。
//
// Varyings 是**固定 4 槽**的 varying 集合，刻意不做通用 attribute 表：
// 通用表意味着每帧要查布局描述符，而本项目的学习目标是看清每个阶段在做什么，
// 不是做一个能挂任意 shader 的引擎（设计文档 N1/A4）。
//
// 但它必须**全 float 且无填充**：M3 的平面方程按 float 分量建数组（12 个），
// M9 的 SIMD 一次处理 4 个分量。那条 static_assert 是接口的一部分，不是装饰。
struct Varyings {
    Vec4 color{};        // 顶点色，线性空间   分量 0..3
    Vec3 worldNormal{};  // 世界空间法线       分量 4..6
    Vec2 uv{};           // 纹理坐标           分量 7..8
    Vec3 worldPos{};     // 世界空间位置       分量 9..11
};

inline constexpr int kVaryingFloatCount = 12;
static_assert(sizeof(Varyings) == kVaryingFloatCount * sizeof(float),
              "Varyings must be 12 floats with no padding (see the comment above)");

inline Varyings operator*(const Varyings& v, float s) {
    Varyings r;
    r.color = v.color * s;
    r.worldNormal = v.worldNormal * s;
    r.uv = v.uv * s;
    r.worldPos = v.worldPos * s;
    return r;
}

inline const float* varyingFloats(const Varyings& v) { return reinterpret_cast<const float*>(&v); }

inline const char* varyingFieldName(int i) {
    static const char* names[kVaryingFloatCount] = {
        "color.x", "color.y", "color.z", "color.w",
        "worldNormal.x", "worldNormal.y", "worldNormal.z",
        "uv.x", "uv.y",
        "worldPos.x", "worldPos.y", "worldPos.z",
    };
    return (i >= 0 && i < kVaryingFloatCount) ? names[i] : "<out of range>";
}

struct VSIn {
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
    Vec4 color{1, 1, 1, 1};   // 顶点色；没有颜色的几何体传白
};

// 裁剪阶段的输入/输出顶点。
//
// vOverW == v / clip.w，并且裁剪对 clip 与 vOverW 用**同一个**线性插值参数。
// 这是透视正确性的来源，推导写在这里（设计文档 A1）：
//   屏幕空间里，1/w 与 attr/w 都是 x,y 的仿射函数；沿一条 clip 空间的边做线性插值、
//   再统一除以插值后的 w，得到的恰好是屏幕空间里 attr 的正确值。
//   反过来，如果裁剪时插值的是未除 w 的 v，除完 w 之后属性会被拉回三角形中心，
//   症状是纹理/颜色随视角变化地扭曲（M3 的四边形数值测试会抓住它）。
struct ClipVertex {
    Vec4 clip;
    Varyings vOverW;
};

// 视口之后的屏幕空间顶点。depth 属于 [0,1]（非 reverse-Z），pos 是像素坐标（top-down）。
struct ScreenVertex {
    Vec2 pos;
    float depth = 0.0f;
    float w = 1.0f;
    float invW = 1.0f;
    Varyings v;
};

}  // namespace sr
