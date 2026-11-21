#include <cmath>

#include "harness.h"
#include "math/transform.h"
#include "render/builtin_shaders.h"
#include "render/color.h"
#include "render/pipeline.h"
#include "render/render_backend.h"
#include "render/sampler.h"
#include "render/stats.h"
#include "render/texture.h"
#include "scene/mesh.h"
#include "test_util.h"

namespace {

// 几何与相机的推算（写在测试里，免得"看起来对"的坐标其实是猜的）：
//   相机 (0,0,3) 看原点，fovY = 60°，aspect = 1，画布 128x128。
//   z=0 平面上的半高 = 3 * tan(30°) = 1.732 ⇒ 世界 y=±1 落在屏幕 y = 64*(1∓1/1.732) ≈ 27 / 101。
//   于是屏幕 (40,40) 对应世界 (−0.65, +0.65)、(88,88) 对应 (+0.65, −0.65)。
// 四边形四个角的 uv 故意按"v=0 在上、v=1 在下"排，这样"贴图上下颠倒"这类错误
// （屏幕 top-down 与 uv 原点的耦合，正是 Review Focus 1/5 那一族）会直接反映在像素值上。
sr::Mesh texturedQuad() {
    sr::Mesh m;
    m.positions = {{-1, 1, 0}, {1, 1, 0}, {1, -1, 0}, {-1, -1, 0}};
    m.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    m.uvs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    m.indices = {0, 1, 2, 0, 2, 3};
    m.recomputeBounds();
    return m;
}

sr::PipelineState stateFor(Canvas& c, sr::FrameStats& stats, const sr::Texture& tex) {
    sr::PipelineState st;
    st.target = &c.rt;
    st.stats = &stats;
    st.vertexShader = sr::transformVS;
    st.fragmentShader = sr::texturedFS;
    st.cull.backfaceCull = false;   // 单个四边形，绕向不是本测试的对象
    st.depthTest = false;
    st.depthWrite = false;
    st.vu.model = sr::Mat4::identity();
    st.vu.view = sr::makeLookAt({0, 0, 3}, {0, 0, 0}, {0, 1, 0});
    st.vu.proj = sr::makePerspective(1.047f, 1.0f, 0.1f, 100.0f);
    st.fu.baseColor = sr::LinearColor{1, 1, 1};
    st.fu.texture = &tex;
    st.fu.sampler.wrapU = sr::WrapMode::ClampEdge;
    st.fu.sampler.wrapV = sr::WrapMode::ClampEdge;
    st.fu.sampler.filter = sr::FilterMode::Nearest;
    st.fu.sampler.useMip = false;   // 关掉 mip：让"这个像素读了哪个纹素"成为确定值
    return st;
}

// 只在 v 方向变化：上半黑、下半白。
sr::Texture vGradient() {
    return sr::makeTexture(makeGrayImage(1, 2, [](int, int y) -> uint8_t {
                               return y == 0 ? uint8_t{0} : uint8_t{255};
                           }),
                           /*colorDataIsSrgb=*/false);
}

// 只在 u 方向变化：左半黑、右半白。
sr::Texture uGradient() {
    return sr::makeTexture(makeGrayImage(2, 1, [](int x, int) -> uint8_t {
                               return x == 0 ? uint8_t{0} : uint8_t{255};
                           }),
                           /*colorDataIsSrgb=*/false);
}

float redAt(const Canvas& c, int x, int y) { return sr::decodeSRGB(c.rt.color.readPacked(x, y)).r; }

}  // namespace

SR_TEST(texture_v_axis_runs_from_screen_top_to_bottom) {
    Canvas c{128, 128};
    c.rt.color.clear(0xFF000000u);
    sr::FrameStats stats;
    const sr::Texture tex = vGradient();
    sr::BackendA backend;
    backend.draw(texturedQuad(), stateFor(c, stats, tex));

    SR_ASSERT(stats.fragmentsWritten > 100);          // 先确认真的画上了，否则下面全是假通过
    SR_ASSERT_NEAR(redAt(c, 64, 40), 0.0f, 1e-2f);    // v≈0.17 ⇒ 上半（黑）
    SR_ASSERT_NEAR(redAt(c, 64, 88), 1.0f, 1e-2f);    // v≈0.82 ⇒ 下半（白）
}

