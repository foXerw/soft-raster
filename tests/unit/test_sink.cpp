#include "render/sink.h"

#include <cmath>

#include "harness.h"
#include "render/color.h"
#include "test_util.h"

SR_TEST(depth_view_maps_near_to_bright_and_far_to_dark) {
    // 方向必须显式决定并 pin 住：读深度图时"亮=近"还是"亮=远"决定了你能不能一眼看出 bug。
    // 约定：近 = 白，远 = 黑（与 depth 数值相反，因为 1.0 是"最远"）。
    Canvas c{4, 4};
    c.rt.depth.clear(1.0f);
    c.rt.depth.write(0, 0, 0.0f);
    c.rt.depth.write(3, 3, 1.0f);
    sr::DebugViewSink sink;
    sink.view = sr::DebugViewSink::View::Depth;
    sink.depthNear = 0.0f;
    sink.depthFar = 1.0f;
    sink.present(c.rt);

    const uint32_t nearPix = c.rt.color.readPacked(0, 0);
    const uint32_t farPix = c.rt.color.readPacked(3, 3);
    SR_ASSERT_EQ(nearPix & 0xFF, 255u);          // 近：全亮
    SR_ASSERT_EQ(farPix & 0xFF, 0u);             // 远：全暗
    SR_ASSERT(nearPix > farPix);
}

SR_TEST(depth_view_normalizes_between_the_given_bounds_and_saturates) {
    Canvas c{4, 1};
    c.rt.depth.write(0, 0, 0.10f);
    c.rt.depth.write(1, 0, 0.25f);
    c.rt.depth.write(2, 0, 0.75f);
    c.rt.depth.write(3, 0, 0.90f);
    sr::DebugViewSink sink;
    sink.view = sr::DebugViewSink::View::Depth;
    sink.depthNear = 0.25f;
    sink.depthFar = 0.75f;
    sink.present(c.rt);

    const int b0 = c.rt.color.readPacked(0, 0) & 0xFF;
    const int b1 = c.rt.color.readPacked(1, 0) & 0xFF;
    const int b2 = c.rt.color.readPacked(2, 0) & 0xFF;
    const int b3 = c.rt.color.readPacked(3, 0) & 0xFF;
    SR_ASSERT_EQ(b0, 255);   // 0.10 比 near 还近 ⇒ 饱和到亮端
    SR_ASSERT_EQ(b1, 255);   // 正好在 near ⇒ 亮
    SR_ASSERT_EQ(b2, 0);     // 正好在 far ⇒ 暗
    SR_ASSERT_EQ(b3, 0);     // 比 far 还远 ⇒ 饱和到暗端
}

SR_TEST(color_view_is_the_identity) {
    Canvas c{4, 4};
    c.rt.color.clear(0xFF112233u);
    sr::DebugViewSink sink;
    sink.view = sr::DebugViewSink::View::Color;
    sink.present(c.rt);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) SR_ASSERT_EQ(c.rt.color.readPacked(x, y), 0xFF112233u);
}

SR_TEST(gray_view_uses_only_the_linear_channels) {
    Canvas c{2, 2};
    c.rt.color.writePacked(0, 0, sr::encodeSRGB({1.0f, 1.0f, 1.0f}));
    c.rt.color.writePacked(1, 0, sr::encodeSRGB({0.0f, 0.0f, 0.0f}));
    sr::DebugViewSink sink;
    sink.view = sr::DebugViewSink::View::Gray;
    sink.present(c.rt);
    SR_ASSERT_EQ(c.rt.color.readPacked(0, 0) & 0xFF, 255u);
    SR_ASSERT_EQ(c.rt.color.readPacked(1, 0) & 0xFF, 0u);
}

SR_TEST(debug_view_never_writes_depth) {
    // sink 只改写颜色。若它哪天开始写深度，"切到深度视图再切回来"就会改变渲染结果，
    // 那种 bug 极难归因（症状是调试视图本身让画面变了）。
    Canvas c{8, 8};
    c.rt.color.clear(0xFF000000u);
    c.rt.depth.clear(0.4f);
    sr::DebugViewSink sink;
    sink.view = sr::DebugViewSink::View::Depth;
    sink.present(c.rt);
    sink.view = sr::DebugViewSink::View::Gray;
    sink.present(c.rt);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) SR_ASSERT_NEAR(c.rt.depth.read(x, y), 0.4f, 1e-6f);
}

SR_TEST(sink_is_polymorphic_through_the_base_interface) {
    Canvas c{4, 4};
    c.rt.color.clear(0xFF112233u);
    sr::DebugViewSink d;
    d.view = sr::DebugViewSink::View::Color;
    sr::OutputSink& base = d;          // app 层只通过基类持有，才能把 debug 换成 stats
    base.present(c.rt);
    SR_ASSERT_EQ(c.rt.color.readPacked(0, 0), 0xFF112233u);
}
