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
// 这里存的是**未除 w** 的属性 v，而不是 v/w。
//
// 为什么（复审 C2 纠正了本文件与计划里原先写反的那句话）：
//   clip = M * eye 是仿射的，所以 clip 空间的线性参数 t 就是 eye 空间的线性参数；
//   沿这条边 attr 与 w **各自**线性，而 attr/w 是"线性/线性"，对 t 并不线性。
//   所以裁剪时必须插值 attr 与 w，除法留到投影之后。
//   反过来（原实现）在 w 变化的边上会错：A(w=1,u=0)、B(w=2,u=3)、t=0.5 处
//   正确值是 1.5/1.5 = 1.0，插值 attr/w 得到 0.75 —— 25% 误差，
//   症状是贴近或穿进模型时，颜色与 UV 在裁剪边界上扭曲。
//
// 注意区分：屏幕空间里 attr/w 与 1/w **确实**是 x,y 的仿射函数，
// 所以 M3 的平面方程插值用 attr/w 是对的（那是 ScreenVertex 之后的事）。
struct ClipVertex {
    Vec4 clip;
    Varyings v;
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