SR_TEST(texture_u_axis_runs_from_screen_left_to_right) {
    Canvas c{128, 128};
    c.rt.color.clear(0xFF000000u);
    sr::FrameStats stats;
    const sr::Texture tex = uGradient();
    sr::BackendA backend;
    backend.draw(texturedQuad(), stateFor(c, stats, tex));

    SR_ASSERT(stats.fragmentsWritten > 100);
    SR_ASSERT_NEAR(redAt(c, 40, 64), 0.0f, 1e-2f);    // u≈0.17 ⇒ 左半
    SR_ASSERT_NEAR(redAt(c, 88, 64), 1.0f, 1e-2f);    // u≈0.82 ⇒ 右半
}

SR_TEST(the_texture_follows_the_geometry_not_the_screen) {
    // 把物体绕 Z 转 180°：屏幕上同一个位置现在对应 uv 的另一端，所以黑白必须对调。
    // 这条把"贴图跟着几何走"与"贴图钉在屏幕上"区分开 —— 后者是 uv varying 没进插值时的症状，
    // 而它看起来完全不像 bug（画面仍然有纹理，只是不跟着转）。
    Canvas c{128, 128};
    c.rt.color.clear(0xFF000000u);
    sr::FrameStats stats;
    const sr::Texture tex = vGradient();
    sr::PipelineState st = stateFor(c, stats, tex);
    st.vu.model = sr::Mat4::rotationZ(3.14159265f);
    sr::BackendA backend;
    backend.draw(texturedQuad(), st);

    SR_ASSERT(stats.fragmentsWritten > 100);
    SR_ASSERT_NEAR(redAt(c, 64, 40), 1.0f, 1e-2f);    // 转了 180° ⇒ 上半变成 v≈0.82（白）
    SR_ASSERT_NEAR(redAt(c, 64, 88), 0.0f, 1e-2f);
}

SR_TEST(missing_texture_falls_back_to_base_color) {
    // Ruling A 的兜底：texture == nullptr 时输出 baseColor，而不是崩掉或画黑。
    Canvas c{128, 128};
    c.rt.color.clear(0xFF000000u);
    sr::FrameStats stats;
    const sr::Texture tex = vGradient();          // 只为拿到一个合法的 SamplerState
    sr::PipelineState st = stateFor(c, stats, tex);
    st.fu.texture = nullptr;
    st.fu.baseColor = sr::LinearColor{0.25f, 0.5f, 0.75f};
    sr::BackendA backend;
    backend.draw(texturedQuad(), st);

    SR_ASSERT(stats.fragmentsWritten > 100);
    const sr::LinearColor got = sr::decodeSRGB(c.rt.color.readPacked(64, 64));
    SR_ASSERT_NEAR(got.r, 0.25f, 2.0f / 255.0f);   // 容差是 8bit 量化的一格，不是随便取的
    SR_ASSERT_NEAR(got.g, 0.5f, 2.0f / 255.0f);
    SR_ASSERT_NEAR(got.b, 0.75f, 2.0f / 255.0f);
}

// 一个宽而扁的四边形：屏幕足迹约 400x100 像素（画布 512，1 世界单位 ≈ 148 像素）。
// **必须是非方形足迹**，理由见 minified_quad_keeps_level_zero_aliasing 里的推导。
sr::Mesh wideQuad() {
    sr::Mesh m;
    m.positions = {{-1.35f, 0.34f, 0}, {1.35f, 0.34f, 0}, {1.35f, -0.34f, 0}, {-1.35f, -0.34f, 0}};
    m.normals = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    m.uvs = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    m.indices = {0, 1, 2, 0, 2, 3};
    m.recomputeBounds();
    return m;
}

