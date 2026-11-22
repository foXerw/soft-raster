#include "scene/fit.h"

#include <cmath>

#include "harness.h"
#include "math/bbox.h"
#include "math/transform.h"
#include "math/vec.h"

namespace {

constexpr float kPi = 3.14159265358979323846f;
// 精确 π/3 而不是 golden 的 1.047f：sin(π/6) = 0.5，期望值才是有限小数
// （distance = 2r = 2.77128…）。1.047f 会给出 2.77176，与 brief 的数字对不上。
constexpr float kFovY = kPi / 3.0f;

sr::BBox cube(float size, const sr::Vec3& center) {
    const float h = size * 0.5f;
    sr::BBox b;
    b.grow(center - sr::Vec3{h, h, h});
    b.grow(center + sr::Vec3{h, h, h});
    return b;
}

// 包围盒 8 个角走 proj * view 到 clip，除 w 后必须全部落在 [-1,1]^3。
// w <= 0 时 perspectiveDivide 给出 inf/NaN：isfinite 与区间断言会红，不会 abort（trap (b)）。
void assertAllCornersInsideNdc(const sr::BBox& bounds, const sr::Mat4& proj, const sr::Mat4& view) {
    const sr::Mat4 pv = proj * view;
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            for (int k = 0; k < 2; ++k) {
                const sr::Vec3 corner{i ? bounds.max.x : bounds.min.x,
                                      j ? bounds.max.y : bounds.min.y,
                                      k ? bounds.max.z : bounds.min.z};
                const sr::Vec4 clip = pv * sr::makeVec4(corner, 1.0f);
                SR_ASSERT(clip.w > 0.0f);   // 角点在相机前方；w<=0 说明 distance 小到把相机塞进了盒子里
                const sr::Vec3 ndc = sr::perspectiveDivide(clip);
                SR_ASSERT(std::isfinite(ndc.x) && std::isfinite(ndc.y) && std::isfinite(ndc.z));
                SR_ASSERT(ndc.x >= -1.0f && ndc.x <= 1.0f);
                SR_ASSERT(ndc.y >= -1.0f && ndc.y <= 1.0f);
                SR_ASSERT(ndc.z >= -1.0f && ndc.z <= 1.0f);
            }
}

}  // namespace

SR_TEST(fit_matches_the_documented_formula_on_the_m4_cube) {
    // 1.6 立方体（与 M4 golden 同边长），中心故意不在原点、base.target 非零：
    // "target 不跟着 bounds.center() 走"的变异没法靠默认值重合蒙混。
    const sr::Vec3 center{2.0f, -3.0f, 7.0f};
    const sr::BBox bounds = cube(1.6f, center);
    sr::OrbitCamera base;
    base.yaw = 0.6f;
    base.pitch = -0.23f;
    base.distance = 9.0f;
    base.target = sr::Vec3{9.0f, 9.0f, 9.0f};
    const sr::FittedCamera f = sr::fitCamera(bounds, kFovY, 16.0f / 9.0f, base);
    // r = 0.8*sqrt(3) = 1.3856406；fovY = π/3 ⇒ sin(fovY/2) = 0.5 ⇒ distance = 2r ≈ 2.77128、
    // near = r/1000 ≈ 0.001386、far = r*10 ≈ 13.8564。
    // 这四个数字**就是** D15 里"fit 相机与 M4 姿态是两张无关的图"的证据：
    // M4 golden 相机在 distance 5 看原点，fit 给出 2.77128 看 (2,-3,7)——
    // fit 只发生在调用方自己的相机里，golden 的姿态常量（golden_scene.h:23-27）继续有效。
    SR_ASSERT_NEAR(f.camera.distance, 2.77128f, 1e-5f);
    SR_ASSERT_NEAR(f.nearPlane, 0.001386f, 1e-5f);
    SR_ASSERT_NEAR(f.farPlane, 13.8564f, 1e-5f);
    SR_ASSERT_NEAR(f.camera.target.x, center.x, 1e-5f);
    SR_ASSERT_NEAR(f.camera.target.y, center.y, 1e-5f);
    SR_ASSERT_NEAR(f.camera.target.z, center.z, 1e-5f);
}

SR_TEST(fit_keeps_yaw_and_pitch_of_the_base_camera) {
    // 按 F 的人不是为了被重置视角（D15）。trap (a)：base 的 yaw/pitch 非零且不对称，
    // "yaw 归零 / pitch 回默认 0.35"的变异都会红。distance 9 ≠ fit 值 2.77、
    // target (1,2,3) ≠ 盒中心原点：同时断言它们**变了**，"原样返回 base"的空实现
    // 不能靠 yaw/pitch 断言冒充"继承了视角"。
    sr::OrbitCamera base;
    base.yaw = 0.6f;
    base.pitch = -0.23f;
    base.distance = 9.0f;
    base.target = sr::Vec3{1.0f, 2.0f, 3.0f};
    const sr::BBox bounds = cube(1.6f, sr::Vec3{0.0f, 0.0f, 0.0f});
    const sr::FittedCamera f = sr::fitCamera(bounds, kFovY, 16.0f / 9.0f, base);
    SR_ASSERT_NEAR(f.camera.yaw, 0.6f, 1e-6f);
    SR_ASSERT_NEAR(f.camera.pitch, -0.23f, 1e-6f);
    SR_ASSERT_NEAR(f.camera.distance, 2.77128f, 1e-5f);
    SR_ASSERT_NEAR(f.camera.target.x, 0.0f, 1e-6f);
    SR_ASSERT_NEAR(f.camera.target.y, 0.0f, 1e-6f);
    SR_ASSERT_NEAR(f.camera.target.z, 0.0f, 1e-6f);
}

