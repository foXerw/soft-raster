#include "render/texture.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace sr {

const uint8_t* Texture::texel(int level, int x, int y) const {
    if (level < 0 || level >= static_cast<int>(levels.size())) return nullptr;
    const ImageRGBA8& im = levels[static_cast<size_t>(level)];
    if (x < 0 || y < 0 || x >= im.width || y >= im.height) return nullptr;
    return &im.rgba[(static_cast<size_t>(y) * im.width + static_cast<size_t>(x)) * 4u];
}

ImageRGBA8 downsampleBox2(const ImageRGBA8& src) {
    ImageRGBA8 out;
    if (src.width <= 0 || src.height <= 0) return out;   // 空图 ⇒ 0x0，交给调用方判断

    // 奇数尺寸仍然**向下取整**（丢掉最后一条不完整的行列，与 GPU 的 mip 链一致），
    // 但一级不允许是 0 厚：2x1 的下一级必须是 1x1。少了这个下限，链会在
    // "短边先到 1"处断掉（8x4 只有 3 级、1x8 只有 1 级），而所有按 log2(边长)+1
    // 数级别的人都去读那并不存在的最后一级 —— M4.1 的进程崩溃正是如此。
    const int w = std::max(1, src.width / 2);
    const int h = std::max(1, src.height / 2);
    out.width = w;
    out.height = h;
    out.rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // 累加用 int：四个 8bit 相加最大 1020，不会溢出，而浮点平均会引入
            // 0.5 舍入的歧义（"平均保持性"那条测试对舍入方式敏感）。
            int sum[4] = {0, 0, 0, 0};
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    // 只有"某一边已经是 1、下一级还要再来一次"的情形会真的夹到
                    // （例如 2x1 → 1x1 的第二行）。夹到边界 = 复制边缘纹素，
                    // 于是分母**仍然是 4**：平均保持性不被破坏，链尾不会突变亮度。
                    const int sy = std::min(y * 2 + dy, src.height - 1);
                    const int sx = std::min(x * 2 + dx, src.width - 1);
                    const size_t at = (static_cast<size_t>(sy) * src.width +
                                       static_cast<size_t>(sx)) * 4u;
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
        // prev 只在本轮的前两行活着：下面那次 push_back 可能让 chain 重分配。
        // 之前它跨过 push_back 一直存在（当轮没再用，所以没炸），但"引用跨过可能
        // 重分配的调用"是等着咬下一口的形状（Task 0.5 记过同类），所以让它到此为止。
        const ImageRGBA8& prev = chain.back();
        if (prev.width <= 1 && prev.height <= 1) break;
        ImageRGBA8 next = downsampleBox2(prev);
        // 兜底：downsampleBox2 只在空图时返回 0x0。宁可链短一级，也不要死循环。
        if (next.width <= 0 || next.height <= 0) break;
        chain.push_back(std::move(next));
    }
    return chain;
}

ImageRGBA8 makeCheckerImage(int cells, int cellPx, const CheckerPalette& palette) {
    ImageRGBA8 out;
    if (cells <= 0 || cellPx <= 0) return out;
    // 上限与 cmdline 的 kMaxDimension 一致：这张图会被 generateMipChain 整条展开
    // （约 4/3 倍内存），一个手滑的 100000 会在测试进程里安静地吃掉几十 GB。
    // 超限走与非法尺寸相同的兜底（空图），调用方按 width==0 判断。
    const size_t side = static_cast<size_t>(cells) * static_cast<size_t>(cellPx);
    if (side > 16384u) return out;

    out.width = static_cast<int>(side);
    out.height = static_cast<int>(side);
    out.rgba.assign(side * side * 4u, 0);

    for (size_t y = 0; y < side; ++y) {
        const int cy = static_cast<int>(y) / cellPx;
        for (size_t x = 0; x < side; ++x) {
            const int cx = static_cast<int>(x) / cellPx;
            const bool plain = ((cx + cy) & 1) == 0;   // 奇偶约定写死在头文件注释里
            const uint8_t* c = plain ? palette.dark : palette.light;
            // 三个角标记。cells==1 时三条分支指向同一格，于是"最后写的赢"就是优先级：
            // origin 最后 ⇒ cells==1 得到一整格 uv 原点色（测试 pin 住了这个取舍）。
            if (cx == cells - 1 && cy == 0) c = palette.uAxis;
            if (cx == 0 && cy == cells - 1) c = palette.vAxis;
            if (cx == 0 && cy == 0) c = palette.origin;

            const size_t at = (y * side + x) * 4u;
            out.rgba[at] = c[0];
            out.rgba[at + 1u] = c[1];
            out.rgba[at + 2u] = c[2];
            out.rgba[at + 3u] = c[3];
        }
    }
    return out;
}

Lod lodFromDerivatives(float texelsU, float texelsV, int levelCount, float dUdx, float dVdx,
                       float dUdy, float dVdy) {
    Lod out;
    if (levelCount <= 0) return out;      // 空纹理：没有级可选，也别让调用方拿去索引
    // 各向异性时取 max 而不是 sqrt(乘积)：后者是"几何平均"，会在一个方向跨得很远时
    // 选到太高的分辨率，症状是某个方向仍然闪烁。真正的各向异性过滤（M11 之外）才需要
    // 沿方向多次采样，这里做的是 GPU 默认行为。
    // u 分量乘 texelsU、v 分量乘 texelsV —— 两个方向各用自己的尺寸，非方形纹理才不串。
    const float ux = std::fabs(dUdx) * texelsU, vy = std::fabs(dVdy) * texelsV;
    const float uy = std::fabs(dVdx) * texelsV, vx = std::fabs(dUdy) * texelsU;
    const float rhoX = std::sqrt(ux * ux + uy * uy);
    const float rhoY = std::sqrt(vx * vx + vy * vy);
    const float rho = std::max(rhoX, rhoY);
    if (!(rho > 1.0f)) return out;        // 被放大：level 0、frac 0。NaN 也走这一支
    const float l = std::log2(rho);
    if (!std::isfinite(l)) {              // rho = inf：直接给末级，不去做 UB 的转换
        out.level = levelCount - 1;
        return out;
    }
    int level = static_cast<int>(std::floor(l));
    if (level < 0) level = 0;
    if (level >= levelCount) {
        // 夹到末级 ⇒ **没有下一级可混**，frac 必须归零，否则三线性会去读 levels[levelCount]
        out.level = levelCount - 1;
        return out;
    }
    out.level = level;
    out.frac = l - static_cast<float>(level);
    return out;
}

int mipLevelFromDerivatives(float texelsU, float texelsV, int levelCount, float dUdx, float dVdx,
                            float dUdy, float dVdy) {
    return lodFromDerivatives(texelsU, texelsV, levelCount, dUdx, dVdx, dUdy, dVdy).level;
}

}  // namespace sr
