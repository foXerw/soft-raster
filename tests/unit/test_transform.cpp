#include "math/transform.h"

#include <cmath>

#include "harness.h"
#include "math/vec.h"
#include "render/builtin_shaders.h"
#include "render/pipeline.h"
#include "scene/golden_scene.h"

namespace {

constexpr float kPi = 3.14159265358979323846f;

void expectVec3Near(const sr::Vec3& a, float x, float y, float z, float tol) {
    SR_ASSERT_NEAR(a.x, x, tol);
    SR_ASSERT_NEAR(a.y, y, tol);
    SR_ASSERT_NEAR(a.z, z, tol);
}

void expectMat4Near(const sr::Mat4& a, const sr::Mat4& b, float tol) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) SR_ASSERT_NEAR(a.m[r][c], b.m[r][c], tol);
}

}  // namespace

SR_TEST(lookAt_puts_the_eye_at_the_origin) {
    const sr::Mat4 v = sr::makeLookAt({0, 0, 5}, {0, 0, 0}, {0, 1, 0});
    expectVec3Near(sr::xyz(v * sr::makeVec4({0, 0, 5}, 1.0f)), 0, 0, 0, 1e-5f);
}

SR_TEST(lookAt_looks_along_minus_z) {
    const sr::Mat4 v = sr::makeLookAt({0, 0, 5}, {0, 0, 0}, {0, 1, 0});
    // 世界原点（相机正前方 5 米）在视图空间里是 (0, 0, -5)：右手系、朝 -z 看。
    expectVec3Near(sr::xyz(v * sr::makeVec4({0, 0, 0}, 1.0f)), 0, 0, -5, 1e-5f);
    // up 保持朝上：世界 {0,1,0} 在相机正前方 5 米、高 1 米处
    // （注意别写成 {0,1,5}：那个点与 eye 同深度，是在相机**旁边**，视图 z 会是 0）
    expectVec3Near(sr::xyz(v * sr::makeVec4({0, 1, 0}, 1.0f)), 0, 1, -5, 1e-5f);
    // 相机右侧（世界 +x）在视图空间里是 +x，同样取正前方 5 米的点
    expectVec3Near(sr::xyz(v * sr::makeVec4({1, 0, 0}, 1.0f)), 1, 0, -5, 1e-5f);
    // 而"旁边"的点视图 z 必须是 0：这条把上面那个容易写错的区分钉住
    expectVec3Near(sr::xyz(v * sr::makeVec4({0, 1, 5}, 1.0f)), 0, 1, 0, 1e-5f);
}

SR_TEST(lookAt_is_a_rigid_transform_for_orthonormal_up) {
    // 视图矩阵只做旋转 + 平移，所以两点之间的距离必须保持不变。
    const sr::Mat4 v = sr::makeLookAt({3, 2, 7}, {0, 1, 0}, {0, 1, 0});
    const sr::Vec3 a = sr::xyz(v * sr::makeVec4({1, 0, 0}, 1.0f));
    const sr::Vec3 b = sr::xyz(v * sr::makeVec4({0, 0, 1}, 1.0f));
    const float dView = sr::length(a - b);
    const float dWorld = sr::length(sr::Vec3{1, 0, 0} - sr::Vec3{0, 0, 1});
    SR_ASSERT_NEAR(dView, dWorld, 1e-5f);
}

SR_TEST(lookAt_with_degenerate_up_stays_finite) {
    // up 与视线平行时 cross 为零。约定：退回一个可用的 up，而不是让 NaN 进矩阵。
    // 症状对照：NaN 进 view 矩阵 ⇒ 整个场景消失，而且没有任何报错。
    const sr::Mat4 v = sr::makeLookAt({0, 5, 0}, {0, 0, 0}, {0, 1, 0});
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) SR_ASSERT(std::isfinite(v.m[r][c]));
    // 退化情况下仍然要把 eye 放到原点
    expectVec3Near(sr::xyz(v * sr::makeVec4({0, 5, 0}, 1.0f)), 0, 0, 0, 1e-5f);
}