SR_TEST(fit_puts_the_whole_bbox_inside_ndc_at_three_scales) {
    // D16 的主角，G-M5.1 的唯一自动化守卫：mm 模型（1600）与 m 模型（0.001）都要立刻可见。
    // 一条包含断言同时守三个数：near 太大 ⇒ 近角 z < -1；far 太小 ⇒ 远角 z > 1；
    // distance 太小 ⇒ x/y 溢出。
    // 但包含是**单向**守卫（Ruling 67）：near 缩小、far 放大、distance 放大都不会让任何角出界，
    // 甚至 near 放大 10 倍（r/100）也裁不到任何角——60° fov 下最近角深度 ≈ d-r ≈ r，
    // 比 r/100 还远两个数量级。所以每个尺度再按公式逐字段断言 distance/near/far，
    // 三个输出各挡各的变异，不依赖包含断言的对称性。
    // 这组数值断言还挡"退化阈值放宽到 1e-3"：0.001 立方体 r = 8.66e-4 会被误判退化而退回
    // base（distance 5），小立方体在 base 相机下照样在 NDC 内——包含绿，只有数值红。
    const float sizes[3] = {0.001f, 1.6f, 1600.0f};
    for (float size : sizes) {
        const sr::BBox bounds = cube(size, sr::Vec3{0.0f, 0.0f, 0.0f});
        const float r = bounds.radius();
        sr::OrbitCamera base;
        base.yaw = 0.6f;
        base.pitch = 0.35f;
        base.distance = 5.0f;
        const sr::FittedCamera f = sr::fitCamera(bounds, kFovY, 16.0f / 9.0f, base);
        // aspect > 1 ⇒ 垂直 fov 更紧 ⇒ distance = r/sin(π/6) = 2r。
        // 容差随 r 缩放（C1′）：1600 尺度 distance ≈ 2771，float ulp ≈ 2.4e-4，固定 1e-5 会假性失败。
        const float tol = 1e-5f * r;
        SR_ASSERT_NEAR(f.camera.distance, 2.0f * r, tol);
        SR_ASSERT_NEAR(f.nearPlane, r / 1000.0f, tol);
        SR_ASSERT_NEAR(f.farPlane, r * 10.0f, tol);
        const sr::Mat4 proj = sr::makePerspective(kFovY, 16.0f / 9.0f, f.nearPlane, f.farPlane);
        // makePerspective 对非正 near/far 退回单位阵：0.001 尺度下单位阵也能"包含"小立方体，
        // 先钉住 proj 真是透视矩阵（m[3][2] = -1 是 w' = -z 的全部来源，见 transform.h:28）。
        SR_ASSERT_NEAR(proj.m[3][2], -1.0f, 1e-6f);
        assertAllCornersInsideNdc(bounds, proj, f.camera.view());
    }
}

SR_TEST(fit_survives_an_empty_bbox_without_nan) {
    // Ruling 49：空盒 radius()==0，公式给出 distance=near=far=0，拿它建投影会让整帧消失
    // （投影除以 far-near ⇒ NaN/inf；本项目 makePerspective 对非正 near/far 退回单位阵，
    // 症状同样是整帧画不出东西）——比"相机没动"糟糕得多。安全响应是**不动作**：
    // base 原样返回 + 结构体默认 near/far（0.1/100），不做任何"小正数 distance"替代。
    // base.distance 取 7 而不是默认 5：挡"一律退回 5.0"的变异。
    sr::OrbitCamera base;
    base.yaw = 0.6f;
    base.pitch = -0.23f;
    base.distance = 7.0f;
    base.target = sr::Vec3{1.0f, 2.0f, 3.0f};
    const sr::BBox empty;
    SR_ASSERT(empty.empty());
    const sr::FittedCamera f = sr::fitCamera(empty, kFovY, 16.0f / 9.0f, base);
    SR_ASSERT_NEAR(f.camera.distance, 7.0f, 1e-6f);
    SR_ASSERT_NEAR(f.camera.yaw, 0.6f, 1e-6f);
    SR_ASSERT_NEAR(f.camera.pitch, -0.23f, 1e-6f);
    SR_ASSERT_NEAR(f.camera.target.x, 1.0f, 1e-6f);
    SR_ASSERT_NEAR(f.camera.target.y, 2.0f, 1e-6f);
    SR_ASSERT_NEAR(f.camera.target.z, 3.0f, 1e-6f);
    SR_ASSERT_NEAR(f.nearPlane, 0.1f, 1e-6f);
    SR_ASSERT_NEAR(f.farPlane, 100.0f, 1e-6f);
    // 返回的相机 + near/far 必须真能建出有限投影：把空盒 center()（原点）投一次，
    // 证明"退回默认"不是另一条 NaN 路径。
    const sr::Mat4 proj = sr::makePerspective(kFovY, 16.0f / 9.0f, f.nearPlane, f.farPlane);
    const sr::Mat4 view = f.camera.view();
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col) {
            SR_ASSERT(std::isfinite(proj.m[row][col]));
            SR_ASSERT(std::isfinite(view.m[row][col]));
        }
    const sr::Vec4 clip = proj * view * sr::makeVec4(empty.center(), 1.0f);
    SR_ASSERT(clip.w > 0.0f);
    const sr::Vec3 ndc = sr::perspectiveDivide(clip);
    SR_ASSERT(std::isfinite(ndc.x) && std::isfinite(ndc.y) && std::isfinite(ndc.z));
    SR_ASSERT(ndc.x >= -1.0f && ndc.x <= 1.0f);
    SR_ASSERT(ndc.y >= -1.0f && ndc.y <= 1.0f);
    SR_ASSERT(ndc.z >= -1.0f && ndc.z <= 1.0f);
}

