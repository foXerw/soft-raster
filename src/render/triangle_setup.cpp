#include "render/triangle_setup.h"

#include <cmath>

namespace sr {
namespace {

inline RejectReason mapGeomReject(GeomReject r) {
    switch (r) {
        case GeomReject::NonFinite: return RejectReason::NonFinite;
        case GeomReject::Degenerate: return RejectReason::Degenerate;
        case GeomReject::Offscreen: return RejectReason::Offscreen;
        default: return RejectReason::None;
    }
}

}  // namespace

bool setupTriangle(const ScreenVertex in[3], const ColorTarget& color, const CullState& cull,
                   const TileGrid& grid, TriangleSetup& out, SetupResult& result) {
    out = TriangleSetup{};
    result.reason = RejectReason::None;

    Vec2 pos[3];
    for (int k = 0; k < 3; ++k) {
        // 深度与 invW 也要检查：只查 pos 的话，NaN 会从 depth 或 1/w 溜进插值与比较。
        if (!std::isfinite(in[k].pos.x) || !std::isfinite(in[k].pos.y) ||
            !std::isfinite(in[k].depth) || !std::isfinite(in[k].invW) || !std::isfinite(in[k].w)) {
            result.reason = RejectReason::NonFinite;
            return false;
        }
        pos[k] = in[k].pos;
    }

    // 1) 退化 2) 离屏 —— 都在几何层判定，顺序见 triangle_geom.h
    const GeomReject gr =
        buildTriangleGeometryChecked(pos, color.width, color.height, out.geom);
    if (gr != GeomReject::None) {
        result.reason = mapGeomReject(gr);
        return false;
    }

    out.area2 = out.geom.area2;
    out.extent = out.geom.extent;

    // 3) 朝向。y-down 屏幕空间里正面满足 **area2 < 0**，所以：
    //      frontFaceIsClockwise == true  ⇒ area2 > 0 是背面
    //    绕向与 y 翻转是耦合的，但翻转只改有向面积的符号，不改肉眼旋向（M2 notes §3a 已更正）。
    out.backFacing = cull.frontFaceIsClockwise ? (out.area2 > 0.0f) : (out.area2 < 0.0f);
    if (cull.backfaceCull && out.backFacing) {
        result.reason = RejectReason::Backface;
        return false;
    }

    // 4) 平面方程。注意建立用的是 **attr/w**（不是 attr）：
    //    屏幕空间里 attr/w 与 1/w 才是 x,y 的仿射函数，attr 本身不是。
    //    写错成 attr 的症状要到 M3 的四边形数值测试才暴露，所以这里注释点名它。
    float w0 = in[0].invW, w1 = in[1].invW, w2 = in[2].invW;
    out.planeInvW = PlaneEq::fromTriangle(in[0].pos, w0, in[1].pos, w1, in[2].pos, w2);
    out.planeDepth = PlaneEq::fromTriangle(in[0].pos, in[0].depth, in[1].pos, in[1].depth,
                                           in[2].pos, in[2].depth);
    if (out.planeInvW.degenerate) {
        result.reason = RejectReason::Degenerate;
        return false;
    }
    for (int f = 0; f < kVaryingFloatCount; ++f) {
        const float a0 = varyingFloats(in[0].v)[f] * w0;
        const float a1 = varyingFloats(in[1].v)[f] * w1;
        const float a2 = varyingFloats(in[2].v)[f] * w2;
        out.planeAttr[f] = PlaneEq::fromTriangle(in[0].pos, a0, in[1].pos, a1, in[2].pos, a2);
    }

    // 5) uv 的屏幕导数（M4 的 mipmap 选择要用）。uv = A/I，其中 A 是 uv/w 的平面、
    //    I 是 1/w 的平面，所以导数来自商法则；在三角形重心处取一次。
    const float cx = (in[0].pos.x + in[1].pos.x + in[2].pos.x) / 3.0f;
    const float cy = (in[0].pos.y + in[1].pos.y + in[2].pos.y) / 3.0f;
    const float iHere = out.planeInvW.sample(cx, cy);
    if (std::fabs(iHere) > 1e-12f) {
        const float i2 = iHere * iHere;
        out.dudx = (out.planeAttr[7].n0 * iHere - out.planeAttr[7].sample(cx, cy) * out.planeInvW.n0) / i2;
        out.dudy = (out.planeAttr[7].n1 * iHere - out.planeAttr[7].sample(cx, cy) * out.planeInvW.n1) / i2;
        out.dvdx = (out.planeAttr[8].n0 * iHere - out.planeAttr[8].sample(cx, cy) * out.planeInvW.n0) / i2;
        out.dvdy = (out.planeAttr[8].n1 * iHere - out.planeAttr[8].sample(cx, cy) * out.planeInvW.n1) / i2;
    }

    // 6) tile 范围。给了网格就必须在网格内（六面裁剪保证它不会炸开）。
    if (grid.valid()) out.tileRange = grid.rangeOf(out.extent);

    return true;
}

}  // namespace sr
