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

TexelF sampleNearest(const Texture& t, WrapMode wrapU, WrapMode wrapV, float u, float v) {
    const int x = texelIndex(u, t.width, wrapU);
    const int y = texelIndex(v, t.height, wrapV);
    const uint8_t* px = t.texel(0, x, y);
    if (px == nullptr) return TexelF{};      // 空纹理。到这里仍然返回有定义的值，不放 NaN 出去
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

}  // namespace sr
