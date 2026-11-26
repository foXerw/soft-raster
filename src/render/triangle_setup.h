#pragma once

#include <cstdint>

#include "math/rect.h"
#include "render/pipeline.h"
#include "render/plane_eq.h"
#include "render/triangle_geom.h"
#include "render/vertex.h"

namespace sr {

inline constexpr int kTileSizePx = 32;   // 像素；Backend B 的微 tile（8x8）在 M9 里另算

// tile 网格。tile 化不只是 SIMD 的前提，也是并行安全的前提（设计文档 §7.2）：
// 每个 tile 拿到全局深度缓冲的一个子矩形 view，互不重叠 ⇒ 无锁、无原子。
struct TileGrid {
    int tilesX = 0;
    int tilesY = 0;

    bool valid() const { return tilesX > 0 && tilesY > 0; }
    static TileGrid forTarget(int width, int height, int tilePx = kTileSizePx) {
        TileGrid g;
        g.tilesX = (width + tilePx - 1) / tilePx;
        g.tilesY = (height + tilePx - 1) / tilePx;
        return g;
    }
    // 像素闭区间 → tile 闭区间
    Rect rangeOf(const Rect& pixels) const {
        Rect r;
        r.x0 = pixels.x0 / tileSide();
        r.y0 = pixels.y0 / tileSide();
        r.x1 = pixels.x1 / tileSide();
        r.y1 = pixels.y1 / tileSide();
        if (valid()) {
            r.x0 = r.x0 < 0 ? 0 : r.x0;
            r.y0 = r.y0 < 0 ? 0 : r.y0;
            r.x1 = r.x1 > tilesX - 1 ? tilesX - 1 : r.x1;
            r.y1 = r.y1 > tilesY - 1 ? tilesY - 1 : r.y1;
        }
        return r;
    }
    int tileSide() const { return kTileSizePx; }
};

enum class RejectReason : uint8_t {
    None = 0,
    Degenerate,   // 面积≈0：共线或重合顶点
    NonFinite,    // NaN/inf 顶点
    Backface,     // 被背面剔除
    Offscreen,    // 完全在目标之外（正常，但和 Degenerate 必须分开计数）
};

struct SetupResult {
    RejectReason reason = RejectReason::None;
    bool rejected() const { return reason != RejectReason::None; }
};

// 每三角形一次的准备工作：边函数 + 每个 varying 分量的平面方程 + 1/w 的平面方程
// + uv 的屏幕导数 + 像素包围盒 + tile 范围。
struct TriangleSetup {
    TriangleGeom geom;
    PlaneEq planeInvW;
    // 深度单独一张平面：depth = ndcZ*0.5+0.5，而 ndcZ 在屏幕空间里是仿射的
    // （z_view 与 w 成正比），所以深度**不需要**透视校正。
    // 把这件事写成字段而不是口头承诺，测试 depth_is_affine_in_screen_space 会守住它。
    PlaneEq planeDepth;
    PlaneEq planeAttr[kVaryingFloatCount];
    float dudx = 0, dudy = 0, dvdx = 0, dvdy = 0;
    Rect tileRange;

    // 这三个是 geom 的别名而不是方法：写成本方法时测试里 s.area2 会漏掉括号，
    // 而 MSVC 报的是"non-standard syntax; use & to create a pointer to member"，
    // 那种错误信息对读者毫无帮助。数据成员 + 一次赋值，换来清晰的错误。
    float area2 = 0.0f;
    Rect extent;
    bool backFacing = false;   // y-down 里 area2 < 0 是正面 ⇒ 正面积就是背面
};

// 判定顺序是接口的一部分（测试 setup_rejects_degenerate_before_testing_facing 守住它）：
// 先非有限、再退化、再朝向、最后离屏。面积≈0 时朝向无意义，先判朝向会让结果随 eps 抖动。
bool setupTriangle(const ScreenVertex in[3], const ColorTarget& color, const CullState& cull,
                   const TileGrid& grid, TriangleSetup& out, SetupResult& result);

// 不需要 tile 信息时的便捷重载（Backend A 用）。
inline bool setupTriangle(const ScreenVertex in[3], const ColorTarget& color, const CullState& cull,
                          TriangleSetup& out, SetupResult& result) {
    return setupTriangle(in, color, cull, TileGrid{}, out, result);
}

}  // namespace sr
