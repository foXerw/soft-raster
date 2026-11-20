#include "render/render_backend.h"

#include "bench/timer.h"
#include "render/builtin_shaders.h"
#include "render/clipping.h"
#include "render/color.h"
#include "render/plane_eq.h"
#include "render/raster_scalar.h"
#include "render/triangle_geom.h"
#include "render/viewport.h"

namespace sr {
namespace {

inline void bump(FrameStats* s, int64_t FrameStats::*field, int64_t n = 1) {
    if (s != nullptr) s->*field += n;
}

}  // namespace

void BackendA::draw(const Mesh& mesh, const PipelineState& state) {
    if (state.target == nullptr || mesh.indices.empty()) return;
    RenderTarget& target = *state.target;
    FrameStats* stats = state.stats;
    const Viewport vp = Viewport::fromColorTarget(target.color);

    Timer total;
    bump(stats, &FrameStats::trianglesSubmitted, mesh.triangleCount());

    // 一个已裁剪的三角形：VS 输出 → 投影 → Setup → 逐像素。
    // 写成 lambda 而不是函数，是为了让它对这些引用保持内联（C5：内层不许间接派发）。
    auto rasterOne = [&](const ClipVertex& a, const ClipVertex& b, const ClipVertex& c) {
        const ClipVertex cv[3] = {a, b, c};
        ScreenVertex sv[3];
        for (int k = 0; k < 3; ++k) sv[k] = vp.project(cv[k]);

        const Vec2 pos[3] = {sv[0].pos, sv[1].pos, sv[2].pos};
        TriangleGeom geom;
        if (!buildTriangleGeometry(pos, target.color.width, target.color.height, geom)) {
            bump(stats, &FrameStats::trianglesDegenerate);
            return;
        }

        // M3 之前不做插值：varying 直接取第一个顶点的值（flat shading）。
        // 深度测试在这里读取但恒通过 —— 标志位是 M2 就预留的，M3 只填实现。
        FragmentInput fi;
        fi.v = sv[0].v;
        fi.depth = sv[0].depth;
        FragmentOutput fo;
        forEachCoveredPixel(geom, [&](int x, int y) {
            bump(stats, &FrameStats::fragmentsCovered);
            fi.screen = Vec2{static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f};
            if (state.fragmentShader != nullptr) {
                if (!state.fragmentShader(fi, state.fu, fo)) return;   // discard
            } else {
                fo.color = state.fu.baseColor;
            }
            bump(stats, &FrameStats::fragmentsShaded);
            target.color.writePacked(x, y, encodeSRGB(fo.color));
            bump(stats, &FrameStats::fragmentsWritten);
        });
        bump(stats, &FrameStats::trianglesRasterized);
    };

    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        // ---- Vertex Shader ----
        ClipVertex cv[3];
        for (int k = 0; k < 3; ++k) {
            const uint32_t idx = mesh.indices[i + k];
            VSIn in;
            in.position = mesh.positions[idx];
            in.normal = idx < mesh.normals.size() ? mesh.normals[idx] : Vec3{0, 1, 0};
            in.uv = idx < mesh.uvs.size() ? mesh.uvs[idx] : Vec2{0, 0};
            cv[k] = state.vertexShader != nullptr ? state.vertexShader(in, state.vu) : ClipVertex{};
        }

        // ---- Clipping（六面）----
        ClipVertex clipped[kMaxClippedVerts];
        const int n = clipPolygonToFrustum(cv, clipped);
        if (n == 0) {
            bump(stats, &FrameStats::trianglesClippedOut);
            continue;
        }
        // ---- 扇形重三角化：凸多边形 out[0], out[i], out[i+1] ----
        for (int t = 1; t + 1 < n; ++t) rasterOne(clipped[0], clipped[t], clipped[t + 1]);
    }

    if (stats != nullptr) stats->msTotal += total.elapsedMs();
}

}  // namespace sr
