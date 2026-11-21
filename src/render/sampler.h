#pragma once
// 采样器（spec §7.4）：把 (u,v) + wrap + mip 换成一个**线性空间**的颜色。
//
// 本文件目前只有 nearest。之所以把 uv→下标 这一步拆成两个能单独测的小函数，
// 是因为 4.3 的双线性要复用它们，而"边界落在纹素边缘还是纹素中心"这件事
// 一旦在两处各写一遍，就会有一处悄悄漂一半 —— 症状不是崩溃，是整张贴图偏移半个纹素。
#include <cstdint>

#include "render/texture.h"

namespace sr {

enum class WrapMode : uint8_t {
    Repeat,     // u=1.25 与 u=0.25 落在同一个纹素上（负数也必须对）
    ClampEdge,  // 越界钉到第一个/最后一个纹素
};

// 由一张图造出可采样纹理：生成整条 mip 链（链规则见 texture.h）。
// colorDataIsSrgb=false 用于**数据**纹理（uv 调试图等），它们不该被 gamma 解码。
Texture makeTexture(ImageRGBA8 base, bool colorDataIsSrgb = true);

// 采样结果。srgb 纹理的 rgb 已解码到线性空间；a 永远是 值/255（alpha 是覆盖率，不是颜色）。
struct TexelF {
    float r{}, g{}, b{}, a{};
};

// 整数下标的 wrap。count <= 0 时返回 0（调用方随后会从 texel() 拿到 nullptr 走兜底分支）。
// Repeat 必须自己把负数掰正：C++ 的 % 向零取整，-1 % 4 == -1，直接当索引就是越界读。
int wrapCoord(int i, int count, WrapMode wrap);

// u * count 向下取整再 wrap。约定：**u=0 是第 0 个纹素的左边缘**，
// 所以纹素中心在 (i+0.5)/count，边界在 k/count（测试把它钉死）。
// 非有限的 u（NaN / ±inf）返回 0：热路径上宁可画成边缘颜色，也不要把 NaN 放出去污染一片像素。
int texelIndex(float u, int count, WrapMode wrap);

// level 0 上的最近邻采样。越界/空纹理返回全 0（有定义的值）。
TexelF sampleNearest(const Texture& t, WrapMode wrapU, WrapMode wrapV, float u, float v);

}  // namespace sr
