#include "render/patterns.h"

#include <cmath>

namespace sr {

void fillLinearGradient(ColorTarget& target, const LinearColor& a, const LinearColor& b, float phase) {
    if (target.width <= 0 || target.height <= 0) return;

    // 每一列只算一次，再复制到各行：既是快，也让"图案是水平的"这个事实出现在代码里
    // 而不是只出现在测试断言里。
    for (int y = 0; y < target.height; ++y) {
        for (int x = 0; x < target.width; ++x) {
            const float t = (target.width > 1) ? static_cast<float>(x) / static_cast<float>(target.width - 1)
                                               : 0.0f;
            // 周期 2 的三角波（镜像折返），对任意 phase 都连续，不会留竖线。
            const float v = t + phase;
            const float s = v - 2.0f * std::floor(v * 0.5f);  // 折到 [0, 2)
            const float u = (s <= 1.0f) ? s : (2.0f - s);     // 再折回 [0, 1]
            const LinearColor c{a.r + (b.r - a.r) * u, a.g + (b.g - a.g) * u, a.b + (b.b - a.b) * u};
            target.writePacked(x, y, encodeSRGB(c));
        }
    }
}

void fillChecker(ColorTarget& target, const LinearColor& a, const LinearColor& b, int cellPx) {
    const int cell = (cellPx < 1) ? 1 : cellPx;
    for (int y = 0; y < target.height; ++y) {
        for (int x = 0; x < target.width; ++x) {
            const bool odd = (((x / cell) + (y / cell)) & 1) != 0;
            target.writePacked(x, y, encodeSRGB(odd ? b : a));
        }
    }
}

}  // namespace sr
