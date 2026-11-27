#include "render/clipping.h"

#include <cassert>

namespace sr {
namespace {

constexpr int kMaxPolygonVerts = kMaxClippedVerts;   // 10 = 3 + 七个判据平面：见头文件里的推导

ClipVertex lerpClipVertex(const ClipVertex& a, const ClipVertex& b, float t) {
    ClipVertex r;
    // clip 与 v 用**同一个** t：沿 clip 边两者都线性（推导见 vertex.h）。
    r.clip = lerp(a.clip, b.clip, t);
    r.v.color = lerp(a.v.color, b.v.color, t);
    r.v.worldNormal = lerp(a.v.worldNormal, b.v.worldNormal, t);
    r.v.uv = lerp(a.v.uv, b.v.uv, t);
    r.v.worldPos = lerp(a.v.worldPos, b.v.worldPos, t);
    return r;
}

}  // namespace

float clipPlaneDistance(const Vec4& c, int plane) {
    // 内部为正。边界（0）算在内，这样相邻两次裁剪不会互相生产退化多边形。
    switch (plane) {
        case kPlaneNegW: return c.w;
        case kPlanePosX: return c.w - c.x;
        case kPlaneNegX: return c.w + c.x;
        case kPlanePosY: return c.w - c.y;
        case kPlaneNegY: return c.w + c.y;
        case kPlanePosZ: return c.w - c.z;
        case kPlaneNegZ: return c.w + c.z;
        default: return 1.0f;
    }
}

bool vertexInsideFrustum(const Vec4& c) {
    for (int p = 0; p < kClipPlaneCount; ++p) {
        if (clipPlaneDistance(c, p) < 0.0f) return false;
    }
    return true;
}

int clipPolygonToFrustum(const ClipVertex in[3], ClipVertex out[kMaxClippedVerts]) {
    ClipVertex src[kMaxPolygonVerts];
    ClipVertex dst[kMaxPolygonVerts];
    for (int i = 0; i < 3; ++i) src[i] = in[i];
    int count = 3;

    for (int plane = 0; plane < kClipPlaneCount && count > 0; ++plane) {
        int m = 0;
        for (int i = 0; i < count; ++i) {
            const ClipVertex& a = src[i];
            const ClipVertex& b = src[(i + 1) % count];
            const float da = clipPlaneDistance(a.clip, plane);
            const float db = clipPlaneDistance(b.clip, plane);
            const bool aIn = (da >= 0.0f);
            const bool bIn = (db >= 0.0f);

            if (aIn) {
                assert(m < kMaxPolygonVerts);
                dst[m++] = a;
            }
            if (aIn != bIn) {
                const float t = da / (da - db);
                // t 落在端点上时不额外产生顶点：a 已经输出过（t==0），
                // 而 b 会在下一轮作为 a 输出（t==1）。少了这个判断就会出现
                // "重复顶点 ⇒ 零面积三角形"，症状是白做工与偶发的退化警告。
                if (t > 0.0f && t < 1.0f) {
                    assert(m < kMaxPolygonVerts);
                    dst[m++] = lerpClipVertex(a, b, t);
                }
            }
        }
        count = m;
        for (int i = 0; i < count; ++i) src[i] = dst[i];
    }

    for (int i = 0; i < count; ++i) out[i] = src[i];
    return count;
}

}  // namespace sr
