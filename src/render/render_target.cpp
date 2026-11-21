#include "render/render_target.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

namespace sr {

namespace {

// 无符号化技巧：负数会变成极大的无符号数，所以一次比较同时挡住"负"和"越界"。
// 越界访问在这个项目里是结构性风险（tile、bbox、裁剪夹取都会算坐标），
// 所以颜色与深度的边界检查不是防御式冗余，而是接口承诺的一部分。
inline bool inRange(int v, int size) {
    return static_cast<unsigned>(v) < static_cast<unsigned>(size);
}

}  // namespace

void ColorTarget::clear(uint32_t packed) {
    const size_t stride = static_cast<size_t>(pitch) / 4u;
    for (int y = 0; y < height; ++y) {
        // 只填 [0, width) 个像素：pitch 的尾随填充字节不属于这块图像。
        std::fill_n(pixels + static_cast<size_t>(y) * stride, static_cast<size_t>(width), packed);
    }
}

void ColorTarget::writePacked(int x, int y, uint32_t packed) {
    if (!contains(x, y)) return;
    pixels[static_cast<size_t>(y) * (static_cast<size_t>(pitch) / 4u) + static_cast<size_t>(x)] = packed;
}

uint32_t ColorTarget::readPacked(int x, int y) const {
    if (!contains(x, y)) return 0u;
    return pixels[static_cast<size_t>(y) * (static_cast<size_t>(pitch) / 4u) + static_cast<size_t>(x)];
}

bool ColorTarget::contains(int x, int y) const {
    return inRange(x, width) && inRange(y, height);
}

void DepthTarget::clear(float v) {
    for (int y = 0; y < height; ++y) {
        std::fill_n(values + static_cast<size_t>(y) * static_cast<size_t>(stride),
                    static_cast<size_t>(width), v);
    }
}

void DepthTarget::write(int x, int y, float d) {
    if (!contains(x, y)) return;
    values[static_cast<size_t>(y) * static_cast<size_t>(stride) + static_cast<size_t>(x)] = d;
}

float DepthTarget::read(int x, int y) const {
    if (!contains(x, y)) return std::numeric_limits<float>::infinity();
    return values[static_cast<size_t>(y) * static_cast<size_t>(stride) + static_cast<size_t>(x)];
}

bool DepthTarget::contains(int x, int y) const {
    return inRange(x, width) && inRange(y, height);
}

DepthTarget DepthTarget::view(int x0, int y0, int w, int h) const {
    // 越界在 Debug 下直接断言：tile 网格的除法一旦算错，静默夹取会把错误写到邻居 tile 里，
    // 那种 bug 在画面上表现为"偶发的一个像素错"，几乎不可能定位。
    assert(x0 >= 0 && y0 >= 0 && w > 0 && h > 0 && x0 + w <= width && y0 + h <= height);
    // Release 也要夹取（计划 Task 0.4 Step 3 就要求，复审 I2 发现只有 assert 等于没做）：
    // NDEBUG 下 assert 消失，越界的 view 会算出一个越界基址 + 未检查的宽高 ⇒ 静默写坏邻居内存。
    int cx0 = x0 < 0 ? 0 : x0;
    int cy0 = y0 < 0 ? 0 : y0;
    int cw = w, ch = h;
    if (cx0 + cw > width) cw = width - cx0;
    if (cy0 + ch > height) ch = height - cy0;
    if (cw < 0) cw = 0;
    if (ch < 0) ch = 0;
    DepthTarget v;
    v.values = values + static_cast<size_t>(cy0) * static_cast<size_t>(stride) + static_cast<size_t>(cx0);
    v.width = cw;
    v.height = ch;
    v.stride = stride;          // 关键：跨度继承父缓冲
    return v;
}

}  // namespace sr
