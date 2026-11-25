#include "render/render_backend.h"

#include <algorithm>

#include "bench/timer.h"
#include "render/builtin_shaders.h"
#include "render/clipping.h"
#include "render/color.h"
#include "render/interpolate.h"
#include "render/plane_eq.h"
#include "render/raster_scalar.h"
#include "render/triangle_setup.h"
#include "render/viewport.h"

namespace sr {
namespace {

inline void bump(FrameStats* s, int64_t FrameStats::*field, int64_t n = 1) {
    if (s != nullptr) s->*field += n;
}

}  // namespace

void BackendA::drawRange(const Mesh& mesh, const PipelineState& state,
                         uint32_t firstIndex, uint32_t indexCount) {
    FrameStats* stats = state.stats;
    // C5：drawCalls 数的是**提交**，每次入口一次，绝不进三角形循环。
    // 放在早退之前：空区间/无目标的提交也是一次 draw call。
    bump(stats, &FrameStats::drawCalls);
    if (state.target == nullptr || mesh.indices.empty()) return;
    RenderTarget& target = *state.target;
    const Viewport vp = Viewport::fromColorTarget(target.color);

    // firstIndex/indexCount 都是**索引数组**的下标/个数，不是顶点的。
    // 夹进数组实际范围：坏区间只许少画，不许越界读。
    const size_t nIndices = mesh.indices.size();
    const size_t begin = std::min<size_t>(firstIndex, nIndices);
    const size_t count = std::min<size_t>(indexCount, nIndices - begin);
    const size_t end = begin + count;

    Timer total;
    // D21（M6.7）：四个阶段共用一只秒表。lapMs() 读一次时钟并把它重启在当下 ⇒
    // 每个三角形至多 4 次读时钟（C5：绝不逐像素），相邻 lap 首尾相接，求和 telescoping
    // 成"最后一次 lap − 起点" ⇒ 四段**划分** drawRange 的时间（stage_timers_partition_the_total）。
    // stats == nullptr 时一次时钟都不读：没人要统计时不为统计付费。
    Timer stage;
    bump(stats, &FrameStats::trianglesSubmitted, static_cast<int64_t>(count / 3));

    // 一个已裁剪的三角形：VS 输出 → 投影 → Setup → 逐像素。
    // 写成 lambda 而不是函数，是为了让它对这些引用保持内联（C5：内层不许间接派发）。
    auto rasterOne = [&](const ClipVertex& a, const ClipVertex& b, const ClipVertex& c) {
        const ClipVertex cv[3] = {a, b, c};
        ScreenVertex sv[3];
        for (int k = 0; k < 3; ++k) sv[k] = vp.project(cv[k]);

        TriangleSetup setup;
        SetupResult res;
        const bool accepted = setupTriangle(sv, target.color, state.cull, TileGrid{}, setup, res);
        // setup 段到此结束 —— 被拒绝的三角形也消费了投影+setup 时间，同样要归段。
        // 扇形重三角化会多次进入本 lambda ⇒ 累加（+=），不是覆盖。
        if (stats != nullptr) stats->msSetup += stage.lapMs();
        if (!accepted) {
            // 拒绝原因分开计数：offscreen 是正常事件，degenerate/nonfinite 是上游有问题的信号。
            // 把它们混成一个数会掩盖 bug（Task 2.4 的测试守住这条）。
            switch (res.reason) {
                case RejectReason::NonFinite:
                case RejectReason::Degenerate: bump(stats, &FrameStats::trianglesDegenerate); break;
                case RejectReason::Backface: bump(stats, &FrameStats::trianglesBackfaceCulled); break;
                case RejectReason::Offscreen: bump(stats, &FrameStats::trianglesOffscreen); break;
                default: bump(stats, &FrameStats::trianglesDegenerate); break;
            }
            return;
        }

        FragmentOutput fo;
        int64_t* clampCounter = (stats != nullptr) ? &stats->interpClampedCount : nullptr;
        forEachCoveredPixel(setup.geom, [&](int x, int y) {
            bump(stats, &FrameStats::fragmentsCovered);
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;
            const Interpolated ip = interpolateAt(setup, px, py, clampCounter);

            // 深度比较用 `<=`（设计文档 §7.1）：共面时后画者赢，这就是 Z-fighting 的取舍，
            // 由 coplanar_triangles_last_writer_wins 那条测试 pin 住而不是假装它不存在。
            // 读取与写入互相独立：depthWrite=false 时不遮挡也不污染（测试同名）。
            float d = 1.0f;
            if (state.depthTest) {
                d = target.depth.read(x, y);
                if (!(ip.depth <= d)) {
                    bump(stats, &FrameStats::fragmentsDepthRejected);
                    return;
                }
                d = ip.depth;
            } else {
                d = ip.depth;
            }

            FragmentInput fi;
            fi.screen = Vec2{px, py};
            fi.depth = ip.depth;
            fi.v = ip.v;
            // 每个标量场一个梯度向量（不是"沿 X / 沿 Y"），命名理由见 pipeline.h 的 FragmentInput。
            fi.gradU = Vec2{setup.dudx, setup.dudy};
            fi.gradV = Vec2{setup.dvdx, setup.dvdy};
            if (state.fragmentShader != nullptr) {
                if (!state.fragmentShader(fi, state.fu, fo)) return;   // discard
            } else {
                fo.color = state.fu.baseColor;
            }
            bump(stats, &FrameStats::fragmentsShaded);
            target.color.writePacked(x, y, encodeSRGB(fo.color));
            bump(stats, &FrameStats::fragmentsWritten);
            if (state.depthWrite) target.depth.write(x, y, ip.depth);
        });
        bump(stats, &FrameStats::trianglesRasterized);
        // raster 段 = 逐像素循环（含深度/FS/写回）。lap 放在 lambda 末尾：
        // 扇形重三角化的每个子三角形各归各的段。
        if (stats != nullptr) stats->msRaster += stage.lapMs();
    };

    for (size_t i = begin; i + 2 < end; i += 3) {
        // ---- Vertex Shader ----
        ClipVertex cv[3];
        for (int k = 0; k < 3; ++k) {
            const uint32_t idx = mesh.indices[i + k];
            VSIn in;
            in.position = mesh.positions[idx];
            in.normal = idx < mesh.normals.size() ? mesh.normals[idx] : Vec3{0, 1, 0};
            in.uv = idx < mesh.uvs.size() ? mesh.uvs[idx] : Vec2{0, 0};
            in.color = idx < mesh.colors.size() ? mesh.colors[idx] : Vec4{1, 1, 1, 1};
            cv[k] = state.vertexShader != nullptr ? state.vertexShader(in, state.vu) : ClipVertex{};
        }
        if (stats != nullptr) stats->msVertex += stage.lapMs();

        // ---- Clipping（六面）----
        ClipVertex clipped[kMaxClippedVerts];
        const int n = clipPolygonToFrustum(cv, clipped);
        // clip 段在 continue 之前归段：被裁掉的三角形也消费了裁剪时间，
        // 漏掉这一 lap 会把它的耗时错记进下一个三角形的 VS 段。
        if (stats != nullptr) stats->msClip += stage.lapMs();
        if (n == 0) {
            bump(stats, &FrameStats::trianglesClippedOut);
            continue;
        }
        // ---- 扇形重三角化：凸多边形 out[0], out[i], out[i+1] ----
        for (int t = 1; t + 1 < n; ++t) rasterOne(clipped[0], clipped[t], clipped[t + 1]);
    }

    // 残缺尾巴（count % 3 != 0）：防御性分支 —— parseObj（M5.2）保证 group 3 对齐，
    // 生产代码目前到不了这里（Ruling 45：保留）。一个坏 group 只记一次 degenerate，
    // 不许断言带走整帧，更不许把尾巴凑成三角形越界读（Review Focus 3）。
    if (count % 3 != 0) bump(stats, &FrameStats::trianglesDegenerate);

    if (stats != nullptr) stats->msTotal += total.elapsedMs();
}

}  // namespace sr
