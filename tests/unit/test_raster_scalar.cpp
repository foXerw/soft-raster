#include "render/raster_scalar.h"

#include <cstdint>
#include <vector>

#include "harness.h"
#include "math/vec.h"
#include "render/color.h"
#include "render/render_target.h"

using sr::ColorTarget;
using sr::FlatTriangle;

namespace {

constexpr uint32_t kClear = 0xFF000000u;

struct Canvas {
    std::vector<uint8_t> colorMem;
    std::vector<float> depthMem;
    sr::RenderTarget rt;

    explicit Canvas(int w, int h)
        : colorMem(static_cast<size_t>(w) * h * 4u, 0), depthMem(static_cast<size_t>(w) * h, 1.0f) {
        rt.color = ColorTarget{reinterpret_cast<uint32_t*>(colorMem.data()), w, h, w * 4};
        rt.depth = sr::DepthTarget{depthMem.data(), w, h};
        rt.color.clear(kClear);
    }
};

int countPixelsOf(const ColorTarget& t, uint32_t packed) {
    int n = 0;
    for (int y = 0; y < t.height; ++y)
        for (int x = 0; x < t.width; ++x)
            if (t.readPacked(x, y) == packed) ++n;
    return n;
}

}  // namespace

SR_TEST(interior_pixels_are_covered_and_exterior_ones_are_not) {
    // 顶点放在**像素中心**上（(1.5,1.5) 是像素 (1,1) 的中心）。
    // 第一版把顶点写成整数坐标 (2,2) 然后断言像素 (2,2) 被覆盖 —— 那是错的：
    // (2,2) 的像素中心是 (2.5,2.5)，离顶点 0.707 远，不在三角形里也是可能的。
    // 更重要的是：**顶点像素本身不保证被覆盖**，因为它同时落在两条边上，
    // 而 fill rule 只让顶边/左边的边界像素归属本三角形。所以这里断言的是
    // "严格内部的点"，不是"顶点"。
    Canvas c{16, 16};
    const uint32_t col = sr::encodeSRGB({1, 0, 0});
    const FlatTriangle t{{sr::Vec2{1.5f, 1.5f}, sr::Vec2{13.5f, 1.5f}, sr::Vec2{1.5f, 13.5f}}, col};
    sr::rasterizeFlatTriangle(c.rt.color, t);

    // 斜边是 x + y = 15（像素中心坐标），内部是 x + y < 15 且 x,y > 1.5
    SR_ASSERT_EQ(c.rt.color.readPacked(2, 2), col);     // (2.5,2.5) 深处内部
    SR_ASSERT_EQ(c.rt.color.readPacked(11, 2), col);    // (11.5,2.5) 靠近斜边但仍在内
    SR_ASSERT_EQ(c.rt.color.readPacked(2, 11), col);
    SR_ASSERT_EQ(c.rt.color.readPacked(14, 14), kClear);  // 完全在外
    SR_ASSERT_EQ(c.rt.color.readPacked(0, 0), kClear);    // 左上角在外（x<1.5）
}

SR_TEST(a_point_clearly_outside_is_not_written) {
    Canvas c{16, 16};
    const uint32_t col = sr::encodeSRGB({0, 1, 0});
    const FlatTriangle t{{sr::Vec2{2, 2}, sr::Vec2{8, 2}, sr::Vec2{2, 8}}, col};
    sr::rasterizeFlatTriangle(c.rt.color, t);
    SR_ASSERT_EQ(c.rt.color.readPacked(14, 14), kClear);
    SR_ASSERT_EQ(c.rt.color.readPacked(12, 3), kClear);
}

