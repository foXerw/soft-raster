#pragma once

#include <cstdint>

namespace sr {

// 颜色一律先用线性空间表示，只在写入颜色目标的那一瞬间编码成 sRGB。
// 原因见 docs/notes/M0.md：光照（点积、相加、乘系数）是线性运算，在 sRGB 值上做
// 会得到"整体发灰"的结果；反过来如果解码和编码各多做一次，暗部会死黑。
// 这两处正好是 Review Focus 5 的两种症状，所以上面那条测试和下面的往返测试都要保住。
struct LinearColor {
    float r{}, g{}, b{};
};

// 像素打包约定（设计文档 §5 与 C8）：DIB 32bpp 的内存字节序是 B,G,R,A，
// 因此按小端读成 uint32_t 时值是 0xAARRGGBB。
uint32_t packBGRA(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

// 线性 → sRGB 单通道，输入会被 clamp 到 [0,1]；NaN 与越界都返回一个合法字节（0）。
uint8_t linearToSrgb8(float v);
float srgb8ToLinear(uint8_t v);

// sRGB → 线性的**浮点**版（M5.1）。与 srgb8ToLinear 唯一的区别是**不经过 256 级量化**：
// MTL 的 Kd 本来就是浮点，走字节版会把 0.5 变成 128/255 再解码，白白吃掉一档。
// 输入 clamp 到 [0,1]，NaN 返回 0（与 linearToSrgb8 同一条"未定义输入也有定义输出"的理由）。
// 两端必须精确：Kd 1 1 1 解成非 1 会让"无 tint"不再成立，而 golden 是零容差比对。
float srgbToLinear(float v);
LinearColor srgbToLinear(const LinearColor& c);

// 整色编码成 0xAARRGGBB（走 LUT，这是它出现在每个像素出口的原因）。
uint32_t encodeSRGB(const LinearColor& c, float alpha = 1.0f);
LinearColor decodeSRGB(uint32_t packed);

}  // namespace sr
