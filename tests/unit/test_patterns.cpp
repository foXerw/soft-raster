#include "render/patterns.h"

#include <cmath>
#include <cstdint>
#include <vector>

#include "harness.h"
#include "render/color.h"
#include "render/render_target.h"

namespace {

sr::RenderTarget makeTarget(int w, int h, std::vector<uint8_t>& colorMem, std::vector<float>& depthMem) {
    colorMem.assign(static_cast<size_t>(w) * h * 4, 0);
    depthMem.assign(static_cast<size_t>(w) * h, 1.0f);
    sr::RenderTarget rt;
    rt.color = sr::ColorTarget{reinterpret_cast<uint32_t*>(colorMem.data()), w, h, w * 4};
    rt.depth = sr::DepthTarget{depthMem.data(), w, h};
    return rt;
}

// 从 packed 里取出某个通道的 sRGB 字节。
int channel(uint32_t packed, char which) {
    switch (which) {
        case 'r': return (packed >> 16) & 0xFF;
        case 'g': return (packed >> 8) & 0xFF;
        case 'b': return packed & 0xFF;
        default: return 0;
    }
}

}  // namespace

SR_TEST(fill_linear_gradient_endpoints_are_the_given_colors) {
    std::vector<uint8_t> cm;
    std::vector<float> dm;
    auto rt = makeTarget(64, 8, cm, dm);
    const sr::LinearColor a{0.0f, 0.0f, 0.0f}, b{1.0f, 1.0f, 1.0f};
    sr::fillLinearGradient(rt.color, a, b, 0.0f);

    SR_ASSERT_EQ(channel(rt.color.readPacked(0, 0), 'g'), sr::linearToSrgb8(0.0f));
    SR_ASSERT_EQ(channel(rt.color.readPacked(63, 7), 'g'), sr::linearToSrgb8(1.0f));
    // 每一行都一样：图案是水平渐变，纵向不该有任何变化。
    for (int y = 1; y < 8; ++y)
        SR_ASSERT_EQ(rt.color.readPacked(31, y), rt.color.readPacked(31, 0));
}

SR_TEST(fill_linear_gradient_is_monotonic_at_zero_phase) {
    std::vector<uint8_t> cm;
    std::vector<float> dm;
    auto rt = makeTarget(64, 1, cm, dm);
    sr::fillLinearGradient(rt.color, {0, 0, 0}, {1, 1, 1}, 0.0f);
    int prev = -1;
    for (int x = 0; x < 64; ++x) {
        const int v = channel(rt.color.readPacked(x, 0), 'g');
        SR_ASSERT(v >= prev);
        prev = v;
    }
}

SR_TEST(fill_linear_gradient_phase_shifts_without_leaving_a_step) {
    // phase 用三角波（mirror）而不是 fract：平移渐变时**不允许**出现接缝。
    // 有接缝的症状是画面上有一条竖线，作为"测试图案"它是有害的——你会以为那是渲染 bug。
    std::vector<uint8_t> cm;
    std::vector<float> dm;
    auto rt = makeTarget(64, 1, cm, dm);
    sr::fillLinearGradient(rt.color, {0, 0, 0}, {1, 1, 1}, 0.25f);
    int maxDelta = 0;
    for (int x = 1; x < 64; ++x) {
        const int d = std::abs(channel(rt.color.readPacked(x, 0), 'g') -
                               channel(rt.color.readPacked(x - 1, 0), 'g'));
        if (d > maxDelta) maxDelta = d;
    }
    // 64 列铺满 0..255 的理想单列步长约 4；容许 3 倍以覆盖 mirror 折返处的格点误差。
    SR_ASSERT(maxDelta <= 12);
    SR_ASSERT(maxDelta >= 1);   // 真的在变化，不是画了一片常数
}

SR_TEST(fill_checker_alternates_by_cell_size) {
    std::vector<uint8_t> cm;
    std::vector<float> dm;
    auto rt = makeTarget(4, 4, cm, dm);
    const sr::LinearColor dark{0.1f, 0.1f, 0.1f}, light{0.9f, 0.9f, 0.9f};
    sr::fillChecker(rt.color, dark, light, 2);

    const uint32_t c00 = rt.color.readPacked(0, 0);
    SR_ASSERT_EQ(c00, rt.color.readPacked(2, 2));      // 同格
    SR_ASSERT_EQ(c00, rt.color.readPacked(1, 1));      // 格内不越过 cell=2 的边界
    SR_ASSERT(c00 != rt.color.readPacked(2, 0));       // 隔壁格是另一种颜色
    SR_ASSERT_EQ(rt.color.readPacked(2, 0), rt.color.readPacked(0, 2));
    SR_ASSERT_EQ(channel(c00, 'g'), sr::linearToSrgb8(0.1f));
    SR_ASSERT_EQ(channel(rt.color.readPacked(2, 0), 'g'), sr::linearToSrgb8(0.9f));
}

SR_TEST(fill_checker_cell_size_one_is_a_pixel_grid) {
    std::vector<uint8_t> cm;
    std::vector<float> dm;
    auto rt = makeTarget(4, 4, cm, dm);
    sr::fillChecker(rt.color, {0, 0, 0}, {1, 1, 1}, 1);
    SR_ASSERT_EQ(rt.color.readPacked(0, 0), rt.color.readPacked(2, 0));
    SR_ASSERT(rt.color.readPacked(1, 0) != rt.color.readPacked(2, 0));
    SR_ASSERT_EQ(rt.color.readPacked(1, 0), rt.color.readPacked(1, 2));
}

SR_TEST(patterns_never_touch_the_depth_attachment) {
    std::vector<uint8_t> cm;
    std::vector<float> dm;
    auto rt = makeTarget(16, 16, cm, dm);
    sr::fillChecker(rt.color, {0, 0, 0}, {1, 1, 1}, 4);
    sr::fillLinearGradient(rt.color, {0, 0, 0}, {1, 1, 1}, 0.3f);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) SR_ASSERT_NEAR(rt.depth.read(x, y), 1.0f, 1e-6f);
}
