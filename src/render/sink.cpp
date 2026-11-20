#include "render/sink.h"

#include <algorithm>
#include <cmath>

#include "render/color.h"

namespace sr {
namespace {

inline float grayToByte(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

}  // namespace

void DebugViewSink::present(RenderTarget& target) {
    switch (view) {
        case View::Color:
            return;   // 恒等：调试视图绝不能改变正常显示

        case View::Depth: {
            // 近 = 亮，远 = 暗（与 depth 数值相反，因为 1.0 是最远）。
            // 归一化到 [depthNear, depthFar] 再取反：直接画 depth 的话，
            // 场景里几乎所有东西都挤在 1.0 附近（非线性深度的固有性质），
            // 看起来就是一片白 —— 那个"看起来正常"的视图毫无诊断价值。
            const float span = depthFar - depthNear;
            const float inv = (std::fabs(span) > 1e-9f) ? 1.0f / span : 0.0f;
            for (int y = 0; y < target.color.height; ++y) {
                for (int x = 0; x < target.color.width; ++x) {
                    const float d = target.depth.read(x, y);
                    const float t = (inv == 0.0f) ? 0.0f : (d - depthNear) * inv;
                    const float g = grayToByte(1.0f - t);
                    target.color.writePacked(x, y, encodeSRGB(LinearColor{g, g, g}));
                }
            }
            return;
        }

        case View::Gray: {
            for (int y = 0; y < target.color.height; ++y) {
                for (int x = 0; x < target.color.width; ++x) {
                    const LinearColor c = decodeSRGB(target.color.readPacked(x, y));
                    // 亮度系数在线性空间组合，然后再编码一次：
                    // 直接在 sRGB 字节上做加权平均会让亮色区域权重失真。
                    const float g = grayToByte(0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b);
                    target.color.writePacked(x, y, encodeSRGB(LinearColor{g, g, g}));
                }
            }
            return;
        }
    }
}

}  // namespace sr
