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

    int levelCount() const { return static_cast<int>(levels.size()); }
    const uint8_t* texel(int level, int x, int y) const;
};

// 盒式滤波生成下一级：2x2 平均。
// 为什么必须是"下一级 = 上一级的 2x2 平均"而不是一级直接从原图算：
// 后者在奇数尺寸或长宽不等时会与硬件的 mip 链不一致，症状是 mipmap 越高层越糊得不均匀。
ImageRGBA8 downsampleBox2(const ImageRGBA8& src);

// 生成完整链，直到某一边变成 1。
std::vector<ImageRGBA8> generateMipChain(const ImageRGBA8& base);

// 由屏幕导数选 mip 层级（M4 后面用）。
int mipLevelFromDerivatives(float maxU, float maxV, float dPdxU, float dPdxV, float dPdyU,
                            float dPdyV);

}  // namespace sr
