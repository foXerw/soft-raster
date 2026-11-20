#include "render/color.h"

#include <cmath>

namespace sr {
namespace {

constexpr int kEncodeLutCount = 4096;  // 输入域细分到 12bit；理由见测试 srgb_lut_error_...

float encodeAnalytic(float v) {
    // IEC 61966-2-1。线性段与幂段的接缝在 0.0031308。
    if (v <= 0.0031308f) return 12.92f * v;
    return 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

float decodeAnalytic(float s) {
    if (s <= 0.04045f) return s / 12.92f;
    return std::pow((s + 0.055f) / 1.055f, 2.4f);
}

float clamp01(float v) {
    // 注意 `!(v > 0.0f)` 而不是 `v < 0.0f`：NaN 走前者，于是未定义输入也有定义输出
    // （测试 srgb_encode_clamps_out_of_range_input 把它定死为 0）。
    if (!(v > 0.0f)) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

struct EncodeLut {
    uint8_t byte[kEncodeLutCount + 1];
    EncodeLut() {
        for (int i = 0; i <= kEncodeLutCount; ++i) {
            const float v = static_cast<float>(i) / static_cast<float>(kEncodeLutCount);
            byte[i] = static_cast<uint8_t>(std::lround(encodeAnalytic(v) * 255.0f));
        }
    }
};

struct DecodeLut {
    float linear[256];
    DecodeLut() {
        for (int i = 0; i < 256; ++i) {
            linear[i] = decodeAnalytic(static_cast<float>(i) / 255.0f);
        }
    }
};

const EncodeLut& encodeLut() {
    static const EncodeLut lut;
    return lut;
}
const DecodeLut& decodeLut() {
    static const DecodeLut lut;
    return lut;
}

}  // namespace

uint32_t packBGRA(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

uint8_t linearToSrgb8(float v) {
    const float c = clamp01(v);
    // 12bit 索引 + 就近取整：误差锁在 1 LSB 内，比每通道一次 pow 快一个数量级。
    const int idx = static_cast<int>(std::lround(c * static_cast<float>(kEncodeLutCount)));
    return encodeLut().byte[idx];
}

float srgb8ToLinear(uint8_t v) { return decodeLut().linear[v]; }

uint32_t encodeSRGB(const LinearColor& c, float alpha) {
    const float a = clamp01(alpha);
    return packBGRA(linearToSrgb8(c.r), linearToSrgb8(c.g), linearToSrgb8(c.b),
                    static_cast<uint8_t>(std::lround(a * 255.0f)));
}

LinearColor decodeSRGB(uint32_t packed) {
    LinearColor c;
    c.r = srgb8ToLinear(static_cast<uint8_t>((packed >> 16) & 0xFFu));
    c.g = srgb8ToLinear(static_cast<uint8_t>((packed >> 8) & 0xFFu));
    c.b = srgb8ToLinear(static_cast<uint8_t>(packed & 0xFFu));
    return c;
}

}  // namespace sr
