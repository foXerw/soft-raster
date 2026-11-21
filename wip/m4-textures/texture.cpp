#include "render/texture.h"

#include <algorithm>
#include <cmath>

namespace sr {

const uint8_t* Texture::texel(int level, int x, int y) const {
    if (level < 0 || level >= static_cast<int>(levels.size())) return nullptr;
    const ImageRGBA8& im = levels[static_cast<size_t>(level)];
    if (x < 0 || y < 0 || x >= im.width || y >= im.height) return nullptr;
    return &im.rgba[(static_cast<size_t>(y) * im.width + static_cast<size_t>(x)) * 4u];
}

ImageRGBA8 downsampleBox2(const ImageRGBA8& src) {
    // 向下取整：奇数尺寸时丢掉最后一条不完整的行列，与 GPU 的 mip 链一致。
    const int w = src.width / 2;
    const int h = src.height / 2;
    ImageRGBA8 out;
    out.width = w;
    out.height = h;
    out.rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0);
    if (w <= 0 || h <= 0) return out;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // 累加用 int：四个 8bit 相加最大 1020，不会溢出，而浮点平均会引入
            // 0.5 舍入的歧义（"平均保持性"那条测试对舍入方式敏感）。
            int sum[4] = {0, 0, 0, 0};
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const size_t at = (static_cast<size_t>(y * 2 + dy) * src.width +
                                       static_cast<size_t>(x * 2 + dx)) * 4u;
                    for (int c = 0; c < 4; ++c) sum[c] += src.rgba[at + static_cast<size_t>(c)];
                }
            }
            const size_t at = (static_cast<size_t>(y) * w + static_cast<size_t>(x)) * 4u;
            // 四舍五入而不是截断：截断会让每一级都系统性偏暗，
            // 于是"mip 越高越暗"看起来像是设计的一部分。
            for (int c = 0; c < 4; ++c) {
                out.rgba[at + static_cast<size_t>(c)] =
                    static_cast<uint8_t>((sum[c] + 2) / 4);
            }
        }
    }
    return out;
}

std::vector<ImageRGBA8> generateMipChain(const ImageRGBA8& base) {
    std::vector<ImageRGBA8> chain;
    if (base.width <= 0 || base.height <= 0) return chain;
    chain.push_back(base);
    while (true) {
        const ImageRGBA8& prev = chain.back();
        if (prev.width <= 1 && prev.height <= 1) break;
        const ImageRGBA8 next = downsampleBox2(prev);
        if (next.width <= 0 || next.height <= 0) break;
        chain.push_back(next);
    }
    return chain;
}

int mipLevelFromDerivatives(float maxU, float maxV, float dPdxU, float dPdxV, float dPdyU,
                            float dPdyV) {
    // 各向异性时取 max 而不是 sqrt(乘积)：后者是"几何平均"，会在一个方向跨得很远时
    // 选到太高的分辨率，症状是某个方向仍然闪烁。真正的各向异性过滤（M11 之外）才需要
    // 沿方向多次采样，这里做的是 GPU 默认行为。
    const float ux = std::fabs(dPdxU) * maxU, vy = std::fabs(dPdyV) * maxV;
    const float uy = std::fabs(dPdxV) * maxV, vx = std::fabs(dPdyU) * maxU;
    const float rhoX = std::sqrt(ux * ux + uy * uy);
    const float rhoY = std::sqrt(vx * vx + vy * vy);
    const float rho = std::max(rhoX, rhoY);
    if (!(rho > 1.0f)) return 0;         // 被放大：用最高分辨率层，绝不返回负数
    const int level = static_cast<int>(std::floor(std::log2(rho)));
    return level < 0 ? 0 : level;
}

}  // namespace sr