SR_TEST(minified_quad_keeps_level_zero_aliasing) {
    // 这条是整个 M4 里唯一端到端覆盖**梯度接线**的测试：render_backend 把
    // TriangleSetup 的 dudx/dudy 填进 gradU、dvdx/dvdy 填进 gradV（Ruling B 的那一行）。
    // 填反了不会崩、不会黑屏，只会让 mip 选错 —— 症状是"贴图糊"或"贴图闪"。
    //
    // 推导（第一版这条测试是**假的**，变异"把 gradU/gradV 对调"存活了，原因值得记下来）：
    //   正确接线 ⇒ rho = max(Tu/W, Tv/H)；对调后 ⇒ rho = max(Tv/W, Tu/H)。
    //   W == H 时两式恒等 ⇒ 方形屏幕足迹**永远**测不出转置。第一版正是方形的。
    // 现在：Tu=256、Tv=4、W≈400、H≈100 ⇒
    //   正确 = max(256/400, 4/100) = 0.64 ⇒ level 0（保留锯齿）
    //   对调 = max(4/400, 256/100) = 2.56 ⇒ level 1（128x2，全部 ≈127/255 的均匀灰）
    // 两者的像素分布完全不同：level 0 会取到 0 与 1 两个极端，level 1 只会给出 ~0.5。
    Canvas c{512, 512};
    c.rt.color.clear(0xFF000000u);
    sr::FrameStats stats;
    const sr::Texture tex = sr::makeTexture(
        makeGrayImage(256, 4, [](int x, int) -> uint8_t { return x % 2 == 0 ? 0 : 255; }),
        /*colorDataIsSrgb=*/false);
    sr::PipelineState st = stateFor(c, stats, tex);
    st.fu.sampler.filter = sr::FilterMode::Trilinear;
    st.fu.sampler.useMip = true;
    sr::BackendA backend;
    backend.draw(wideQuad(), st);

    SR_ASSERT(stats.fragmentsWritten > 1000);
    // 沿中间一行扫 64 个像素，统计极值与跳变次数
    float lo = 1e9f, hi = -1e9f;
    int jumps = 0;
    float prev = redAt(c, 100, 256);
    for (int x = 100; x < 164; ++x) {
        const float r = redAt(c, x, 256);
        if (r < lo) lo = r;
        if (r > hi) hi = r;
        if (std::fabs(r - prev) > 0.5f) ++jumps;
        prev = r;
    }
    SR_ASSERT(lo < 0.1f);        // level 0 才可能取到接近纯黑
    SR_ASSERT(hi > 0.9f);        // 以及接近纯白；level 1 的均匀灰两个都到不了
    SR_ASSERT(jumps >= 10);      // 锯齿：63 个相邻对里约 30 个跨过半程
}

SR_TEST(base_color_tints_the_texture_multiplicatively) {
    // 白纹理 * 彩色 tint = 那个彩色；黑纹理 * 任何 tint = 黑。
    // 这条把"tint 是乘法还是替换"钉住 —— 替换的话下面第一组断言会等于 tint 本身，
    // 而乘法在白色区域才等于 tint，两者只在**非白**纹素上分得开。
    Canvas c{128, 128};
    c.rt.color.clear(0xFF000000u);
    sr::FrameStats stats;
    const sr::Texture tex = vGradient();          // 上半 0、下半 1
    sr::PipelineState st = stateFor(c, stats, tex);
    st.fu.baseColor = sr::LinearColor{1.0f, 0.5f, 0.0f};
    sr::BackendA backend;
    backend.draw(texturedQuad(), st);

    SR_ASSERT(stats.fragmentsWritten > 100);
    const sr::LinearColor bottom = sr::decodeSRGB(c.rt.color.readPacked(64, 88));   // 纹素 1
    SR_ASSERT_NEAR(bottom.r, 1.0f, 2.0f / 255.0f);
    SR_ASSERT_NEAR(bottom.g, 0.5f, 2.0f / 255.0f);
    SR_ASSERT_NEAR(bottom.b, 0.0f, 2.0f / 255.0f);
    const sr::LinearColor top = sr::decodeSRGB(c.rt.color.readPacked(64, 40));      // 纹素 0
    SR_ASSERT_NEAR(top.r, 0.0f, 2.0f / 255.0f);
    SR_ASSERT_NEAR(top.g, 0.0f, 2.0f / 255.0f);
}
