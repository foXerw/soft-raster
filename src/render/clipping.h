#pragma once

#include "render/vertex.h"

namespace sr {

// clip space 六面裁剪。为什么不是"只裁近平面"（设计文档 §7.1 的裁决）：
// 只裁近平面时，视锥 x/y 之外的三角形仍会投影成极大坐标，
// M8 的 tile bin 范围会炸开、M9 的定点 edge function 有溢出风险，只能靠 scissor 硬夹。
// 六面版把这一整类问题在源头消掉，代码只多约 40 行（同一个循环参数化到 7 个判据平面，即 `kClipPlaneCount`），
// 而且这正是 GPU 的做法。
//
// 约定：内部为正。距离函数 d = (平面) 使得 d >= 0 表示在视锥内。
// 边界上的点算"在内"，这样相邻两次裁剪不会互相生产退化多边形。
enum ClipPlane {
    kPlaneNegW = 0,   // w > 0
    kPlanePosX,       // x <= w
    kPlaneNegX,       // x >= -w
    kPlanePosY,       // y <= w
    kPlaneNegY,       // y >= -w
    kPlanePosZ,       // z <= w
    kPlaneNegZ,       // z >= -w
    kClipPlaneCount
};

// 顶点到某个裁剪平面的带符号距离（内部为正）。
float clipPlaneDistance(const Vec4& clip, int plane);

bool vertexInsideFrustum(const Vec4& clip);

// 两个凸多边形相交，结果顶点数最多是两者之和：三角形 3 个 + **七个**裁剪平面各贡献 1 个 = 10。
// （第一版按"最多 5 个"设计接口，实测一个跨多个平面的三角形能产出 6~8 个顶点 ——
//  上限写小的后果是静默截断，症状是"模型某些角缺一块"。）
inline constexpr int kMaxClippedVerts = 3 + kClipPlaneCount;

// 输入是一个凸多边形（三角形给 3 个），输出裁剪后的凸多边形。
// 返回值是输出顶点数（0 表示整个被剔除）。out 必须能放 kMaxClippedVerts 个。
int clipPolygonToFrustum(const ClipVertex in[3], ClipVertex out[kMaxClippedVerts]);

// 把裁剪后的多边形按扇形重三角化：out[0], out[i], out[i+1]。
inline int fanTriangleCount(int polygonVerts) { return polygonVerts >= 3 ? polygonVerts - 2 : 0; }

}  // namespace sr
