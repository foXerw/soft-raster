#pragma once

#include <cmath>
#include <cstdint>

#include "math/rect.h"
#include "math/vec.h"
#include "render/plane_eq.h"

namespace sr {

// 屏幕空间三角形的"可光栅化"形态：三条边函数 + 有向面积 + 已夹好的像素包围盒。
//
// 这一层与 TriangleSetup（M2 的平面方程）分开，是因为逐像素循环只依赖这三样东西：
// Backend A 用它，Backend B 的微 tile 覆盖判定也用它，两者因此共享同一套 fill rule
// —— 这是 spec §7.8 "正确性不因优化而改变" 那条硬约束在代码结构上的落点。
struct TriangleGeom {
    Vec2 v[3];
    EdgeFunc edges[3];
    Rect extent;
    float area2 = 0.0f;
};

// 拒绝原因分开报告，而不是一个 bool：
// "完全在屏幕外"是正常事件，"退化"是上游有问题的信号，混成一个计数会掩盖 bug
// （Task 2.4 的 setup_offscreen_triangle_is_reported_offscreen_not_degenerate 守住这条）。
enum class GeomReject : uint8_t { None = 0, NonFinite, Degenerate, Offscreen };

// 判定顺序是刻意的：先查非有限（NaN 会让后面的比较全都为假，症状是"整屏不画"），
// 再查面积（共线/重合顶点会让 PlaneEq 无解），最后才查是否完全在目标外。
inline GeomReject buildTriangleGeometryChecked(const Vec2 in[3], int targetWidth, int targetHeight,
                                               TriangleGeom& out) {
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(in[i].x) || !std::isfinite(in[i].y)) return GeomReject::NonFinite;
    }

    out.v[0] = in[0];
    out.v[1] = in[1];
    out.v[2] = in[2];

    const float area2 = cross(in[1] - in[0], in[2] - in[0]);
    if (!(std::fabs(area2) > 1e-6f)) return GeomReject::Degenerate;   // 退化：共线或重合
    out.area2 = area2;

    // 内侧统一为正：绕向用 sign 吸收掉，而不是把顶点顺序改写
    // （改顶点顺序会让"第一个顶点的 varying"这种 flat 取值悄悄换掉一个顶点）。
    const float sign = (area2 > 0.0f) ? 1.0f : -1.0f;
    out.edges[0] = EdgeFunc::fromEdge(in[0], in[1], sign);
    out.edges[1] = EdgeFunc::fromEdge(in[1], in[2], sign);
    out.edges[2] = EdgeFunc::fromEdge(in[2], in[0], sign);

    // 包围盒取像素中心的整数范围：min 向上取整、max 向下取整，
    // 这样盒子里的像素中心一定落在 [floor(min), ceil(max)] 之内。
    const float minX = std::min({in[0].x, in[1].x, in[2].x});
    const float maxX = std::max({in[0].x, in[1].x, in[2].x});
    const float minY = std::min({in[0].y, in[1].y, in[2].y});
    const float maxY = std::max({in[0].y, in[1].y, in[2].y});

    // 像素中心是 (x + 0.5)，所以"包含 min/max"的整数行是 floor(min-0.5) .. ceil(max-0.5)。
    // 这是保守的超集：宁可多算几列，也不能漏列。
    Rect r;
    r.x0 = static_cast<int>(std::floor(minX - 0.5f));
    r.x1 = static_cast<int>(std::ceil(maxX - 0.5f));
    r.y0 = static_cast<int>(std::floor(minY - 0.5f));
    r.y1 = static_cast<int>(std::ceil(maxY - 0.5f));
    out.extent = r.clampedTo(targetWidth, targetHeight);
    return out.extent.empty() ? GeomReject::Offscreen : GeomReject::None;
}

inline bool buildTriangleGeometry(const Vec2 in[3], int targetWidth, int targetHeight,
                                  TriangleGeom& out) {
    return buildTriangleGeometryChecked(in, targetWidth, targetHeight, out) == GeomReject::None;
}

// 逐像素回调。模板而不是 std::function：内层每个像素都要调用一次，
// 任何间接派发都会让编译器无法内联（设计文档 C5），而这里正是 M9 要 SIMD 的那一段。
// 返回值 = 通过覆盖判定的像素数（含被目标边界丢弃的那些）。
template <class Fn>
inline int forEachCoveredPixel(const TriangleGeom& g, Fn&& paint) {
    int covered = 0;
    for (int y = g.extent.y0; y <= g.extent.y1; ++y) {
        for (int x = g.extent.x0; x <= g.extent.x1; ++x) {
            // 像素中心：整数像素 (x, y) 的中心是 (x + 0.5, y + 0.5)
            const Vec2 p{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
            if (!g.edges[0].covers(p)) continue;
            if (!g.edges[1].covers(p)) continue;
            if (!g.edges[2].covers(p)) continue;
            ++covered;
            paint(x, y);
        }
    }
    return covered;
}

}  // namespace sr