SR_TEST(square_split_on_diagonal_has_no_gap_and_no_overlap) {
    // 4x4 像素、顶点落在像素中心 (0.5, 4.5)。两个三角形共享一条对角线。
    // 判据用**覆盖计数之和**而不是手算 mask：
    //   缝隙 ⇒ 有像素一次都没被覆盖 ⇒ 总和 < 16
    //   双写 ⇒ 有像素被两个三角形都算进去 ⇒ 总和 > 16
    // 于是这一条不需要知道 fill rule 的细节就能把规则唯一化（Review Focus 4）。
    Canvas c{4, 4};
    const uint32_t colA = sr::encodeSRGB({1, 0, 0});
    const uint32_t colB = sr::encodeSRGB({0, 0, 1});

    sr::RasterStats s1, s2;
    const FlatTriangle a{{sr::Vec2{0.5f, 0.5f}, sr::Vec2{4.5f, 0.5f}, sr::Vec2{4.5f, 4.5f}}, colA};
    const FlatTriangle b{{sr::Vec2{0.5f, 0.5f}, sr::Vec2{4.5f, 4.5f}, sr::Vec2{0.5f, 4.5f}}, colB};
    sr::rasterizeFlatTriangle(c.rt.color, a, &s1);
    sr::rasterizeFlatTriangle(c.rt.color, b, &s2);

    SR_ASSERT_EQ(s1.pixelsCovered + s2.pixelsCovered, 16);
    SR_ASSERT_EQ(s1.pixelsWritten + s2.pixelsWritten, 16);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) SR_ASSERT(c.rt.color.readPacked(x, y) != kClear);
}

SR_TEST(square_split_on_the_other_diagonal_has_no_gap_and_no_overlap) {
    // 两个劈分方向都测：只有一方向失败 ⇒ top/left 规则的符号错了。
    Canvas c{4, 4};
    const uint32_t colA = sr::encodeSRGB({1, 1, 0});
    const uint32_t colB = sr::encodeSRGB({0, 1, 1});

    sr::RasterStats s1, s2;
    const FlatTriangle a{{sr::Vec2{0.5f, 0.5f}, sr::Vec2{4.5f, 0.5f}, sr::Vec2{0.5f, 4.5f}}, colA};
    const FlatTriangle b{{sr::Vec2{4.5f, 0.5f}, sr::Vec2{4.5f, 4.5f}, sr::Vec2{0.5f, 4.5f}}, colB};
    sr::rasterizeFlatTriangle(c.rt.color, a, &s1);
    sr::rasterizeFlatTriangle(c.rt.color, b, &s2);

    SR_ASSERT_EQ(s1.pixelsCovered + s2.pixelsCovered, 16);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) SR_ASSERT(c.rt.color.readPacked(x, y) != kClear);
}

SR_TEST(degenerate_triangle_writes_nothing) {
    Canvas c{16, 16};
    sr::RasterStats s;
    const FlatTriangle line{{sr::Vec2{1, 1}, sr::Vec2{5, 5}, sr::Vec2{9, 9}}, sr::encodeSRGB({1, 1, 1})};
    sr::rasterizeFlatTriangle(c.rt.color, line, &s);
    SR_ASSERT_EQ(s.pixelsCovered, 0);
    SR_ASSERT_EQ(s.pixelsWritten, 0);
    SR_ASSERT_EQ(s.trianglesRejected, 1);
    SR_ASSERT_EQ(countPixelsOf(c.rt.color, sr::encodeSRGB({1, 1, 1})), 0);

    // 重合顶点同样要拒
    const FlatTriangle dup{{sr::Vec2{3, 3}, sr::Vec2{3, 3}, sr::Vec2{7, 7}}, sr::encodeSRGB({1, 1, 1})};
    sr::RasterStats s2;
    sr::rasterizeFlatTriangle(c.rt.color, dup, &s2);
    SR_ASSERT_EQ(s2.trianglesRejected, 1);
    SR_ASSERT_EQ(s2.pixelsWritten, 0);
}

SR_TEST(non_finite_vertices_are_rejected_not_rasterized) {
    // 症状对照：NaN 顶点如果进了 inner loop，边界算出来是空集或全集，
    // 前者静默、后者会把一大片区域涂掉。两种都比"报错"难查。
    Canvas c{16, 16};
    sr::RasterStats s;
    const float nan = std::nanf("");
    const FlatTriangle bad{{sr::Vec2{nan, 1}, sr::Vec2{8, 2}, sr::Vec2{2, 9}}, sr::encodeSRGB({1, 1, 1})};
    sr::rasterizeFlatTriangle(c.rt.color, bad, &s);
    SR_ASSERT_EQ(s.trianglesRejected, 1);
    SR_ASSERT_EQ(s.pixelsWritten, 0);

    const FlatTriangle inf{{sr::Vec2{1, 1}, sr::Vec2{INFINITY, 2}, sr::Vec2{2, 9}},
                           sr::encodeSRGB({1, 1, 1})};
    sr::RasterStats s2;
    sr::rasterizeFlatTriangle(c.rt.color, inf, &s2);
    SR_ASSERT_EQ(s2.trianglesRejected, 1);
}

