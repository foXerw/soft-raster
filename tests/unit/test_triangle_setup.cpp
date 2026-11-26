#include "render/triangle_setup.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "harness.h"
#include "math/transform.h"
#include "math/vec.h"
#include "render/builtin_shaders.h"
#include "render/clipping.h"
#include "render/pipeline.h"
#include "render/viewport.h"
#include "scene/mesh.h"
#include "test_util.h"

namespace {

constexpr float kEps = 1e-4f;

sr::ScreenVertex makeSV(float x, float y, float depth, float w, const sr::Varyings& v) {
    sr::ScreenVertex s;
    s.pos = sr::Vec2{x, y};
    s.depth = depth;
    s.w = w;
    s.invW = 1.0f / w;
    s.v = v;
    return s;
}

sr::Varyings varyingsWithColor(float r) {
    sr::Varyings v;
    v.color = sr::Vec4{r, 0, 0, 1};
    return v;
}

sr::CullState cullOn() {
    sr::CullState c;
    c.backfaceCull = true;
    c.frontFaceIsClockwise = true;
    return c;
}

sr::CullState cullOff() {
    sr::CullState c;
    c.backfaceCull = false;
    c.frontFaceIsClockwise = true;
    return c;
}

}  // namespace

SR_TEST(front_facing_triangles_have_negative_signed_area_in_top_down_pixels) {
    // 符号约定不能靠口算"这是顺时针还是逆时针" —— 在 y 朝下的数组里那句话本身就容易被读反。
    // 推导链只有四步，写在这里当作权威：
    //   1. Mesh 生成器约定：从物体外面看是逆时针（右手系，世界空间）。
    //   2. view*proj 保持这个手性进入 NDC（y 朝上）：正面三角形的 shoelace 叉积为正。
    //   3. Viewport 把 y 翻成 top-down（C8）⇒ 叉积符号翻转。
    //   4. 所以：**屏幕空间里 area2 < 0 的是正面**。注意 y 翻转只翻有向面积的**符号**，不翻肉眼旋向：
    //      正面在屏幕上仍是**逆**时针（旧版此处写"这就是 GPU 文档说的 clockwise"，178e9db 实测更正）。
    Canvas c{16, 16};
    const sr::ScreenVertex front[3] = {makeSV(0, 0, 0.5f, 1, varyingsWithColor(1)),
                                       makeSV(0, 10, 0.5f, 1, varyingsWithColor(0)),
                                       makeSV(10, 0, 0.5f, 1, varyingsWithColor(0))};
    sr::TriangleSetup s;
    sr::SetupResult res;
    SR_ASSERT(sr::setupTriangle(front, c.rt.color, cullOn(), s, res));
    SR_ASSERT_FALSE(res.rejected());
    SR_ASSERT(s.area2 < 0.0f);
    SR_ASSERT_FALSE(s.backFacing);

    const sr::ScreenVertex back[3] = {front[0], front[2], front[1]};
    sr::TriangleSetup s2;
    sr::SetupResult res2;
    SR_ASSERT(!sr::setupTriangle(back, c.rt.color, cullOn(), s2, res2));
    SR_ASSERT(s2.area2 > 0.0f);
    SR_ASSERT(s2.backFacing);
    SR_ASSERT_EQ(static_cast<int>(res2.reason), static_cast<int>(sr::RejectReason::Backface));
}

SR_TEST(culling_accepts_exactly_the_faces_whose_normals_face_the_camera) {
    // 与符号约定无关的语义判据，也是上面那条测试的权威来源：
    // 一个面该不该画，取决于它的外法线是否朝向相机，而不是取决于我怎么数顺时针。
    const sr::Vec3 eye{0, 0, 5};
    const sr::Mesh cube = sr::makeCube(2.0f, true);

    const auto facingTriangles = [&](const sr::Mat4& model) {
        int n = 0;
        for (size_t i = 0; i + 2 < cube.indices.size(); i += 3) {
            const sr::Vec3 a = sr::xyz(model * sr::makeVec4(cube.positions[cube.indices[i]], 1));
            const sr::Vec3 b = sr::xyz(model * sr::makeVec4(cube.positions[cube.indices[i + 1]], 1));
            const sr::Vec3 cc = sr::xyz(model * sr::makeVec4(cube.positions[cube.indices[i + 2]], 1));
            const sr::Vec3 nrm = sr::normalize(sr::cross(b - a, cc - a));
            if (sr::dot(nrm, eye - a) > 0.0f) ++n;
        }
        return n;
    };

    // 正对姿态：只有 +Z 那个面（2 个三角形）朝向相机。4 个侧面在这里**不是棱对**——
    // eye=(0,0,5) 时它们的 dot = -1（-Z 面是 -6），所以是实打实的背对，退化数为 0。
    // （旧注释说侧面"正好棱对、故 8 个退化"，那是正交投影的直觉，透视下不成立；
    //  见 docs/notes/M2-几何阶段.md 的撤回与 test_pipeline_integration.cpp 的实测。）
    SR_ASSERT_EQ(facingTriangles(sr::Mat4::identity()), 2);

    // 一般姿态：3 个面朝向相机 ⇒ 6 个三角形。这才是"恰好一半"的来源，
    // 而不是任何绕向假设。
    SR_ASSERT_EQ(facingTriangles(sr::Mat4::rotationX(0.43f) * sr::Mat4::rotationY(0.61f)), 6);
}

