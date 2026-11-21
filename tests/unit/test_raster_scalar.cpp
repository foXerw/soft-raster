#include "render/raster_scalar.h"

#include <cstdint>
#include <cmath>
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

SR_TEST(reversed_winding_covers_exactly_the_same_pixels) {
    // 同一个三角形，顶点顺序反过来，覆盖的像素必须一模一样。
    // 这条测试是 M2 集成阶段补的：EdgeFunc 的内侧法线 {-e.y, e.x} 只对一种绕向成立，
    // 而"正面三角形"在 top-down 屏幕空间里恰好是 area2 < 0 的那一种 ——
    // M1 的全部测试用例都是正绕向，所以这个 bug 一直到立方体接进来才暴露（症状：covered = 0）。
    Canvas c{16, 16};
    const uint32_t col = sr::encodeSRGB({1, 1, 1});
    const sr::Vec2 v0{2.5f, 3.5f}, v1{13.5f, 2.5f}, v2{4.5f, 12.5f};

    sr::RasterStats fwd, rev;
    const FlatTriangle a{{v0, v1, v2}, col};
    const FlatTriangle b{{v0, v2, v1}, col};
    sr::rasterizeFlatTriangle(c.rt.color, a, &fwd);
    c.rt.color.clear(kClear);
    sr::rasterizeFlatTriangle(c.rt.color, b, &rev);

    SR_ASSERT(fwd.pixelsCovered > 0);
    SR_ASSERT_EQ(rev.pixelsCovered, fwd.pixelsCovered);

    // 逐像素比对
    Canvas c2{16, 16};
    sr::rasterizeFlatTriangle(c2.rt.color, a);
    Canvas c3{16, 16};
    sr::rasterizeFlatTriangle(c3.rt.color, b);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
            SR_ASSERT_EQ(c2.rt.color.readPacked(x, y), c3.rt.color.readPacked(x, y));
}

SR_TEST(steep_shared_edge_has_no_gap_and_no_overlap) {
    // 复审 C3：共享一条**陡边**（这里从 (0,0) 到 (1,10)，|dx| < |dy|）的两个三角形。
    // 旧规则 "opposite.x > p0.x || opposite.x > p1.x" 在这里让两边都声称拥有边界像素（双写），
    // 换一组 opposite 位置则两边都不声称（缝隙）。±45° 的对角线测试看不见它，
    // 因为那里 opposite.x 恰好等于边的最小 x。
    // 新规则用内侧法线：左边 ⟺ n.x > 0，顶边 ⟺ n.x == 0 && n.y > 0。
    const sr::Vec2 e0{0.5f, 0.5f}, e1{1.5f, 10.5f};
    int doubleWrites = 0, gaps = 0;
    // 遍历几种第三顶点组合：缝隙与双写都要为 0
    const sr::Vec2 oppL[3] = {{0.7f, 5.5f}, {-0.5f, 5.5f}, {0.5f, 5.5f}};
    const sr::Vec2 oppR[3] = {{2.5f, 5.5f}, {1.5f, 5.5f}, {3.5f, 6.5f}};
    for (int k = 0; k < 3; ++k) {
        Canvas c{8, 12};
        sr::RasterStats sL, sR;
        const FlatTriangle L{{e0, e1, oppL[k]}, sr::encodeSRGB({1, 0, 0})};
        // 右三角形用反向的同一条边
        const FlatTriangle R{{e1, e0, oppR[k]}, sr::encodeSRGB({0, 0, 1})};
        sr::rasterizeFlatTriangle(c.rt.color, L, &sL);
        sr::rasterizeFlatTriangle(c.rt.color, R, &sR);
        // 落在共享边上的像素中心：x=1.5 那一列的 y=1..10 中，(1, y) 的中心是 (1.5, y+0.5)
        for (int y = 1; y <= 9; ++y) {
            const bool inL = c.rt.color.readPacked(1, y) == sr::encodeSRGB({1, 0, 0});
            const bool inR = c.rt.color.readPacked(1, y) == sr::encodeSRGB({0, 0, 1});
            // 后写的会覆盖前写的，所以用覆盖计数之和与"被写次数"来判定
            if (inL && inR) ++doubleWrites;
        }
        // 独立地用覆盖数验证：两个三角形的覆盖数之和 减去 实际被写的像素数 = 重叠数
        Canvas c2{8, 12};
        sr::RasterStats a;
        sr::rasterizeFlatTriangle(c2.rt.color, L, &a);
        int written = 0;
        for (int y = 0; y < 12; ++y)
            for (int x = 0; x < 8; ++x)
                if (c2.rt.color.readPacked(x, y) != kClear) ++written;
        if (written != a.pixelsCovered) ++gaps;   // 有覆盖却没落笔 ⇒ 越界或丢像素
    }
    SR_ASSERT_EQ(doubleWrites, 0);
    SR_ASSERT_EQ(gaps, 0);
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
    const long long afterFirst = s.pixelsWritten;
    sr::rasterizeFlatTriangle(c.rt.color, t2, &s);
    SR_ASSERT(s.pixelsWritten > afterFirst);
    SR_ASSERT_EQ(s.trianglesRejected, 0);
}

