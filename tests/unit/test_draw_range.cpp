#include <cstdint>

#include "harness.h"
#include "render/builtin_shaders.h"
#include "render/color.h"
#include "render/render_backend.h"
#include "render/stats.h"
#include "scene/mesh.h"
#include "test_util.h"

// M5.0：后端的索引区间入口 drawRange(mesh, state, firstIndex, indexCount)。
// firstIndex 是 **mesh.indices 的下标**，不是顶点下标（MeshGroup 会按同一单位喂偏移）。
//
// 夹具的牙齿在哪里：顶点刻意按"B 在前、A 在后"存放，indices = {3,4,5, 0,1,2} 是
// **非恒等**映射。若把 firstIndex 当顶点下标用（变异①），两个 slice 会画反三角形，
// 像素断言立刻红；若 indices 是恒等的 {0..5}，这个错误将完全不可见。
// 两个三角形分居屏幕两角、颜色不同，"画的是哪一个"从像素读得出来。

namespace {

constexpr int kW = 64;
constexpr int kH = 64;
constexpr uint32_t kBg = 0xFF000000u;

// 顶点 0..2 = 右上角绿色三角形 B；顶点 3..5 = 左下角红色三角形 A。
// 位置直接写在 clip 空间（w=1，三个矩阵都是单位阵），NDC CCW ⇒ 屏幕 CW ⇒ 正面。
sr::Mesh twoTriangles() {
    sr::Mesh m;
    m.positions = {
        {0.1f, 0.1f, 0.5f},   {0.9f, 0.1f, 0.5f},  {0.5f, 0.9f, 0.5f},    // B
        {-0.9f, -0.9f, 0.5f}, {-0.1f, -0.9f, 0.5f}, {-0.5f, -0.1f, 0.5f}, // A
    };
    m.colors = {
        {0, 1, 0, 1}, {0, 1, 0, 1}, {0, 1, 0, 1},
        {1, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 1},
    };
    m.indices = {3, 4, 5, 0, 1, 2};   // slice [0,3) = A(红)，slice [3,6) = B(绿)
    return m;
}

sr::PipelineState makeState(Canvas& c, sr::FrameStats& stats) {
    sr::PipelineState st;
    st.target = &c.rt;
    st.vertexShader = sr::transformVS;
    st.fragmentShader = sr::vertexColorFS;
    st.stats = &stats;
    st.vu.model = sr::Mat4::identity();
    st.vu.view = sr::Mat4::identity();
    st.vu.proj = sr::Mat4::identity();
    st.vu.normalMatrix = sr::Mat4::identity();
    st.depthTest = false;
    st.depthWrite = false;
    return st;
}

// 两个三角形各自的质心像素（视口映射 x=(ndcX/2+0.5)*W, y=(1-(ndcY/2+0.5))*H）。
constexpr int kAx = 16, kAy = 52;   // A(红) 的质心 ≈ (16, 52.3)
constexpr int kBx = 48, kBy = 20;   // B(绿) 的质心 ≈ (48, 20.3)

}  // namespace

SR_TEST(draw_range_submits_only_the_index_slice) {
    Canvas c{kW, kH};
    const sr::Mesh m = twoTriangles();
    sr::BackendA backend;
    const uint32_t red = sr::encodeSRGB(sr::LinearColor{1, 0, 0});
    const uint32_t green = sr::encodeSRGB(sr::LinearColor{0, 1, 0});

    sr::FrameStats s1;
    auto st1 = makeState(c, s1);
    c.rt.color.clear(kBg);
    backend.drawRange(m, st1, 0, 3);
    SR_ASSERT_EQ(s1.trianglesSubmitted, 1);
    SR_ASSERT_EQ(s1.trianglesRasterized, 1);
    SR_ASSERT_EQ(c.rt.color.readPacked(kAx, kAy), red);    // 画的是 A
    SR_ASSERT_EQ(c.rt.color.readPacked(kBx, kBy), kBg);    // B 一个像素都不许有

    sr::FrameStats s2;
    auto st2 = makeState(c, s2);
    c.rt.color.clear(kBg);
    backend.drawRange(m, st2, 3, 3);
    SR_ASSERT_EQ(s2.trianglesSubmitted, 1);
    SR_ASSERT_EQ(s2.trianglesRasterized, 1);
    SR_ASSERT_EQ(c.rt.color.readPacked(kBx, kBy), green);  // 画的是 B
    SR_ASSERT_EQ(c.rt.color.readPacked(kAx, kAy), kBg);    // A 一个像素都不许有
}

SR_TEST(draw_calls_are_counted_per_submission) {
    Canvas c{kW, kH};
    const sr::Mesh m = twoTriangles();
    sr::BackendA backend;

    // 两次提交共 3 个三角形：drawCalls 必须是 2 而不是 3 —— 数的是入口，不是三角形。
    // （若自增挪进三角形循环（变异②），这里得到 3。）
    sr::FrameStats s1;
    auto st1 = makeState(c, s1);
    backend.drawRange(m, st1, 0, 6);   // 2 个三角形
    backend.drawRange(m, st1, 0, 3);   // 1 个三角形
    SR_ASSERT_EQ(s1.drawCalls, 2);

    // 整网格 draw 是非虚包装 = 一次 drawRange ⇒ 恰好 1（变异②下会是 2）。
    sr::FrameStats s2;
    auto st2 = makeState(c, s2);
    backend.draw(m, st2);
    SR_ASSERT_EQ(s2.drawCalls, 1);
    SR_ASSERT_EQ(s2.trianglesSubmitted, 2);

    // 空区间也是一次提交：循环跑 0 次，drawCalls 仍须 +1（变异②下会是 0）。
    sr::FrameStats s3;
    auto st3 = makeState(c, s3);
    backend.drawRange(m, st3, 0, 0);
    SR_ASSERT_EQ(s3.drawCalls, 1);
    SR_ASSERT_EQ(s3.trianglesSubmitted, 0);
    SR_ASSERT_EQ(s3.trianglesRasterized, 0);
}

SR_TEST(draw_range_with_a_truncated_tail_drops_it_and_counts_degenerate) {
    // 防御性分支（Ruling 45）：parseObj（M5.2）保证 group 一律 3 对齐，生产代码目前
    // **到不了**这条尾巴 —— 保留它不是死代码，而是"一个坏 group 只记一次 degenerate，
    // 不许断言带走整帧、更不许越界读"（Review Focus 3）。
    // 6 索引的网格只提交 5 个：完整三角形只有 A；残缺的 2 索引尾巴不许长成 B
    // （循环条件若写成 i + 2 <= end（变异③），indices[5] 会被多吃进来，B 被画出）。
    Canvas c{kW, kH};
    const sr::Mesh m = twoTriangles();
    sr::BackendA backend;
    sr::FrameStats s;
    auto st = makeState(c, s);
    c.rt.color.clear(kBg);
    backend.drawRange(m, st, 0, 5);
    SR_ASSERT_EQ(s.trianglesSubmitted, 1);
    SR_ASSERT_EQ(s.trianglesRasterized, 1);
    SR_ASSERT_EQ(s.trianglesDegenerate, 1);
    SR_ASSERT_EQ(c.rt.color.readPacked(kAx, kAy), sr::encodeSRGB(sr::LinearColor{1, 0, 0}));
    SR_ASSERT_EQ(c.rt.color.readPacked(kBx, kBy), kBg);
}