SR_TEST(setup_culls_exactly_the_three_far_faces_of_a_cube) {
    // 这是把"绕向 + y 翻转 + 剔除"三者耦合钉死的实用办法：
    // 一般姿态下立方体 12 个三角形里恰好 6 个正面、6 个背面；
    // 把 frontFace 反过来，通过的必须是**另外那 6 个**（索引集合互不相交）。
    // 只断言"数量相等"是不够的 —— 那会放过"剔错了面"的实现。
    const sr::Mesh cube = sr::makeCube(2.0f, true);
    const sr::Mat4 model = sr::Mat4::rotationX(0.43f) * sr::Mat4::rotationY(0.61f);
    const sr::Mat4 view = sr::makeLookAt({0, 0, 5}, {0, 0, 0}, {0, 1, 0});
    const sr::Mat4 proj = sr::makePerspective(1.047f, 16.0f / 9.0f, 0.1f, 100.0f);
    const sr::Viewport vp{0, 0, 160, 90};

    std::vector<int> passedCw, passedCcw;
    for (size_t i = 0; i + 2 < cube.indices.size(); i += 3) {
        sr::ClipVertex cv[3];
        for (int k = 0; k < 3; ++k) {
            sr::VSIn in;
            in.position = cube.positions[cube.indices[i + k]];
            in.normal = cube.normals[cube.indices[i + k]];
            in.uv = cube.uvs[cube.indices[i + k]];
            sr::VertexUniforms u;
            u.model = model;
            u.view = view;
            u.proj = proj;
            cv[k] = sr::transformVS(in, u);
        }
        sr::ClipVertex clipped[sr::kMaxClippedVerts];
        const int n = sr::clipPolygonToFrustum(cv, clipped);
        if (n < 3) continue;
        for (int t = 1; t + 1 < n; ++t) {
            const sr::ClipVertex tri[3] = {clipped[0], clipped[t], clipped[t + 1]};
            sr::ScreenVertex sv[3];
            for (int k = 0; k < 3; ++k) sv[k] = vp.project(tri[k]);

            Canvas c{160, 90};
            sr::TriangleSetup s;
            sr::SetupResult res;
            if (sr::setupTriangle(sv, c.rt.color, cullOn(), s, res)) passedCw.push_back(static_cast<int>(i));
            sr::CullState flip = cullOn();
            flip.frontFaceIsClockwise = false;
            sr::TriangleSetup s2;
            sr::SetupResult res2;
            if (sr::setupTriangle(sv, c.rt.color, flip, s2, res2)) {
                passedCcw.push_back(static_cast<int>(i));
            }
        }
    }

    // 一般姿态下 12 个三角形里 6 个属于朝向相机的 3 个面（每面 2 个三角形）。
    SR_ASSERT_EQ(static_cast<int>(passedCw.size()), 6);
    SR_ASSERT_EQ(static_cast<int>(passedCcw.size()), 6);
    for (const int a : passedCw) {
        bool inOther = false;
        for (const int b : passedCcw) inOther |= (a == b);
        SR_ASSERT_FALSE(inOther);   // 两个集合必须不相交
    }
}