SR_TEST(fits_vertical_fov_wins_in_a_wide_window_and_horizontal_in_a_tall_one) {
    // aspect 16:9 ⇒ fovX > fovY，垂直更紧 ⇒ distance = 2r ≈ 2.77128；
    // aspect 9:16 ⇒ fovX < fovY，水平更紧 ⇒ 必须退得更远（4.48603 > 2.77128）。
    // "永远用 fovY"的变异在宽窗口下与正确实现完全一致（min 本来就取 fovY），
    // 只有高瘦窗口这条会红——这个用例就是为它存在的。
    const sr::BBox bounds = cube(1.6f, sr::Vec3{0.0f, 0.0f, 0.0f});
    const float r = bounds.radius();
    sr::OrbitCamera base;
    base.yaw = 0.6f;
    base.pitch = 0.35f;
    const sr::FittedCamera wide = sr::fitCamera(bounds, kFovY, 16.0f / 9.0f, base);
    const sr::FittedCamera tall = sr::fitCamera(bounds, kFovY, 9.0f / 16.0f, base);
    SR_ASSERT_NEAR(wide.camera.distance, 2.0f * r, 1e-5f);
    // fovX = 2*atan(tan(π/6)*9/16) = 0.6280288，distance = r/sin(fovX/2) = 4.4860277。
    SR_ASSERT_NEAR(tall.camera.distance, 4.48603f, 1e-5f);
    SR_ASSERT(tall.camera.distance > wide.camera.distance);
    const sr::Mat4 projWide = sr::makePerspective(kFovY, 16.0f / 9.0f, wide.nearPlane, wide.farPlane);
    assertAllCornersInsideNdc(bounds, projWide, wide.camera.view());
    const sr::Mat4 projTall = sr::makePerspective(kFovY, 9.0f / 16.0f, tall.nearPlane, tall.farPlane);
    assertAllCornersInsideNdc(bounds, projTall, tall.camera.view());
}

SR_TEST(fit_returns_base_unchanged_for_a_sub_epsilon_bbox) {
    // Ruling 49 的第二个条件：非空但 radius < 1e-6 同样退化。
    // 1e-7 立方体：r = 5e-8*sqrt(3) = 8.66e-8，非空、radius > 0、但低于阈值。
    // base 各字段全非默认（distance 9、yaw -0.4、pitch 0.5、target (-2,4,6)）：
    // "退回默认相机"与"照常 fit"两种变异都会红。
    sr::OrbitCamera base;
    base.yaw = -0.4f;
    base.pitch = 0.5f;
    base.distance = 9.0f;
    base.target = sr::Vec3{-2.0f, 4.0f, 6.0f};
    const sr::BBox tiny = cube(1e-7f, sr::Vec3{0.0f, 0.0f, 0.0f});
    SR_ASSERT_FALSE(tiny.empty());
    SR_ASSERT(tiny.radius() > 0.0f);
    SR_ASSERT(tiny.radius() < 1e-6f);
    const sr::FittedCamera f = sr::fitCamera(tiny, kFovY, 1.0f, base);
    SR_ASSERT_NEAR(f.camera.distance, 9.0f, 1e-6f);
    SR_ASSERT_NEAR(f.camera.yaw, -0.4f, 1e-6f);
    SR_ASSERT_NEAR(f.camera.pitch, 0.5f, 1e-6f);
    SR_ASSERT_NEAR(f.camera.target.x, -2.0f, 1e-6f);
    SR_ASSERT_NEAR(f.camera.target.y, 4.0f, 1e-6f);
    SR_ASSERT_NEAR(f.camera.target.z, 6.0f, 1e-6f);
    SR_ASSERT_NEAR(f.nearPlane, 0.1f, 1e-6f);
    SR_ASSERT_NEAR(f.farPlane, 100.0f, 1e-6f);
}
