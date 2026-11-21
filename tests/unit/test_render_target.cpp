#include "render/render_target.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "harness.h"

namespace {

// 故意用不连续的 pitch，这样"行内偏移写对但行跨度写错"这类 bug 立刻暴露。
constexpr int kPitch = 64 * 4 + 16;

std::vector<uint8_t> makeMemory(int w, int h) {
    // 夹具本身的前提：pitch 必须容得下 width 个像素，否则下面所有断言都在测一个不存在的目标。
    SR_ASSERT(w * 4 <= kPitch);
    return std::vector<uint8_t>(static_cast<size_t>(kPitch) * h, 0);
}

}  // namespace

SR_TEST(color_target_roundtrip_with_nontrivial_pitch) {
    auto mem = makeMemory(64, 32);
    sr::ColorTarget t{reinterpret_cast<uint32_t*>(mem.data()), 64, 32, kPitch};

    t.clear(0xFF112233u);
    SR_ASSERT_EQ(t.readPacked(0, 0), 0xFF112233u);
    SR_ASSERT_EQ(t.readPacked(63, 31), 0xFF112233u);   // 右下角：行跨度写错时这里最先错

    t.writePacked(7, 5, 0xDEADBEEFu);
    SR_ASSERT_EQ(t.readPacked(7, 5), 0xDEADBEEFu);
    SR_ASSERT_EQ(t.readPacked(8, 5), 0xFF112233u);     // 邻居没被踩
    SR_ASSERT_EQ(t.readPacked(7, 6), 0xFF112233u);
}

SR_TEST(color_target_clears_only_the_declared_rows_and_cols) {
    auto mem = makeMemory(64, 32);
    sr::ColorTarget t{reinterpret_cast<uint32_t*>(mem.data()), 64, 32, kPitch};
    t.clear(0xFF000000u);
    // pitch 的填充字节必须留在目标之外：DIB 的 stride 尾随字节没人显示，
    // 但 golden 比对读的是 ColorTarget 的语义像素，所以越界写入必须不发生。
    const uint32_t* row1 = reinterpret_cast<const uint32_t*>(mem.data() + kPitch);
    SR_ASSERT_EQ(mem[kPitch - 4], 0);                  // 第 0 行的 padding 区保持 0
    SR_ASSERT_EQ(row1[-1], 0u);                        // 前一行 padding 读出来仍是 0
}

SR_TEST(color_target_out_of_bounds_writes_are_dropped_not_wrapped) {
    auto mem = makeMemory(16, 16);
    sr::ColorTarget t{reinterpret_cast<uint32_t*>(mem.data()), 16, 16, 16 * 4};
    t.clear(0xFF000000u);
    t.writePacked(-1, 0, 0xFF445566u);
    t.writePacked(0, 16, 0xFF445566u);
    t.writePacked(16, 0, 0xFF445566u);
    t.writePacked(0, -1, 0xFF445566u);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) SR_ASSERT_EQ(t.readPacked(x, y), 0xFF000000u);
    // 越界读也必须有定义的值（0），这样调试视图不会读到垃圾。
    SR_ASSERT_EQ(t.readPacked(-3, 5), 0u);
    SR_ASSERT_EQ(t.readPacked(100, 100), 0u);
}

SR_TEST(color_target_contains_matches_the_iteration_domain) {
    auto mem = makeMemory(8, 4);
    sr::ColorTarget t{reinterpret_cast<uint32_t*>(mem.data()), 8, 4, 8 * 4};
    SR_ASSERT(t.contains(0, 0));
    SR_ASSERT(t.contains(7, 3));
    SR_ASSERT_FALSE(t.contains(8, 3));
    SR_ASSERT_FALSE(t.contains(7, 4));
    SR_ASSERT_FALSE(t.contains(-1, 0));
}

SR_TEST(top_down_row_zero_is_the_top_edge) {
    // C8：第 0 行就是图像上边缘 ⇒ 行 1 的地址比行 0 **大**一个 pitch。
    auto mem = makeMemory(8, 2);
    sr::ColorTarget t{reinterpret_cast<uint32_t*>(mem.data()), 8, 2, 8 * 4};
    t.writePacked(0, 0, 0xFF000000u);   // 上边缘：黑
    t.writePacked(0, 1, 0xFF0000FFu);   // 下边缘：蓝
    SR_ASSERT_EQ(t.pixels[0], 0xFF000000u);                       // 内存起点即第 0 行
    SR_ASSERT_EQ(t.pixels[t.pitch / 4], 0xFF0000FFu);              // 加一个 pitch 是下一行
    SR_ASSERT_EQ(t.readPacked(0, 0), 0xFF000000u);
}

SR_TEST(depth_target_clear_default_is_far) {
    std::vector<float> buf(16 * 16, 0.0f);
    sr::DepthTarget d{buf.data(), 16, 16};
    d.clear();                                   // 约定：默认 clear 到 1.0（最远）
    SR_ASSERT_NEAR(d.read(0, 0), 1.0f, 1e-6f);
    SR_ASSERT_NEAR(d.read(15, 15), 1.0f, 1e-6f);
}

SR_TEST(depth_target_view_aliases_the_parent_buffer) {
    std::vector<float> buf(16 * 16, 1.0f);
    sr::DepthTarget d{buf.data(), 16, 16};
    auto sub = d.view(4, 4, 8, 8);
    sub.write(0, 0, 0.25f);                     // 子矩形的 (0,0) == 全局 (4,4)
    SR_ASSERT_NEAR(d.read(4, 4), 0.25f, 1e-6f); // **同一块内存**，不是拷贝
    SR_ASSERT_NEAR(d.read(5, 4), 1.0f, 1e-6f);
    SR_ASSERT_NEAR(d.read(4, 5), 1.0f, 1e-6f);
    SR_ASSERT_EQ(sub.width, 8);
    SR_ASSERT_EQ(sub.height, 8);
}

SR_TEST(depth_target_out_of_bounds_read_is_infinity) {
    std::vector<float> buf(8 * 8, 0.5f);
    sr::DepthTarget d{buf.data(), 8, 8};
    // +inf 是"必然失败"的深度：tile 边界外的测试不会碰巧通过（Backend B 的正确性依赖它）。
    SR_ASSERT(std::isinf(d.read(-1, 0)) && d.read(-1, 0) > 0.0f);
    SR_ASSERT(std::isinf(d.read(0, 8)) && d.read(0, 8) > 0.0f);
    // 越界写同样被丢弃，不能踩到缓冲外面。
    d.write(0, 8, 0.1f);
    SR_ASSERT_NEAR(d.read(0, 7), 0.5f, 1e-6f);
}

SR_TEST(render_target_bundles_the_two_attachments) {
    std::vector<uint8_t> color(4 * 4 * 4);
    std::vector<float> depth(4 * 4, 1.0f);
    sr::RenderTarget rt;
    rt.color = sr::ColorTarget{reinterpret_cast<uint32_t*>(color.data()), 4, 4, 4 * 4};
    rt.depth = sr::DepthTarget{depth.data(), 4, 4};
    rt.color.clear(0xFF000000u);
    rt.depth.clear();
    SR_ASSERT_EQ(rt.color.readPacked(3, 3), 0xFF000000u);
    SR_ASSERT_NEAR(rt.depth.read(3, 3), 1.0f, 1e-6f);
}
