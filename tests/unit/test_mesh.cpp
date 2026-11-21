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

SR_TEST(cube_face_uvs_follow_the_face_instead_of_folding_over_itself) {
    // 这条是 M4.7 接纹理时抓出来的真 bug：appendQuad 一度按**顶点下标的位模式**赋 uv
    // （`{i&1, i>>1}`），对环序的四角（左下、右下、右上、左上）给出
    // (0,0) (1,0) (0,1) (1,1) —— 按这个顺序连起来是个**蝴蝶结**（uv 多边形自交），
    // 于是每张贴图沿一条反对角线翻折回去。
    // 为什么到 M4.7 才发现：M2/M3 没有任何 shader 读 uv，而"M4 之前"唯一用 cube 贴图的
    // 地方就是这个任务。教训与 Ruling 36 同族：**没被消费过的属性不会自己报错**。
    //
    // 断言分三层，缺任一层都会放过一类错误：
    //  ① 四个 uv 恰好是单位正方形的四个角（"覆盖 [0,1]"的字面要求）
    //  ② 空间里互为对角的两个角，在 uv 里也必须互为对角 —— **蝴蝶结满足 ① 但不满足 ②**，
    //     所以只有 ① 的测试是假测试（第一版我就只写了 ①，看到它抓不住才补的 ②）
    //  ③ uv 多边形的有向面积绝对值 == 1（蝴蝶结的两叶互相抵消 ⇒ 面积为 0）
    const sr::Mesh m = sr::makeCube(2.0f, true);   // perFaceNormals ⇒ 每面独占 4 个顶点
    SR_ASSERT_EQ(m.positions.size(), 24u);
    if (m.positions.size() < 24u) return;          // 形状不对就到此为止，别越界索引（Ruling 26）

    for (int f = 0; f < 6; ++f) {
        const sr::Vec2 q0 = m.uvs[static_cast<size_t>(f) * 4u + 0u];
        const sr::Vec2 q1 = m.uvs[static_cast<size_t>(f) * 4u + 1u];
        const sr::Vec2 q2 = m.uvs[static_cast<size_t>(f) * 4u + 2u];
        const sr::Vec2 q3 = m.uvs[static_cast<size_t>(f) * 4u + 3u];
        const sr::Vec2 q[4] = {q0, q1, q2, q3};

        for (int k = 0; k < 4; ++k) {
            const auto onCorner = [](float v) { return std::fabs(v) < 1e-6f || std::fabs(v - 1.0f) < 1e-6f; };
            SR_ASSERT(onCorner(q[k].x));
            SR_ASSERT(onCorner(q[k].y));
        }
        // 四个角互不相同 ⇒ 才是"每面一张完整贴图"，而不是"两个角挤在同一纹素"
        for (int a = 0; a < 4; ++a) {
            for (int b = a + 1; b < 4; ++b) {
                const float d = std::sqrt((q[a].x - q[b].x) * (q[a].x - q[b].x) +
                                          (q[a].y - q[b].y) * (q[a].y - q[b].y));
                SR_ASSERT(d > 0.5f);
            }
        }
        // ② 对角对应：空间对角 (0,2) 与 (1,3) 在 uv 里也必须是 sqrt(2) 而不是 1
        for (int k = 0; k < 2; ++k) {
            const sr::Vec2& a = q[k];
            const sr::Vec2& b = q[(k + 2) % 4];
            const float diag = std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
            SR_ASSERT_NEAR(diag, std::sqrt(2.0f), 1e-6f);
        }
        // ③ 鞋带公式
        float twice = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const sr::Vec2& a = q[k];
            const sr::Vec2& b = q[(k + 1) % 4];
            twice += a.x * b.y - b.x * a.y;
        }
        SR_ASSERT_NEAR(std::fabs(twice) * 0.5f, 1.0f, 1e-6f);
    }
}

SR_TEST(cube_uv_follows_each_faces_own_up_and_right) {
    // 本项目的约定是**全栈 top-down**（C8 把视口翻成 top-down，图像存储也是 top-down，
    // 采样器 v=0 = 图像第 0 行 = 顶部）。所以 mesh 这边也必须让 v=0 落在每个面的**顶边**，
    // 否则"贴图与图像同向"这条规则要在两处各说一遍，而第二处永远是那个被忘的。
    // OBJ 的 vt 原点在左下（v=0 在底），那一次翻折由 M5 的载入器负责 —— 格式差异
    // 就该在格式的门口处理掉，不留进管线。
    //
    // 关键取舍：uv 必须来自**角在这一面局部坐标系里的角色**（上/右），不是来自
    // 它在 faces[] 表里的下标。理由就藏在这六个面的表里：+Z/-Z 的四个角是
    // "下、下、上、上"（沿底边起步），而 +X/-X 是"下、上、上、下"（沿左边起步）——
    // 两者都合法（都是绕序），但同一张按下标查的 uv 表在 ±X 上会让贴图的 u 轴
    // 指向世界的**上方**，也就是那两面被转了 90°。这个 90° 不是这次改出来的，
    // 它一直在，只是双色棋盘转 90° 看不出来，而带角标记的贴图看得出来。
    //
    // 顶面与底面没有 +Y 可用，所以"上"必须**选**一个并写下来：+Y 面朝 -Z，-Y 面朝 +Z。
    // 选哪个都行，但选完就要有断言盯着，否则哪天改的人会以为它还是随机的。
    const sr::Mesh m = sr::makeCube(2.0f, true);
    if (m.positions.size() < 24u) { SR_ASSERT_EQ(m.positions.size(), 24u); return; }

    for (int f = 0; f < 6; ++f) {
        const sr::Vec3 n = m.normals[static_cast<size_t>(f) * 4u];
        // 这一面的"上"：侧面用世界的 +Y；顶面用 -Z，底面用 +Z（上面刚选定的那条）。
        const sr::Vec3 up = (std::fabs(n.y) < 0.5f)
                                ? sr::Vec3{0.0f, 1.0f, 0.0f}
                                : (n.y > 0.0f ? sr::Vec3{0.0f, 0.0f, -1.0f}
                                              : sr::Vec3{0.0f, 0.0f, 1.0f});
        // 从外面看的"右" = 上 × 法线（右手系：+Y × +Z = +X，正对 +Z 面时右就是 +X ✓）
        const sr::Vec3 right = sr::normalize(sr::cross(up, n));
        sr::Vec3 center{0.0f, 0.0f, 0.0f};
        for (int k = 0; k < 4; ++k) center = center + m.positions[static_cast<size_t>(f) * 4u + k];
        center = center * 0.25f;

        for (int k = 0; k < 4; ++k) {
            const size_t i = static_cast<size_t>(f) * 4u + static_cast<size_t>(k);
            const sr::Vec3 d = m.positions[i] - center;
            // 恰好是 0 或 1，不是"接近"：这是生成器写死的两个常量，
            // 容差只会把"改了但没改对"放过去。
            const int wantV = sr::dot(d, up) > 0.0f ? 0 : 1;
            const int wantU = sr::dot(d, right) > 0.0f ? 1 : 0;
            SR_ASSERT_EQ(m.uvs[i].y, wantV);
            SR_ASSERT_EQ(m.uvs[i].x, wantU);
        }
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
