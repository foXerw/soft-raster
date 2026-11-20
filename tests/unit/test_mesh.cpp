#include "scene/mesh.h"

#include <cmath>

#include "harness.h"
#include "math/vec.h"
#include "test_util.h"

namespace {

int degenerateTriangleCount(const sr::Mesh& m) {
    int n = 0;
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        const sr::Vec3 a = m.positions[m.indices[i]];
        const sr::Vec3 b = m.positions[m.indices[i + 1]];
        const sr::Vec3 c = m.positions[m.indices[i + 2]];
        const float area2 = sr::length(sr::cross(b - a, c - a));
        if (!(area2 > 1e-6f)) ++n;
    }
    return n;
}

}  // namespace

SR_TEST(cube_per_face_normals_has_24_vertices_and_12_triangles) {
    const sr::Mesh m = sr::makeCube(2.0f, true);
    SR_ASSERT_EQ(m.positions.size(), 24u);
    SR_ASSERT_EQ(m.normals.size(), 24u);
    SR_ASSERT_EQ(m.indices.size(), 36u);
    SR_ASSERT_EQ(m.triangleCount(), 12);
    // 每面 4 个顶点（法线不同），所以 bounds 是 [-1,1]^3
    SR_ASSERT_NEAR(m.bounds.min.x, -1.0f, 1e-6f);
    SR_ASSERT_NEAR(m.bounds.max.z, 1.0f, 1e-6f);
    SR_ASSERT_NEAR(m.boundRadius(), std::sqrt(3.0f), 1e-5f);
    expectVec3Near(m.boundCenter(), 0, 0, 0, 1e-6f);
}

SR_TEST(cube_shared_normals_has_8_vertices) {
    const sr::Mesh m = sr::makeCube(2.0f, false);
    SR_ASSERT_EQ(m.positions.size(), 8u);
    SR_ASSERT_EQ(m.indices.size(), 36u);
    // 共享顶点 ⇒ 角上法线是三个面的平均，长度仍是 1（归一化过）
    for (const sr::Vec3& n : m.normals) SR_ASSERT_NEAR(sr::length(n), 1.0f, 1e-5f);
}

SR_TEST(cube_face_normals_point_outward) {
    const sr::Mesh m = sr::makeCube(2.0f, true);
    for (size_t i = 0; i < m.positions.size(); ++i) {
        const sr::Vec3 p = m.positions[i];
        const sr::Vec3 n = m.normals[i];
        // 外法线：与位置同向（立方体中心在原点）
        SR_ASSERT(sr::dot(p, n) > 0.0f);
    }
}

SR_TEST(cube_winding_is_ccw_in_ndc_so_culling_works) {
    // 绕向是 M2 背面剔除的地基：从 +z 看 +z 面，顶点必须是逆时针（NDC 约定）。
    // 用面法线与三角形叉积同向来验证（右手系）。
    const sr::Mesh m = sr::makeCube(2.0f, true);
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        const sr::Vec3 a = m.positions[m.indices[i]];
        const sr::Vec3 b = m.positions[m.indices[i + 1]];
        const sr::Vec3 c = m.positions[m.indices[i + 2]];
        const sr::Vec3 faceNormal = sr::normalize(sr::cross(b - a, c - a));
        const sr::Vec3 stored = m.normals[m.indices[i]];
        SR_ASSERT(sr::dot(faceNormal, stored) > 0.9f);
    }
}

SR_TEST(uv_sphere_vertices_lie_on_the_unit_sphere) {
    const sr::Mesh m = sr::makeUVSphere(1.0f, 8, 12);
    // 最容易写错的一条：经纬度索引错位或法线未归一，会立刻让某个顶点偏离球面。
    for (const sr::Vec3& p : m.positions) SR_ASSERT_NEAR(sr::length(p), 1.0f, 1e-5f);
    for (const sr::Vec3& n : m.normals) SR_ASSERT_NEAR(sr::length(n), 1.0f, 1e-5f);
    // 三角形数：顶/底两个 fan 各 slices 个 + 中间 (stacks-2) 行各 2*slices 个
    SR_ASSERT_EQ(m.triangleCount(), 2 * 12 + 2 * (8 - 2) * 12);
    // uv 必须在 [0,1]
    for (const sr::Vec2& uv : m.uvs) {
        SR_ASSERT(uv.x >= -1e-6f && uv.x <= 1.0f + 1e-6f);
        SR_ASSERT(uv.y >= -1e-6f && uv.y <= 1.0f + 1e-6f);
    }
}

SR_TEST(uv_sphere_poles_are_duplicated_so_uv_does_not_wrap_wrong) {
    // 极点若被共享，接缝处的 uv 会从 1 跳回 0，M4 的纹理会在接缝上拉出一条线。
    const sr::Mesh m = sr::makeUVSphere(1.0f, 4, 8);
    int poleVertices = 0;
    for (const sr::Vec3& p : m.positions) {
        if (std::fabs(p.y) > 0.999f) ++poleVertices;
    }
    SR_ASSERT(poleVertices >= 2 * 8);   // 每个经度一份
}

SR_TEST(torus_has_no_degenerate_triangles) {
    const sr::Mesh m = sr::makeTorus(1.0f, 0.35f, 24, 12);
    SR_ASSERT_EQ(degenerateTriangleCount(m), 0);
    SR_ASSERT(m.triangleCount() > 0);
}

SR_TEST(all_indices_are_in_range_and_count_is_a_multiple_of_three) {
    for (const sr::Mesh& m : {sr::makeCube(1.0f, true), sr::makeUVSphere(1.0f, 6, 8),
                              sr::makeTorus(1.0f, 0.3f, 12, 8)}) {
        SR_ASSERT_EQ(m.indices.size() % 3, 0u);
        for (const uint32_t idx : m.indices) SR_ASSERT(idx < m.positions.size());
        SR_ASSERT_EQ(m.positions.size(), m.normals.size());
        SR_ASSERT_EQ(m.positions.size(), m.uvs.size());
    }
}

SR_TEST(recompute_bounds_tracks_the_points) {
    sr::Mesh m = sr::makeCube(2.0f, true);
    for (sr::Vec3& p : m.positions) p = p + sr::Vec3{10, 0, 0};
    m.recomputeBounds();
    SR_ASSERT_NEAR(m.bounds.min.x, 9.0f, 1e-6f);
    SR_ASSERT_NEAR(m.bounds.max.x, 11.0f, 1e-6f);
    SR_ASSERT_NEAR(m.boundCenter().x, 10.0f, 1e-6f);
}
