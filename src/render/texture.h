#pragma once
// RED 阶段的桩：类型与签名齐全，函数返回空/零。
#include <cstdint>
#include <vector>

#include "math/vec.h"

namespace sr {

struct ImageRGBA8 {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;   // 行主序，4 字节/像素，top-down
};

// 纹理：mipmap 链 + 采样参数。级别 0 是原图。
struct Texture {
    int width = 0;
    int height = 0;
    std::vector<ImageRGBA8> levels;

    // true = 字节是 **sRGB 编码的颜色数据**（PNG/JPG/TGA 存的都是这个），采样时解码到线性空间。
    // false = 字节是**数据**（uv 调试图、M6 之后的法线贴图），解码会把 0.5 变成 0.73，
    // 于是"uv 视图"不再是 uv 视图。这就是 GPU 区分 SRGB8_ALPHA8 与 RGBA8 的理由。
    bool srgb = true;

    int levelCount() const { return static_cast<int>(levels.size()); }
    const uint8_t* texel(int level, int x, int y) const;
};

// 盒式滤波生成下一级：2x2 平均，尺寸向下取整但**每一维都不低于 1**
// （2x1 的下一级是 1x1，沿只有的那一行复制采样；1x0 不是一级纹理，它会把链的尾巴吃掉）。
// 为什么必须是"下一级 = 上一级的 2x2 平均"而不是一级直接从原图算：
// 后者在奇数尺寸或长宽不等时会与硬件的 mip 链不一致，症状是 mipmap 越高层越糊得不均匀。
ImageRGBA8 downsampleBox2(const ImageRGBA8& src);

// 生成完整链，直到**两边都**变成 1（最后一级恒为 1x1）。
std::vector<ImageRGBA8> generateMipChain(const ImageRGBA8& base);

// LOD 选择的结果：level 是要采的那一级（已夹进 [0, levelCount-1]），
// frac 是朝**下一级**混合的比重 ∈ [0,1)。
// 为什么 frac 与 level 必须一起夹：三线性要读 levels[level] 与 levels[level+1]，
// 若只夹 level 不夹 frac，末级仍会带着 frac=0.7 去读那并不存在的下一级。
// 所以不变式是：**frac > 0 ⇒ level+1 < levelCount**（测试 lod_clamps_so_the_next_level_always_exists）。
struct Lod {
    int level = 0;
    float frac = 0.0f;
};

// 由屏幕导数选 mip 层级（spec §7.4：D3D11 那套 rho = max(rho_x, rho_y)，level = floor(log2 rho)）。
// 导数的单位是**归一化 UV / 像素**，也就是 TriangleSetup::dudx/dvdx/dudy/dvdy 直接给的东西
// （Mesh 的 uv 是 j/slices，取值 [0,1)）。换算成"纹素/像素"要乘纹理尺寸，这件事在这里做
// 而不是在调用方做：尺寸和 levelCount 都在 Texture 手里，交给调用方就等于安排它哪天传错。
// levelCount <= 0（空纹理）时返回 0；NaN / 无穷大的导数一律落到安全的一端。
// **lodFromDerivatives 是唯一的 log2 实现**，取整版只是它的一行包装，两份实现不会漂移。
Lod lodFromDerivatives(float texelsU, float texelsV, int levelCount, float dUdx, float dVdx,
                       float dUdy, float dVdy);

int mipLevelFromDerivatives(float texelsU, float texelsV, int levelCount, float dUdx, float dVdx,
                            float dUdy, float dVdy);

}  // namespace sr