SR_TEST(setup_rejects_degenerate_before_testing_facing) {
    // 判定顺序是接口的一部分：面积≈0 时朝向无意义，若先判朝向，
    // 结果会随 eps 抖动（症状是同一帧里同一条棱有时画有时不画）。
    Canvas c{16, 16};
    const sr::ScreenVertex in[3] = {makeSV(1, 1, 0.5f, 1, {}), makeSV(5, 5, 0.5f, 1, {}),
                                    makeSV(9, 9, 0.5f, 1, {})};
    sr::TriangleSetup s;
    sr::SetupResult res;
    SR_ASSERT(!sr::setupTriangle(in, c.rt.color, cullOn(), s, res));
    SR_ASSERT_EQ(static_cast<int>(res.reason), static_cast<int>(sr::RejectReason::Degenerate));
}

SR_TEST(setup_rejects_non_finite_vertices_without_touching_pixels) {
    // 症状对照：NaN 进 inner loop 会让包围盒变成空集或全集 —— 前者静默，
    // 后者把一大片涂掉。两种都比"直接报错"难查，所以这里必须在 Setup 就拒掉。
    Canvas c{16, 16};
    c.rt.color.clear(0xFF010203u);
    const sr::ScreenVertex in[3] = {makeSV(std::nanf(""), 1, 0.5f, 1, {}), makeSV(8, 2, 0.5f, 1, {}),
                                    makeSV(2, 9, 0.5f, 1, {})};
    sr::TriangleSetup s;
    sr::SetupResult res;
    SR_ASSERT(!sr::setupTriangle(in, c.rt.color, cullOn(), s, res));
    SR_ASSERT_EQ(static_cast<int>(res.reason), static_cast<int>(sr::RejectReason::NonFinite));
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) SR_ASSERT_EQ(c.rt.color.readPacked(x, y), 0xFF010203u);

    const sr::ScreenVertex inf[3] = {makeSV(1, 1, 0.5f, 1, {}), makeSV(INFINITY, 2, 0.5f, 1, {}),
                                     makeSV(2, 9, 0.5f, 1, {})};
    sr::SetupResult res2;
    SR_ASSERT(!sr::setupTriangle(inf, c.rt.color, cullOn(), s, res2));
    SR_ASSERT_EQ(static_cast<int>(res2.reason), static_cast<int>(sr::RejectReason::NonFinite));
}

SR_TEST(setup_clamps_the_pixel_extent_to_the_target) {
    Canvas c{32, 32};
    const sr::ScreenVertex in[3] = {makeSV(-100, -100, 0.5f, 1, {}), makeSV(400, -50, 0.5f, 1, {}),
                                    makeSV(-50, 400, 0.5f, 1, {})};
    sr::TriangleSetup s;
    sr::SetupResult res;
    SR_ASSERT(sr::setupTriangle(in, c.rt.color, cullOff(), s, res));
    SR_ASSERT(s.extent.x0 >= 0);
    SR_ASSERT(s.extent.y0 >= 0);
    SR_ASSERT(s.extent.x1 <= 31);
    SR_ASSERT(s.extent.y1 <= 31);
    SR_ASSERT_FALSE(s.extent.empty());
}

SR_TEST(setup_tile_range_covers_the_extent_and_stays_in_the_grid) {
    // 六面裁剪的价值在这里兑现：tile 范围必须是**有界且落在网格内**的，
    // 否则 M8 的分箱会写出巨大的区间，M9 的定点 edge function 还有溢出风险。
    Canvas c{128, 128};
    sr::TileGrid grid = sr::TileGrid::forTarget(128, 128, sr::kTileSizePx);
    SR_ASSERT_EQ(grid.tilesX, 4);
    SR_ASSERT_EQ(grid.tilesY, 4);

    const sr::ScreenVertex in[3] = {makeSV(5, 5, 0.5f, 1, {}), makeSV(60, 8, 0.5f, 1, {}),
                                    makeSV(10, 70, 0.5f, 1, {})};
    sr::TriangleSetup s;
    sr::SetupResult res;
    SR_ASSERT(sr::setupTriangle(in, c.rt.color, cullOff(), grid, s, res));
    // tileRange 必须覆盖 extent
    SR_ASSERT(s.tileRange.x0 * sr::kTileSizePx <= s.extent.x0);
    SR_ASSERT(s.tileRange.y0 * sr::kTileSizePx <= s.extent.y0);
    SR_ASSERT((s.tileRange.x1 + 1) * sr::kTileSizePx - 1 >= s.extent.x1);
    SR_ASSERT((s.tileRange.y1 + 1) * sr::kTileSizePx - 1 >= s.extent.y1);
    SR_ASSERT(s.tileRange.x0 >= 0 && s.tileRange.y0 >= 0);
    SR_ASSERT(s.tileRange.x1 < grid.tilesX && s.tileRange.y1 < grid.tilesY);
}

