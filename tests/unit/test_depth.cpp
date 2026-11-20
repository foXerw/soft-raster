#include <cmath>

#include "harness.h"
#include "math/transform.h"
#include "render/builtin_shaders.h"
#include "render/pipeline.h"
#include "render/render_backend.h"
#include "render/stats.h"
#include "scene/mesh.h"
#include "test_util.h"

namespace {

// 打包值是 0xAARRGGBB（内存字节序 B,G,R,A），所以蓝是 0xFF0000FF 而不是 0xFFFF0000。
// 这条在 Task 0.3 就被测试钉过，本任务的第一版又写反了一次 —— 说明这类常量
// 最好由 encodeSRGB 现算，而不是手敲十六进制。
constexpr uint32_t kNear = 0xFF00FF00u;   // 绿
constexpr uint32_t kFar = 0xFF0000FFu;    // 蓝

// 两个共面重叠的三角形，一个近一个远，覆盖同一块中心区域。
void drawTwoOverlapping(Canvas& c, bool depthTest, bool depthWrite, sr::FrameStats& stats) {
    sr::PipelineState st;
    st.target = &c.rt;
    st.stats = &stats;
    st.cull.backfaceCull = false;
    st.depthTest = depthTest;
    st.depthWrite = depthWrite;
    st.vertexShader = sr::transformVS;
    st.fragmentShader = sr::vertexColorFS;

    sr::VertexUniforms u;
    u.model = sr::Mat4::identity();
    u.view = sr::makeLookAt({0, 0, 5}, {0, 0, 0}, {0, 1, 0});
    u.proj = sr::makePerspective(1.047f, 1.0f, 0.1f, 100.0f);
    st.vu = u;

    sr::Mesh far;
    far.positions = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
    far.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    far.uvs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    far.colors = {{0, 0, 1, 1}, {0, 0, 1, 1}, {0, 0, 1, 1}, {0, 0, 1, 1}};
    far.indices = {0, 1, 2, 0, 2, 3};

    sr::Mesh near;
    near.positions = {{-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f},
                      {-0.5f, 0.5f, 0.5f}};
    near.normals = far.normals;
    near.uvs = far.uvs;
    near.colors = {{0, 1, 0, 1}, {0, 1, 0, 1}, {0, 1, 0, 1}, {0, 1, 0, 1}};
    near.indices = {0, 1, 2, 0, 2, 3};

    sr::BackendA backend;
    // 顺序是**近的先画、远的后画**：这样 depthTest 开启时"远的被拒"才会真的发生
    // （反过来画的话一个都不会被拒，fragmentsDepthRejected 会是 0，
    //  于是那条断言测的其实是"绘制顺序恰好让深度没机会起作用"）。
    backend.draw(near, st);
    backend.draw(far, st);
}

}  // namespace

SR_TEST(depth_target_clears_to_far_by_default) {
    Canvas c{8, 8};
    c.rt.depth.clear(0.25f);
    SR_ASSERT_NEAR(c.rt.depth.read(0, 0), 0.25f, 1e-6f);
    c.rt.depth.clear();
    SR_ASSERT_NEAR(c.rt.depth.read(7, 7), 1.0f, 1e-6f);   // 约定：默认最远 = 1.0
}

SR_TEST(nearer_triangle_wins_when_depth_test_is_on) {
    Canvas c{64, 64};
    c.rt.color.clear(0xFF000000u);
    c.rt.depth.clear();
    sr::FrameStats stats;
    drawTwoOverlapping(c, true, true, stats);

    SR_ASSERT_EQ(c.rt.color.readPacked(32, 32), kNear);
    SR_ASSERT(stats.fragmentsDepthRejected > 0);
}

SR_TEST(farther_triangle_wins_when_depth_test_is_off) {
    // 同一场景、只差一个标志：这条与上一条并排读，就是"深度测试做了什么"最直接的证明。
    Canvas c{64, 64};
    c.rt.color.clear(0xFF000000u);
    c.rt.depth.clear();
    sr::FrameStats stats;
    drawTwoOverlapping(c, false, true, stats);

    SR_ASSERT_EQ(c.rt.color.readPacked(32, 32), kFar);    // 后画的总赢（painter 缺陷）
    SR_ASSERT_EQ(stats.fragmentsDepthRejected, 0);
}

SR_TEST(coplanar_triangles_last_writer_wins_because_comparison_is_less_or_equal) {
    // 设计文档 §7.1 定死 depth 比较用 `<=`。共面时两边 depth 相等，
    // `<=` 让后画的赢 —— 这就是 Z-fighting 的取舍，测试把它 pin 住而不是假装它不存在。
    Canvas c{32, 32};
    c.rt.color.clear(0xFF000000u);
    c.rt.depth.clear();

    sr::PipelineState st;
    st.target = &c.rt;
    st.cull.backfaceCull = false;
    st.depthTest = true;
    st.depthWrite = true;
    st.vertexShader = sr::transformVS;
    st.fragmentShader = sr::vertexColorFS;
    st.vu.model = sr::Mat4::identity();
    st.vu.view = sr::makeLookAt({0, 0, 5}, {0, 0, 0}, {0, 1, 0});
    st.vu.proj = sr::makePerspective(1.047f, 1.0f, 0.1f, 100.0f);

    sr::Mesh a;
    a.positions = {{-1, -1, 0}, {1, -1, 0}, {0, 1, 0}};
    a.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    a.uvs = {{0, 0}, {1, 0}, {0.5f, 1}};
    a.colors = {{1, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 1}};
    a.indices = {0, 1, 2};

    sr::Mesh b = a;
    b.colors = {{0, 1, 1, 1}, {0, 1, 1, 1}, {0, 1, 1, 1}};   // 完全共面，只换颜色

    sr::BackendA backend;
    backend.draw(a, st);
    const uint32_t afterFirst = c.rt.color.readPacked(16, 16);
    backend.draw(b, st);
    const uint32_t afterSecond = c.rt.color.readPacked(16, 16);

    SR_ASSERT(afterFirst != 0xFF000000u);
    SR_ASSERT(afterFirst != afterSecond);   // `<=` ⇒ 第二个也写进去了
}