SR_TEST(perspective_maps_near_and_far_to_ndc_z_bounds) {
    const sr::Mat4 p = sr::makePerspective(kPi / 2.0f, 1.0f, 1.0f, 10.0f);
    // 近平面中心：clip.z == -clip.w（NDC z = -1）
    const sr::Vec4 nearC = p * sr::Vec4{0, 0, -1, 1};
    SR_ASSERT_NEAR(nearC.z, -nearC.w, 1e-5f);
    SR_ASSERT_NEAR(nearC.w, 1.0f, 1e-5f);
    // 远平面中心：clip.z == +clip.w（NDC z = +1）
    const sr::Vec4 farC = p * sr::Vec4{0, 0, -10, 1};
    SR_ASSERT_NEAR(farC.z, farC.w, 1e-5f);
    SR_ASSERT_NEAR(farC.w, 10.0f, 1e-5f);
    // 中间某个深度必须落在两者之间，且 w 随距离增长（透视除法的来源）
    const sr::Vec4 midC = p * sr::Vec4{0, 0, -3, 1};
    SR_ASSERT(midC.z / midC.w > -1.0f && midC.z / midC.w < 1.0f);
    SR_ASSERT(midC.w > nearC.w && midC.w < farC.w);
}

SR_TEST(perspective_maps_frustum_edges_to_ndc_one) {
    const sr::Mat4 p = sr::makePerspective(kPi / 2.0f, 1.0f, 1.0f, 10.0f);
    // fovY = 90° ⇒ 近平面上 y = ±1 的点映到 NDC y = ±1
    const sr::Vec4 top = p * sr::Vec4{0, 1, -1, 1};
    SR_ASSERT_NEAR(top.y / top.w, 1.0f, 1e-5f);
    const sr::Vec4 bottom = p * sr::Vec4{0, -1, -1, 1};
    SR_ASSERT_NEAR(bottom.y / bottom.w, -1.0f, 1e-5f);
    // aspect = 1 ⇒ x 方向同样
    const sr::Vec4 right = p * sr::Vec4{1, 0, -1, 1};
    SR_ASSERT_NEAR(right.x / right.w, 1.0f, 1e-5f);
}

SR_TEST(perspective_aspect_halves_the_horizontal_range) {
    const sr::Mat4 p = sr::makePerspective(kPi / 2.0f, 2.0f, 1.0f, 10.0f);
    // aspect = 2 ⇒ 水平视野是垂直的两倍：近平面上 x = ±2 才到 NDC x = ±1
    const sr::Vec4 right = p * sr::Vec4{2, 0, -1, 1};
    SR_ASSERT_NEAR(right.x / right.w, 1.0f, 1e-5f);
    const sr::Vec4 half = p * sr::Vec4{1, 0, -1, 1};
    SR_ASSERT_NEAR(half.x / half.w, 0.5f, 1e-5f);
}

SR_TEST(perspective_fov_shrinks_the_y_scale_monotonically) {
    const float s30 = sr::makePerspective(kPi / 6.0f, 1.0f, 1.0f, 10.0f).m[1][1];
    const float s60 = sr::makePerspective(kPi / 3.0f, 1.0f, 1.0f, 10.0f).m[1][1];
    const float s90 = sr::makePerspective(kPi / 2.0f, 1.0f, 1.0f, 10.0f).m[1][1];
    SR_ASSERT(s30 > s60);
    SR_ASSERT(s60 > s90);
    SR_ASSERT_NEAR(s90, 1.0f, 1e-6f);   // fovY=90° ⇒ cot(45°) = 1
}

SR_TEST(perspective_rejects_non_positive_planes) {
    // 约定：near <= 0 或 far <= near 直接返回单位阵（调用方会看到"什么都没投影"，
    // 比除零产生的 inf/NaN 好定位得多）。
    const sr::Mat4 bad = sr::makePerspective(kPi / 2.0f, 1.0f, 0.0f, 10.0f);
    SR_ASSERT_NEAR(bad.m[0][0], 1.0f, 1e-6f);
    SR_ASSERT_NEAR(bad.m[2][3], 0.0f, 1e-6f);
    const sr::Mat4 flipped = sr::makePerspective(kPi / 2.0f, 1.0f, 5.0f, 5.0f);
    SR_ASSERT_NEAR(flipped.m[0][0], 1.0f, 1e-6f);
}

SR_TEST(view_projection_chain_puts_a_point_in_front_of_camera_at_center) {
    // 端到端：view * proj 之后，相机正前方的点落在 NDC 中心，深度在 (0,1)。
    const sr::Mat4 vp = sr::makePerspective(kPi / 3.0f, 16.0f / 9.0f, 0.1f, 100.0f) *
                        sr::makeLookAt({0, 0, 4}, {0, 0, 0}, {0, 1, 0});
    const sr::Vec4 c = vp * sr::Vec4{0, 0, 0, 1};
    SR_ASSERT_NEAR(c.x / c.w, 0.0f, 1e-5f);
    SR_ASSERT_NEAR(c.y / c.w, 0.0f, 1e-5f);
    const float ndcZ = c.z / c.w;
    SR_ASSERT(ndcZ > -1.0f && ndcZ < 1.0f);
    // 相机后面的点 w < 0：裁剪必须把它丢掉（M2 的六面裁剪会用到这个性质）
    const sr::Vec4 behind = vp * sr::Vec4{0, 0, 8, 1};
    SR_ASSERT(behind.w < 0.0f);
}

