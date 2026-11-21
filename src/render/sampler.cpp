#include "render/sampler.h"

#include <algorithm>
#include <cmath>

#include "render/color.h"

namespace sr {

Texture makeTexture(ImageRGBA8 base, bool colorDataIsSrgb) {
    Texture t;
    t.width = base.width;
    t.height = base.height;
    t.srgb = colorDataIsSrgb;
    t.levels = generateMipChain(base);
    return t;
}

int wrapCoord(int i, int count, WrapMode wrap) {
    if (count <= 0) return 0;
    if (wrap == WrapMode::ClampEdge) return std::clamp(i, 0, count - 1);
    const int m = i % count;                 // 符号跟被除数：-1 % 4 == -1
    return m < 0 ? m + count : m;            // 掰成正的才是纹素下标
}

int texelIndex(float u, int count, WrapMode wrap) {
    if (count <= 0) return 0;
    // 先乘到纹素尺度，再决定怎么收回 int 值域：
    // float→int 只在值域内才安全，而 UV 在退化三角形/极端缩放下可以是任意大的数，
    // 所以 Repeat 先用 fmod 把整数圈摘掉（|结果| < count），ClampEdge 直接夹住。
    // 注意 fmod **只对 Repeat 做**：对 ClampEdge 做的话 u=2.0 会被折回 0，
    // 而它应该钉在最后一个纹素上（测试 clamp_edge_pins_outside_uv_to_the_border_texel 抓住过这个）。
    float s = u * static_cast<float>(count);
    if (!std::isfinite(s)) {
        s = 0.0f;                            // NaN / ±inf：给一个有定义的落点，不走 UB 的转换
    } else if (wrap == WrapMode::Repeat) {
        s = std::fmod(s, static_cast<float>(count));
    } else {
        s = std::clamp(s, -1.0f, static_cast<float>(count));
    }
    // floor 而不是截断：s = -1.0 两种给的一样，但 s = -0.2 截断给 0、floor 给 -1，
    // 后者才是"第 0 个纹素左边沿外一格"的正确读法（Repeat 下 = 最后一个纹素）。
    return wrapCoord(static_cast<int>(std::floor(s)), count, wrap);
}

namespace {

// 字节 → 线性值。nearest 与 bilinear 共用一份：解码规则（哪些通道解码、alpha 不解码）
// 写两遍的话，将来只改一遍就会出现"同一张图两种过滤给出不同的暗部"。
TexelF toTexelF(const Texture& t, const uint8_t* px) {
    if (px == nullptr) return TexelF{};    // 空纹理/越界。仍然返回有定义的值，不放 NaN 出去
    TexelF out;
    if (t.srgb) {
        // 每个通道一次 256 项查表（Task 0.3 的 DecodeLut），不是 pow：
        // 这是逐像素路径，pow 会把整个 M4 的可见成果变成一次性能演示。
        out.r = srgb8ToLinear(px[0]);
        out.g = srgb8ToLinear(px[1]);
        out.b = srgb8ToLinear(px[2]);
    } else {
        out.r = static_cast<float>(px[0]) / 255.0f;
        out.g = static_cast<float>(px[1]) / 255.0f;
        out.b = static_cast<float>(px[2]) / 255.0f;
    }
    out.a = static_cast<float>(px[3]) / 255.0f;   // alpha 永远不做 gamma 解码
    return out;
}

// 双线性用的"左/下那个 tap"。centered 已经是**平移过半个纹素**的坐标（u*W - 0.5），
// 所以 floor(centered) 就是左 tap 的下标，小数部分就是朝右挪的比重。
// 与 texelIndex 一样先把值收进 int 的安全范围：float→int 超范围是 UB，
// 而退化三角形给来的 uv 可以是任意大的数。
int lowerTap(float centered, int count, WrapMode wrap, float& frac) {
    if (count <= 0 || !std::isfinite(centered)) {
        frac = 0.0f;
        return 0;
    }
    float s = centered;
    if (wrap == WrapMode::Repeat) {
        s = std::fmod(s, static_cast<float>(count));
        if (s < 0.0f) s += static_cast<float>(count);   // → [0, count)
    } else {
        // ClampEdge 只夹"够不着 int 边界"即可：真正的钳位由 wrapCoord 完成，
        // 于是 u=0 处的左 tap 是 -1 → 钳到 0，两个 tap 相同 ⇒ 结果就是边缘纹素本身。
        s = std::clamp(s, -1.0f, static_cast<float>(count));
    }
    const float lo = std::floor(s);
    frac = s - lo;
    return static_cast<int>(lo);
}

}  // namespace

namespace {

// 某一级的宽高。级别不存在时返回 0，于是下游的下标计算全部走 count<=0 的兜底分支。
void levelSize(const Texture& t, int level, int& w, int& h) {
    if (level < 0 || level >= t.levelCount()) {
        w = 0;
        h = 0;
        return;
    }
    w = t.levels[static_cast<size_t>(level)].width;
    h = t.levels[static_cast<size_t>(level)].height;
}

}  // namespace

TexelF sampleNearestLevel(const Texture& t, int level, WrapMode wrapU, WrapMode wrapV, float u,
                          float v) {
    int w = 0, h = 0;
    levelSize(t, level, w, h);   // 用**该级**的宽高做索引：每级减半，用 level 0 的会把贴图放大两倍
    return toTexelF(t, t.texel(level, texelIndex(u, w, wrapU), texelIndex(v, h, wrapV)));
}

TexelF sampleBilinearLevel(const Texture& t, int level, WrapMode wrapU, WrapMode wrapV, float u,
                           float v) {
    int w = 0, h = 0;
    levelSize(t, level, w, h);
    float fu = 0.0f, fv = 0.0f;
    const int x0 = lowerTap(u * static_cast<float>(w) - 0.5f, w, wrapU, fu);
    const int y0 = lowerTap(v * static_cast<float>(h) - 0.5f, h, wrapV, fv);
    // 两个 tap 各自过一遍 wrapCoord：Repeat 下 x0 已经在范围内（这步是恒等），
    // ClampEdge 下它可能是 -1 或 count（这步把它钳回边界）。
    const int xa = wrapCoord(x0, w, wrapU);
    const int xb = wrapCoord(x0 + 1, w, wrapU);
    const int ya = wrapCoord(y0, h, wrapV);
    const int yb = wrapCoord(y0 + 1, h, wrapV);

    const TexelF c00 = toTexelF(t, t.texel(level, xa, ya));
    const TexelF c10 = toTexelF(t, t.texel(level, xb, ya));
    const TexelF c01 = toTexelF(t, t.texel(level, xa, yb));
    const TexelF c11 = toTexelF(t, t.texel(level, xb, yb));

    const float w00 = (1.0f - fu) * (1.0f - fv), w10 = fu * (1.0f - fv);
    const float w01 = (1.0f - fu) * fv, w11 = fu * fv;
    TexelF out;
    out.r = w00 * c00.r + w10 * c10.r + w01 * c01.r + w11 * c11.r;
    out.g = w00 * c00.g + w10 * c10.g + w01 * c01.g + w11 * c11.g;
    out.b = w00 * c00.b + w10 * c10.b + w01 * c01.b + w11 * c11.b;
    out.a = w00 * c00.a + w10 * c10.a + w01 * c01.a + w11 * c11.a;
    return out;
}

TexelF sampleNearest(const Texture& t, WrapMode wrapU, WrapMode wrapV, float u, float v) {
    return sampleNearestLevel(t, 0, wrapU, wrapV, u, v);
}

TexelF sampleBilinear(const Texture& t, WrapMode wrapU, WrapMode wrapV, float u, float v) {
    return sampleBilinearLevel(t, 0, wrapU, wrapV, u, v);
}

Lod lodFromGradients(float texelsU, float texelsV, int levelCount, Vec2 gradU, Vec2 gradV) {
    // gradU = (∂u/∂x, ∂u/∂y)、gradV = (∂v/∂x, ∂v/∂y)；标量版的顺序是
    // (∂u/∂x, ∂v/∂x, ∂u/∂y, ∂v/∂y) —— 转置只在这一行发生，别处不必再想一遍。
    return lodFromDerivatives(texelsU, texelsV, levelCount, gradU.x, gradV.x, gradU.y, gradV.y);
}

TexelF sampleTrilinearLevel(const Texture& t, WrapMode wrapU, WrapMode wrapV, float u, float v,
                            const Lod& lod) {
    const TexelF lo = sampleBilinearLevel(t, lod.level, wrapU, wrapV, u, v);
    if (lod.frac <= 0.0f) return lo;   // 没有下一级可混（含 useMip=false 与末级两种情况）
    const TexelF hi = sampleBilinearLevel(t, lod.level + 1, wrapU, wrapV, u, v);
    const float f = lod.frac;
    TexelF out;
    out.r = lo.r + f * (hi.r - lo.r);
    out.g = lo.g + f * (hi.g - lo.g);
    out.b = lo.b + f * (hi.b - lo.b);
    out.a = lo.a + f * (hi.a - lo.a);
    return out;
}

TexelF sampleTrilinear(const Texture& t, WrapMode wrapU, WrapMode wrapV, float u, float v,
                       Vec2 gradU, Vec2 gradV) {
    const Lod lod = lodFromGradients(static_cast<float>(t.width), static_cast<float>(t.height),
                                     t.levelCount(), gradU, gradV);
    return sampleTrilinearLevel(t, wrapU, wrapV, u, v, lod);
}

TexelF sample(const Texture& t, const SamplerState& s, float u, float v, Vec2 gradU, Vec2 gradV) {
    Lod lod;   // useMip=false ⇒ 停在 {0,0}，三种 filter 都只看 level 0
    if (s.useMip) {
        lod = lodFromGradients(static_cast<float>(t.width), static_cast<float>(t.height),
                               t.levelCount(), gradU, gradV);
    }
    switch (s.filter) {
        case FilterMode::Nearest: return sampleNearestLevel(t, lod.level, s.wrapU, s.wrapV, u, v);
        case FilterMode::Bilinear:
            return sampleBilinearLevel(t, lod.level, s.wrapU, s.wrapV, u, v);
        case FilterMode::Trilinear:
        default: return sampleTrilinearLevel(t, s.wrapU, s.wrapV, u, v, lod);
    }
}

}  // namespace sr