SR_TEST(depth_write_false_does_not_occlude_and_does_not_pollute) {
    // 症状分离：如果 depthWrite 被错误地绑在 depthTest 上，这条会失败而上一条通过。
    Canvas c{64, 64};
    c.rt.color.clear(0xFF000000u);
    c.rt.depth.clear();

    sr::PipelineState st;
    st.target = &c.rt;
    st.cull.backfaceCull = false;
    st.depthTest = true;
    st.depthWrite = false;         // 被测物体不写深度
    st.vertexShader = sr::transformVS;
    st.fragmentShader = sr::vertexColorFS;
    st.vu.model = sr::Mat4::identity();
    st.vu.view = sr::makeLookAt({0, 0, 5}, {0, 0, 0}, {0, 1, 0});
    st.vu.proj = sr::makePerspective(1.047f, 1.0f, 0.1f, 100.0f);

    sr::Mesh nearPlane;
    nearPlane.positions = {{-0.5f, -0.5f, 1.0f}, {0.5f, -0.5f, 1.0f}, {0.5f, 0.5f, 1.0f},
                           {-0.5f, 0.5f, 1.0f}};
    nearPlane.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    nearPlane.uvs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    nearPlane.colors = {{1, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 1}, {1, 0, 0, 1}};
    nearPlane.indices = {0, 1, 2, 0, 2, 3};

    sr::Mesh farPlane = nearPlane;
    for (auto& p : farPlane.positions) p.z = -1.0f;      // 更远
    farPlane.colors = {{0, 0, 1, 1}, {0, 0, 1, 1}, {0, 0, 1, 1}, {0, 0, 1, 1}};

    sr::BackendA backend;
    backend.draw(nearPlane, st);
    const float depthAfterNear = c.rt.depth.read(32, 32);
    SR_ASSERT_NEAR(depthAfterNear, 1.0f, 1e-6f);   // 没写进去

    backend.draw(farPlane, st);
    SR_ASSERT_EQ(c.rt.color.readPacked(32, 32), kFar);   // 远的照样画出来（没被遮挡）
}

SR_TEST(cube_depth_flag_flips_which_face_wins) {
    // M2 的缺陷测试与 M3 的修复写在同一个场景里，只差 depthTest 一个标志。
    const sr::Mesh cube = sr::makeCube(2.0f, true);
    const uint32_t frontColor = sr::encodeSRGB({cube.colors[16].x, cube.colors[16].y, cube.colors[16].z});
    const uint32_t backColor = sr::encodeSRGB({cube.colors[20].x, cube.colors[20].y, cube.colors[20].z});

    auto draw = [&](bool depthTest) {
        Canvas c{160, 90};
        c.rt.color.clear(0xFF000000u);
        c.rt.depth.clear();
        sr::FrameStats stats;
        sr::PipelineState st;
        st.target = &c.rt;
        st.stats = &stats;
        st.vertexShader = sr::transformVS;
        st.fragmentShader = sr::vertexColorFS;
        st.cull.backfaceCull = false;      // 凸体正对姿态：不剔除才有穿帮
        st.depthTest = depthTest;
        st.depthWrite = depthTest;
        st.vu.model = sr::Mat4::identity();
        st.vu.view = sr::makeLookAt({0, 0, 5}, {0, 0, 0}, {0, 1, 0});
        st.vu.proj = sr::makePerspective(1.047f, 160.0f / 90.0f, 0.1f, 100.0f);
        sr::BackendA backend;
        backend.draw(cube, st);
        return c.rt.color.readPacked(80, 45);
    };

    SR_ASSERT_EQ(draw(false), backColor);    // painter 缺陷
    SR_ASSERT_EQ(draw(true), frontColor);    // 深度修正了它
}

SR_TEST(depth_values_are_written_and_monotonic_with_distance) {
    // 近处的深度值必须更小（非 reverse-Z）。这条与"哪个面赢"是两件事：
    // 前者是数值方向，后者是行为。方向写反的症状是所有遮挡反过来。
    Canvas c{64, 64};
    c.rt.color.clear(0xFF000000u);
    c.rt.depth.clear();
    sr::FrameStats stats;
    drawTwoOverlapping(c, true, true, stats);
    const float center = c.rt.depth.read(32, 32);
    const float edge = c.rt.depth.read(4, 4);   // 只有远平面覆盖到的区域
    SR_ASSERT(center < edge);
    SR_ASSERT(center >= 0.0f && edge <= 1.0f);
}