// ---------------------------------------------------------------------------
// M6.0：叫醒休眠接口。设计 §8.1 #1–#5。
// 本章主题：M3 预声明了 worldPos / normalMatrix 并注释"给 M6 用"，但没有任何
// shader 读它们 —— 于是两处错误语义（物体空间冒充世界空间、identity 冒充逆转置）
// 一直没人发现。下面的测试就是让它们从此藏不住的断言。
// ---------------------------------------------------------------------------

SR_TEST(worldPos_is_world_space_not_object_space) {
    // M6.0 之前 transformVS 写的是 v.worldPos = in.position（物体空间）。
    // 平移 (5,0,0) 下的 (1,2,3) 必须到 (6,2,3)：这条在 Step 4 之前必须红，
    // 那个红就是"字段没人读"的证据本身。
    sr::VertexUniforms u;
    u.model = sr::Mat4::translation({5, 0, 0});
    u.view = sr::Mat4::identity();
    u.proj = sr::Mat4::identity();
    u.normalMatrix = sr::Mat4::identity();
    sr::VSIn in;
    in.position = {1, 2, 3};
    in.normal = {0, 1, 0};
    const sr::ClipVertex cv = sr::transformVS(in, u);
    expectVec3Near(cv.v.worldPos, 6.0f, 2.0f, 3.0f, 1e-5f);
}

SR_TEST(normal_matrix_undoes_non_uniform_scale_for_normals) {
    // 本章最值的一条推导：inverse-transpose 是几何必然，不是仪式。
    // model = scale(2,1,1)，物体空间的面取 p0=(0,0,0)、p1=(1,-1,0)、p2=(0,0,1)，
    // 于是 n = normalize(1,1,0) 真的垂直于原表面（下面两条点积断言钉住这个前提）。
    // 派发词里写的面是 p1=(1,0,0)、p2=(0,1,0) —— 那是 xy 平面，法线是 (0,0,1)，
    // (1,1,0)/√2 躺在面内；且 z 方向法线在 scale(2,1,1) 下不变，"朴素变换失去
    // 垂直性"的现象根本不会出现。按设计 §8.1 #1 的 n 与 scale 反推，面必须如上取。
    const sr::Mat4 model = sr::Mat4::scale({2, 1, 1});
    const sr::Vec3 p0{0, 0, 0}, p1{1, -1, 0}, p2{0, 0, 1};
    const sr::Vec3 n = sr::normalize({1, 1, 0});
    // 夹具前提：n 垂直于原表面。前提不成立时后面的推导全部失去意义，所以先钉住它。
    SR_ASSERT_NEAR(sr::dot(n, p1 - p0), 0.0f, 1e-5f);
    SR_ASSERT_NEAR(sr::dot(n, p2 - p0), 0.0f, 1e-5f);
    // 变换后的表面切向：t1 = (2,-1,0)、t2 = (0,0,1)
    const sr::Vec3 t1 = sr::xyz(model * sr::makeVec4(p1 - p0, 0.0f));
    const sr::Vec3 t2 = sr::xyz(model * sr::makeVec4(p2 - p0, 0.0f));
    // 朴素变换（直接拿 model 的 3x3 乘法线）：(2,1,0)/√2，与 t1 的点积 = 3/√2 ≈ 2.12
    const sr::Vec3 naive = sr::xyz(model * sr::makeVec4(n, 0.0f));
    SR_ASSERT(sr::dot(naive, t1) > 0.1f);
    // 法线矩阵变换后垂直性恢复。nm 故意初始化成朴素选择 model：
    // makeNormalMatrix 若没写 out（桩 / 失败），红会落在几何断言上而不是空转。
    sr::Mat4 nm = model;
    SR_ASSERT(sr::makeNormalMatrix(model, nm));
    const sr::Vec3 nOut = sr::normalize(sr::xyz(nm * sr::makeVec4(n, 0.0f)));
    SR_ASSERT_NEAR(sr::dot(nOut, t1), 0.0f, 1e-5f);
    SR_ASSERT_NEAR(sr::dot(nOut, t2), 0.0f, 1e-5f);
}

