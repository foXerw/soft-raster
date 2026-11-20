#pragma once

#include <cmath>

#include "math/vec.h"

namespace sr {

// 三角形内部判定的两个基本工具。它们是整个光栅化的数学核心，所以推导写在这里而不是笔记里
// （设计文档 A1：抽象必须止步于阶段边界，阶段内部要直白）。
//
// 坐标约定：top-down 屏幕空间（y 朝下，见设计文档 C8），正面是 CW。
// 在这套约定下，有向边 e = p1 - p0 的**内侧法线**是 n = {-e.y, e.x}。
// 验证：三角形 (0,0)-(10,0)-(0,10) 的叉积 cross((10,0),(0,10)) = 100 > 0 ⇒ 视觉上是 CW；
// 边 (0,0)→(10,0) 的 n = {0, 10}，对内部点 (2,2) 求值得 20 > 0 ⇒ 内部为正 ✓。
// （若沿用教科书的 y 朝上约定，法线是 {e.y, -e.x}，符号正好相反 —— 这是最容易写错的一处，
//  症状是"整个三角形一个像素都不画"或"画满整屏"。）

struct EdgeFunc {
    Vec2 p0;              // 边的起点
    Vec2 p1;              // 边的终点（top/left 判定要用到）
    Vec2 n;               // 内侧法线
    float c = 0.0f;       // 使 evaluate(p0) == evaluate(p1) == 0

    // sign 由三角形的绕向决定（见 buildTriangleGeometryChecked 的调用点）：
    // {-e.y, e.x} 让"面积叉积为正"的那种顶点顺序内部为正；反过来的绕向必须取负。
    // 少了这个参数，负绕向的三角形会一个像素都不覆盖 —— 而 top-down 屏幕空间里
    // **正面恰好是负绕向**，所以这个 bug 只在立方体接进来时才暴露（症状：covered = 0）。
    static EdgeFunc fromEdge(const Vec2& a, const Vec2& b, float sign = 1.0f) {
        const Vec2 e{b.x - a.x, b.y - a.y};
        EdgeFunc f;
        f.p0 = a;
        f.p1 = b;
        f.n = {-e.y * sign, e.x * sign};
        f.c = -(f.n.x * a.x + f.n.y * a.y);
        return f;
    }

    float evaluate(const Vec2& p) const { return n.x * p.x + n.y * p.y + c; }

    // top-left 填充规则。两条判据都用"第三顶点相对这条边的位置"来表达，
    // 这样规则与 y 轴方向、与三角形绕向的关系是**显式**的：
    //   顶边：边水平，且第三顶点在它下方（top-down 里 y 更大）。
    //   左边：边不水平，且第三顶点在边的右侧 ⇒ 这条边构成三角形的左轮廓。
    // 权威性由 test_raster_scalar.cpp 里那两个"正方形沿对角线劈分"的测试判定：
    // 缝隙 ⇒ 覆盖总和 < 像素数；双写 ⇒ 总和 > 像素数。
    bool edgeIsTopOrLeft(const Vec2& opposite) const {
        const bool horizontal = (p0.y == p1.y);
        if (horizontal) return opposite.y > p0.y;
        return opposite.x > p0.x || opposite.x > p1.x;
    }

    // 边界像素恰好被一个三角形拥有：> 0 一定进；== 0 只有顶边/左边才进。
    bool covers(const Vec2& p, const Vec2& opposite) const {
        const float v = evaluate(p);
        if (v > 0.0f) return true;
        if (v < 0.0f) return false;
        return edgeIsTopOrLeft(opposite);
    }
};

struct PlaneEq {
    float n0 = 0.0f;
    float n1 = 0.0f;
    float c = 0.0f;
    bool degenerate = false;

    // 过三个 (x, y, value) 点的平面 f(x,y) = n0*x + n1*y + c。
    // 推导：令 d1 = p1-p0, d2 = p2-p0, g1 = v1-v0, g2 = v2-v0，则
    //   [d1.x d1.y; d2.x d2.y] · [n0; n1] = [g1; g2]
    // 2x2 克莱姆法则解出来，det 就是两倍的有向面积 ⇒ 共线时 det≈0，无解。
    static PlaneEq fromTriangle(const Vec2& p0, float v0, const Vec2& p1, float v1,
                                const Vec2& p2, float v2) {
        PlaneEq f;
        const float d1x = p1.x - p0.x, d1y = p1.y - p0.y;
        const float d2x = p2.x - p0.x, d2y = p2.y - p0.y;
        const float g1 = v1 - v0, g2 = v2 - v0;
        const float det = d1x * d2y - d1y * d2x;
        if (!(std::fabs(det) > 1e-20f)) {
            f.degenerate = true;      // 保持 n0/n1/c 为 0：调用方看 degenerate 标志，不看值
            return f;
        }
        f.n0 = (g1 * d2y - g2 * d1y) / det;
        f.n1 = (d1x * g2 - d2x * g1) / det;
        f.c = v0 - f.n0 * p0.x - f.n1 * p0.y;
        return f;
    }

    float sample(float x, float y) const { return n0 * x + n1 * y + c; }
};

}  // namespace sr
