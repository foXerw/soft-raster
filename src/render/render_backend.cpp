#include "render/render_backend.h"

#include "bench/timer.h"
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

        // ---- Clipping：M2 的下一个任务在这里插入六面裁剪 ----

        // ---- Perspective divide + viewport ----
        ScreenVertex sv[3];
        for (int k = 0; k < 3; ++k) sv[k] = vp.project(cv[k]);

        // ---- Primitive Setup ----
        Vec2 pos[3] = {sv[0].pos, sv[1].pos, sv[2].pos};
        TriangleGeom geom;
        if (!buildTriangleGeometry(pos, target.color.width, target.color.height, geom)) {
            bump(stats, &FrameStats::trianglesDegenerate);
            continue;
        }

        // ---- Rasterize + Fragment Shader ----
        // M3 之前不做插值：varying 直接取第一个顶点的值（flat shading）。
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
    }

    if (stats != nullptr) stats->msTotal += total.elapsedMs();
}

}  // namespace sr