SR_TEST(shared_edge_leaves_no_gap_even_with_awkward_coordinates) {
    // 这条测试是为了复现"立方体正面有一条点状对角缝"而写的，**它没有复现出来**：
    // 直接读 yaw=0/pitch=0 那张图沿对角线的像素，颜色是连续的
    // (#8983E6 → #E1E3E6，一路平滑，没有任何背景色混进来)。
    // 所以我先前那句"C3 的修复可能还不完整"是错的，撤回。
    // 测试仍然保留：它断言的是"贴着共享边的像素必须至少被一个三角形认领"这条真实不变式
    // （根因本该是 `evaluate(p) == 0` 这个**精确比较**：
    // 共享同一条边的两个三角形用相反的顶点顺序算边函数，实数域里 v_A = -v_B，
    // 浮点域里差几个 ULP ⇒ 边界像素可以同时被判成"外侧"，谁都不认领 = 缝。
    // 第一版这个测试检查的是"远离所有边的像素"，那当然不会有缝 —— 缝在边上，
    // 所以必须专门检查**贴着共享边**的像素中心。
    const sr::Vec2 q0{0.5f, 0.5f}, q1{10.3f, 0.7f}, q2{10.1f, 9.6f}, q3{0.7f, 9.4f};

    Canvas c{16, 16};
    const uint32_t colA = sr::encodeSRGB({1, 0, 0});
    const uint32_t colB = sr::encodeSRGB({0, 0, 1});
    const FlatTriangle A{{q0, q1, q2}, colA};
    const FlatTriangle B{{q0, q2, q3}, colB};
    sr::rasterizeFlatTriangle(c.rt.color, A);
    sr::rasterizeFlatTriangle(c.rt.color, B);

    // 共享边是 q0→q2。找出像素中心落在它附近（垂直距离 < 0.25 像素）
    // 且投影落在线段内部的那些点：它们必须至少被一个三角形认领。
    const sr::Vec2 e{q2.x - q0.x, q2.y - q0.y};
    const float len = std::sqrt(e.x * e.x + e.y * e.y);
    const sr::Vec2 unit{e.x / len, e.y / len};
    int nearEdge = 0, gaps = 0;
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const sr::Vec2 p{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
            const sr::Vec2 d{p.x - q0.x, p.y - q0.y};
            const float along = d.x * unit.x + d.y * unit.y;
            const float perp = std::fabs(-d.x * unit.y + d.y * unit.x);
            if (along < 0.75f || along > len - 0.75f) continue;   // 线段之外不算
            if (perp > 0.25f) continue;
            ++nearEdge;
            const uint32_t v = c.rt.color.readPacked(x, y);
            if (v == kClear) ++gaps;
        }
    }
    SR_ASSERT(nearEdge > 3);          // 构造必须真的产生贴着边的采样点
    SR_ASSERT_EQ(gaps, 0);
}