SR_TEST(triangle_fully_outside_target_is_clipped_by_bbox) {
    Canvas c{8, 8};
    sr::RasterStats s;
    const FlatTriangle far{{sr::Vec2{100, 100}, sr::Vec2{200, 100}, sr::Vec2{100, 200}},
                           sr::encodeSRGB({1, 1, 1})};
    sr::rasterizeFlatTriangle(c.rt.color, far, &s);
    SR_ASSERT_EQ(s.pixelsWritten, 0);
    SR_ASSERT_EQ(countPixelsOf(c.rt.color, sr::encodeSRGB({1, 1, 1})), 0);

    // 部分在外：只允许写界内的像素（越界写会踩内存，这里靠 ColorTarget 的边界检查兜住）
    const FlatTriangle partial{{sr::Vec2{-20, -20}, sr::Vec2{40, -20}, sr::Vec2{-20, 40}},
                               sr::encodeSRGB({0, 1, 0})};
    sr::RasterStats s2;
    sr::rasterizeFlatTriangle(c.rt.color, partial, &s2);
    SR_ASSERT(s2.pixelsWritten > 0);
    SR_ASSERT(s2.pixelsWritten <= 8 * 8);
    SR_ASSERT(s2.pixelsCovered >= s2.pixelsWritten);   // 覆盖 >= 写入（界外的覆盖被丢弃）
}

SR_TEST(covered_count_matches_picks_theorem_plus_the_top_left_boundary_runs) {
    // 这条测试原本写的是"覆盖数 ≈ 面积 32"，实测 36 —— 不是 bug，是我算错了两件事：
    //   1. 像素中心计数本来就不等于面积，误差量级是周长的一半。
    //   2. top-left 规则会把**顶边与左边**上的边界像素算进来（斜边不算）。
    // 精确推导（顶点 (0,0),(8,0),(0,8) 平移到像素中心格点）：
    //   Pick 定理：A = I + B/2 - 1 ⇒ 32 = I + 24/2 - 1 ⇒ I = 21 个严格内部点
    //   顶边上的边界像素：j=0, i=0..7 ⇒ 8 个
    //   左边上的边界像素：i=0, j=0..7 ⇒ 8 个
    //   左上角 (0,0) 被两条边重复计数 ⇒ -1
    //   合计 21 + 8 + 8 - 1 = 36
    // 斜边上的 8 个边界点一个都不算进来（它既不是顶边也不是左边）—— 那正是
    // "相邻三角形不重不漏"的另一半证明。
    Canvas c{16, 16};
    sr::RasterStats s;
    const FlatTriangle t{{sr::Vec2{0.5f, 0.5f}, sr::Vec2{8.5f, 0.5f}, sr::Vec2{0.5f, 8.5f}},
                         sr::encodeSRGB({1, 1, 1})};
    sr::rasterizeFlatTriangle(c.rt.color, t, &s);
    SR_ASSERT_EQ(s.pixelsCovered, 36);
}

SR_TEST(raster_stats_accumulate_across_calls) {
    Canvas c{16, 16};
    sr::RasterStats s;
    const FlatTriangle t1{{sr::Vec2{1, 1}, sr::Vec2{5, 1}, sr::Vec2{1, 5}}, sr::encodeSRGB({1, 0, 0})};
    const FlatTriangle t2{{sr::Vec2{9, 9}, sr::Vec2{13, 9}, sr::Vec2{9, 13}}, sr::encodeSRGB({0, 1, 0})};
    sr::rasterizeFlatTriangle(c.rt.color, t1, &s);
    const int afterFirst = s.pixelsWritten;
    sr::rasterizeFlatTriangle(c.rt.color, t2, &s);
    SR_ASSERT(s.pixelsWritten > afterFirst);
    SR_ASSERT_EQ(s.trianglesRejected, 0);
}
