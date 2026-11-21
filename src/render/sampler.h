#pragma once
// 采样器（spec §7.4）：把 (u,v) + wrap + mip 换成一个**线性空间**的颜色。
//
// 本文件目前只有 nearest。之所以把 uv→下标 这一步拆成两个能单独测的小函数，
// 是因为 4.3 的双线性要复用它们，而"边界落在纹素边缘还是纹素中心"这件事
// 一旦在两处各写一遍，就会有一处悄悄漂一半 —— 症状不是崩溃，是整张贴图偏移半个纹素。
#include <cstdint>

#include "math/vec.h"
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

enum class FilterMode : uint8_t {
    Nearest,     // 取单个纹素（放大时的"像素艺术"效果）
    Bilinear,    // 四个纹素加权
    Trilinear,   // 两个 mip 级各做 bilinear 再按 frac 混合
};

// 过滤方式与 mip 开关是**两个正交的旋钮**：spec §7.6 的按键表把它们分开列
// （"mip 开关"与"采样模式"），所以 useMip=false + Trilinear 是个合法组合
// —— 语义是"仍按 frac 混，但永远在 level 0"，实测就是 Lod{0,0}，即退回 bilinear。
// 反过来 Nearest + useMip=true 表示"在被选中的那一级上取单个纹素"，不是永远 level 0。
struct SamplerState {
    WrapMode wrapU = WrapMode::Repeat;
    WrapMode wrapV = WrapMode::Repeat;
    FilterMode filter = FilterMode::Trilinear;
    bool useMip = true;
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

// level 0 上的双线性采样。**插值发生在线性空间**：先把四个 taps 各自解码到线性再加权，
// 反过来（先加权后解码）会让中间调整体偏亮，spec §7.3 的"线性空间着色"从这一步就开始生效。
// 权重用的是"距左/下那个 tap 的比例"：centered = u*W - 0.5，f = centered - floor(centered)。
TexelF sampleBilinear(const Texture& t, WrapMode wrapU, WrapMode wrapV, float u, float v);

// 指定 mip 级别的版本。**索引用该级自己的宽高**，不是 level 0 的：每级宽高减半，
// 拿 level 0 的 W 去算 level 1 的下标会把采样位置整体放大两倍（症状是贴图错位 + 莫名地模糊）。
// 级别不存在时返回全 0（有定义的值），与空纹理走同一条兜底。
TexelF sampleNearestLevel(const Texture& t, int level, WrapMode wrapU, WrapMode wrapV, float u,
                          float v);
TexelF sampleBilinearLevel(const Texture& t, int level, WrapMode wrapU, WrapMode wrapV, float u,
                           float v);

// LOD 的 Vec2 入口：gradU = (∂u/∂x, ∂u/∂y)、gradV = (∂v/∂x, ∂v/∂y)，
// 与 FragmentInput::gradU/gradV 同形状 ⇒ 调用方没有机会把四个 float 按位置传反（Ruling B）。
Lod lodFromGradients(float texelsU, float texelsV, int levelCount, Vec2 gradU, Vec2 gradV);

// 三线性：level 与 level+1 各做一次 bilinear，再按 lod.frac 混合。
TexelF sampleTrilinearLevel(const Texture& t, WrapMode wrapU, WrapMode wrapV, float u, float v,
                            const Lod& lod);
TexelF sampleTrilinear(const Texture& t, WrapMode wrapU, WrapMode wrapV, float u, float v,
                       Vec2 gradU, Vec2 gradV);

// 统一入口（FS 只用这一个）：先定 LOD，再按 FilterMode 分派。
TexelF sample(const Texture& t, const SamplerState& s, float u, float v, Vec2 gradU, Vec2 gradV);

}  // namespace sr
