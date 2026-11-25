#pragma once

#include <cstdint>

namespace sr {

// 一帧的计数与分阶段耗时。字段名在计划 Task 2.1 定死，测试按这些名字断言。
//
// 多线程（M8）下的聚合方式是"每线程一份 + 最后 merge"，不用原子：
// 原子计数在逐像素路径上是实打实的性能损失，而 tile 之间本来就不共享状态。
struct FrameStats {
    // draw call 提交次数（M5.0）：每次进入 drawRange 记一次，与三角形数无关。
    // 一帧多个 index range（M5 的 OBJ group）时，它是"提交了几次"的唯一计数。
    int64_t drawCalls = 0;
    int64_t trianglesSubmitted = 0;
    int64_t trianglesClippedOut = 0;
    int64_t trianglesDegenerate = 0;
    int64_t trianglesBackfaceCulled = 0;
    int64_t trianglesRasterized = 0;
    int64_t trianglesOffscreen = 0;

    // fragmentsCovered = 通过覆盖判定的像素数；fragmentsWritten = 实际落到颜色的像素数。
    // 两者之差 = 被深度测试拒掉的像素。M8 的后端一致性测试就靠这四个计数完全相等。
    int64_t fragmentsCovered = 0;
    int64_t fragmentsWritten = 0;
    int64_t fragmentsDepthRejected = 0;
    int64_t fragmentsShaded = 0;

    int64_t interpClampedCount = 0;

    // 分阶段耗时（M6.7/D21 起由 BackendA::drawRange 写入；此前是"声明了但没人写"的
    // 休眠接口）。四段来自同一只秒表的相邻 lapMs()，**划分**一次 draw 的时间：
    // sum(ms*) ≈ msTotal（略小：循环/尾巴开销未归段），stage_timers_partition_the_total
    // 钉住这个关系。msTotal 只覆盖 drawRange；整帧时间（clear/HUD/sink）在 app 层另测。
    // 每三角形至多 4 次读时钟，绝不逐像素（C5）；stats == nullptr 时一次都不读。
    double msVertex = 0;
    double msClip = 0;
    double msSetup = 0;
    double msRaster = 0;
    double msTotal = 0;

    void reset() { *this = FrameStats{}; }

    void merge(const FrameStats& o) {
        drawCalls += o.drawCalls;
        trianglesSubmitted += o.trianglesSubmitted;
        trianglesClippedOut += o.trianglesClippedOut;
        trianglesDegenerate += o.trianglesDegenerate;
        trianglesBackfaceCulled += o.trianglesBackfaceCulled;
        trianglesRasterized += o.trianglesRasterized;
        trianglesOffscreen += o.trianglesOffscreen;
        fragmentsCovered += o.fragmentsCovered;
        fragmentsWritten += o.fragmentsWritten;
        fragmentsDepthRejected += o.fragmentsDepthRejected;
        fragmentsShaded += o.fragmentsShaded;
        interpClampedCount += o.interpClampedCount;
        msVertex += o.msVertex;
        msClip += o.msClip;
        msSetup += o.msSetup;
        msRaster += o.msRaster;
        msTotal += o.msTotal;
    }
};

}  // namespace sr