SR_TEST(setup_full_screen_triangle_tiles_the_whole_grid) {
    Canvas c{100, 100};
    const sr::TileGrid grid = sr::TileGrid::forTarget(100, 100, sr::kTileSizePx);
    const sr::ScreenVertex in[3] = {makeSV(-1000, -1000, 0.5f, 1, {}), makeSV(3000, -1000, 0.5f, 1, {}),
                                    makeSV(-1000, 3000, 0.5f, 1, {})};
    sr::TriangleSetup s;
    sr::SetupResult res;
    SR_ASSERT(sr::setupTriangle(in, c.rt.color, cullOff(), grid, s, res));
    SR_ASSERT_EQ(s.tileRange.x0, 0);
    SR_ASSERT_EQ(s.tileRange.y0, 0);
    SR_ASSERT_EQ(s.tileRange.x1, grid.tilesX - 1);
    SR_ASSERT_EQ(s.tileRange.y1, grid.tilesY - 1);
}

SR_TEST(setup_plane_inv_w_reproduces_the_vertex_values) {
    Canvas c{16, 16};
    const sr::ScreenVertex in[3] = {makeSV(1, 1, 0.2f, 1, varyingsWithColor(1)),
                                    makeSV(9, 2, 0.4f, 10, varyingsWithColor(0)),
                                    makeSV(2, 9, 0.6f, 4, varyingsWithColor(0))};
    sr::TriangleSetup s;
    sr::SetupResult res;
    SR_ASSERT(sr::setupTriangle(in, c.rt.color, cullOff(), sr::TileGrid{}, s, res));
    for (int k = 0; k < 3; ++k) {
        const float sampled = s.planeInvW.sample(in[k].pos.x, in[k].pos.y);
        SR_ASSERT_NEAR(sampled, in[k].invW, kEps);
    }
    // 每个 varying 分量都要能还原
    for (int f = 0; f < sr::kVaryingFloatCount; ++f) {
        for (int k = 0; k < 3; ++k) {
            const float want = sr::varyingFloats(in[k].v)[f];
            const float gotOverW = s.planeAttr[f].sample(in[k].pos.x, in[k].pos.y);
            SR_ASSERT_NEAR(gotOverW, want * in[k].invW, kEps);
        }
    }
}

SR_TEST(setup_computes_uv_screen_derivatives) {
    // uv 沿 x 每像素 +1、沿 y 不变（w 恒定时 dudx 就是 1）。
    // M4 的 mipmap 层级选择依赖这四个数，所以先在这里 pin 住。
    Canvas c{16, 16};
    auto withUv = [](float u, float v) {
        sr::Varyings vy;
        vy.uv = sr::Vec2{u, v};
        return vy;
    };
    const sr::ScreenVertex in[3] = {makeSV(1, 1, 0.5f, 1, withUv(0, 0)),
                                    makeSV(11, 1, 0.5f, 1, withUv(10, 0)),
                                    makeSV(1, 11, 0.5f, 1, withUv(0, 10))};
    sr::TriangleSetup s;
    sr::SetupResult res;
    SR_ASSERT(sr::setupTriangle(in, c.rt.color, cullOff(), sr::TileGrid{}, s, res));
    SR_ASSERT_NEAR(s.dudx, 1.0f, kEps);
    SR_ASSERT_NEAR(s.dudy, 0.0f, kEps);
    SR_ASSERT_NEAR(s.dvdx, 0.0f, kEps);
    SR_ASSERT_NEAR(s.dvdy, 1.0f, kEps);
}

SR_TEST(setup_offscreen_triangle_is_reported_offscreen_not_degenerate) {
    // 区分两种"什么都没画"：完全在目标外（正常，应该计入 offscreen）
    // 与退化（异常，说明上游有问题）。把它们混成一个计数会掩盖 bug。
    Canvas c{16, 16};
    const sr::ScreenVertex in[3] = {makeSV(500, 500, 0.5f, 1, {}), makeSV(600, 500, 0.5f, 1, {}),
                                    makeSV(500, 600, 0.5f, 1, {})};
    sr::TriangleSetup s;
    sr::SetupResult res;
    SR_ASSERT(!sr::setupTriangle(in, c.rt.color, cullOff(), sr::TileGrid{}, s, res));
    SR_ASSERT_EQ(static_cast<int>(res.reason), static_cast<int>(sr::RejectReason::Offscreen));
}