SR_TEST(normal_is_normalized_after_the_transform_not_before) {
    // D4：归一化必须在乘法**之后**。输入故意不是单位向量（长度 √2）：
    // scale(2,1,1) 的法线矩阵 diag(0.5,1,1) 会重新改变长度，变换前归一化等于没归一化 ——
    // 若只在乘法前归一化，输出长度是 √0.625 ≈ 0.7906 而不是 1。
    sr::VertexUniforms u;
    u.model = sr::Mat4::scale({2, 1, 1});
    u.view = sr::Mat4::identity();
    u.proj = sr::Mat4::identity();
    sr::Mat4 nm{};
    SR_ASSERT(sr::makeNormalMatrix(u.model, nm));
    u.normalMatrix = nm;
    sr::VSIn in;
    in.position = {0, 0, 0};
    in.normal = {1, 1, 0};
    const sr::ClipVertex cv = sr::transformVS(in, u);
    SR_ASSERT_NEAR(sr::length(cv.v.worldNormal), 1.0f, 1e-5f);
    // 方向也要对：若 shader 绕过 normalMatrix 直接用 model，长度仍是 1 但方向不同，
    // 这条分量断言把那种"碰巧单位长"的假通过挡掉。
    const sr::Vec3 want = sr::normalize(sr::xyz(nm * sr::makeVec4(in.normal, 0.0f)));
    expectVec3Near(cv.v.worldNormal, want.x, want.y, want.z, 1e-5f);
}

SR_TEST(makeNormalMatrix_rejects_a_singular_model_and_does_not_write_out) {
    // 与 tryInvert 同约定：失败 ⇒ false 且 out 一个元素都不动。
    // "不动"用 tol 0 的容差断言钉（test_mat.cpp 对 tryInvert 的既有判例）。
    const sr::Mat4 sentinel = sr::Mat4::translation({7, 8, 9});

    // 恰好奇异：一个轴缩成 0 ⇒ 秩亏，任何合理的检测都该拒。
    sr::Mat4 out = sentinel;
    SR_ASSERT_FALSE(sr::makeNormalMatrix(sr::Mat4::scale({0, 1, 1}), out));
    expectMat4Near(out, sentinel, 0.0f);

    // 接近奇异（相对阈值）：det = 1e-10 ≠ 0，但小于 tryInvert 的相对 eps = 1e-8·maxAbs³。
    // 这条是"固定阈值 det != 0"变异的杀手：数学上可逆不等于数值上该求逆 ——
    // 条件数 1e5 会把法线放大五个数量级，固定阈值在这里会放行并写坏 out。
    sr::Mat4 out2 = sentinel;
    SR_ASSERT_FALSE(sr::makeNormalMatrix(sr::Mat4::scale({1e-5f, 1e-5f, 1}), out2));
    expectMat4Near(out2, sentinel, 0.0f);
}

SR_TEST(makeNormalMatrix_of_a_pure_rotation_equals_that_rotation) {
    // 纯旋转 R 正交：inverse(R) = transpose(R) ⇒ transpose(inverse(R)) = R。
    // 这条同时是"返回 inv 不转置"变异的杀手：R 不对称，inv = Rᵀ ≠ R。
    const sr::Mat4 r = sr::Mat4::rotationX(0.43f) * sr::Mat4::rotationY(0.61f);
    sr::Mat4 nm{};
    SR_ASSERT(sr::makeNormalMatrix(r, nm));
    expectMat4Near(nm, r, 1e-5f);
}

SR_TEST(golden_scene_normal_matrix_is_no_longer_identity) {
    // 设计 §8.1 #4 / Ruling 85：grep 守卫只是绊线，真正的证明是这条 ——
    // golden 场景的法线矩阵必须**等于那个旋转本身**且**不等于 identity**。
    // M6.0 之前生产侧四处全传 identity，而 goldenModel = rotX(0.43)·rotY(0.61)
    // 是纯旋转，其法线矩阵就是它自己。今天没有 FS 读 worldNormal，所以两份
    // golden 逐字节不变；M6.2 起光照读到的是旋转后的法线，靠这条保证。
    const sr::Mat4 rot = sr::goldenModel();
    sr::Mat4 nm{};
    SR_ASSERT(sr::makeNormalMatrix(rot, nm));
    expectMat4Near(nm, rot, 1e-5f);
    // 不等于 identity：16 个元素里最大差必须远超容差（m[0][2] = sin(0.61) ≈ 0.573）。
    const sr::Mat4 id = sr::Mat4::identity();
    float maxDiff = 0.0f;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            const float d = std::fabs(nm.m[r][c] - id.m[r][c]);
            if (d > maxDiff) maxDiff = d;
        }
    SR_ASSERT(maxDiff > 0.1f);
}
