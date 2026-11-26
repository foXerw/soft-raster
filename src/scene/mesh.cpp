#include "scene/mesh.h"

#include <cmath>
#include <cstdint>

namespace sr {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// 所有生成器都遵守同一条绕向约定：**从物体外面看是逆时针**（右手系，NDC 约定）。
// 视口的 y 翻转（C8）之后，它在 top-down 屏幕空间里变成顺时针 ——
// Task 2.4 的背面剔除正是按"屏幕空间 CW 为正面"写的，两处必须一致。
//
// 面的顺序固定为 +X, -X, +Y, -Y, +Z, -Z，且每面两个三角形。
// 这个顺序不是审美选择：测试断言"一般姿态下恰好剔掉 6 个三角形"，改了顺序那些数字就得重算。
// 注意正对姿态是 **10 个背对 + 0 个退化**（eye=(0,0,5) 时 4 个侧面的 n·(eye-a) = -1，
// 不是棱对；实测在 test_pipeline_integration.cpp 的 axis_aligned_cube_… 里）。
// "侧面退化成线段、8 个退化"只在正交投影下成立 —— 见 docs/notes/M2-几何阶段.md 那条撤回。
struct QuadFace {
    Vec3 v[4];
    Vec3 normal;
};

// 面色必须两两可区分：M2 的 painter 缺陷测试靠"中心像素是哪个面的颜色"来判定，
// 单色立方体在那个姿态下看起来完全正常（凸体 + 剔除已经解决了遮挡），缺陷是看不见的。
constexpr Vec4 kFaceColors[6] = {
    {1.00f, 0.20f, 0.20f, 1.0f},   // +X
    {0.20f, 0.95f, 0.25f, 1.0f},   // -X
    {0.25f, 0.35f, 1.00f, 1.0f},   // +Y
    {1.00f, 0.95f, 0.20f, 1.0f},   // -Y
    {0.95f, 0.25f, 1.00f, 1.0f},   // +Z
    {0.20f, 0.90f, 0.95f, 1.0f},   // -Z
};

void appendQuad(Mesh& m, const QuadFace& f, bool perFaceNormals, const Vec4& faceColor) {
    // uv 按**角在四边形里的角色**赋，不是按下标的位模式。
    // 之前写的是 `{i&1, i>>1}`，对环序四角给出 (0,0)(1,0)(0,1)(1,1) ——
    // 连起来是个蝴蝶结（uv 多边形自交、有向面积为 0），于是每张贴图沿一条反对角线
    // 翻折回去。M2/M3 看不出来，因为没有任何 shader 读 cube 的 uv；M4.7 一接纹理就暴露，
    // 症状还是最难归因的那种："贴图看起来是贴上了，只是哪里怪怪的"。
    // 由 test_mesh 的 cube_face_uvs_follow_the_face_instead_of_folding_over_itself 钉住。
    //
    // uv 由**角在这一面局部坐标系里的角色**决定，不是按它在 faces[] 表里的下标查表。
    //
    // 为什么必须这样：六个面的角序都是绕序的，但起点不同 —— +Z/-Z 从底边的左角起步
    // （下、下、上、上），+X/-X 从左边起步（下、上、上、下）。同一张按下标查的 uv 表
    // 在 ±X 上会让贴图的 u 轴指向世界的上方，也就是那两面被转了 90°；双色棋盘转 90°
    // 看不出来，带角标记的贴图看得出来（这个 90° 从 M2 就一直在）。
    //
    // 于是全栈只有一条方向规则：图像第 0 行 = 顶部、采样器 v=0 = 第 0 行、
    // mesh 的 v=0 = 这一面的顶边 ⇒ **贴图与图像同向**。
    // OBJ 的 vt 原点在左下，那一次 `v = 1 - v` 由 M5 的载入器负责（格式的差异在格式的
    // 门口处理掉，不留进管线）。
    // 守门：test_mesh 的 cube_uv_follows_each_faces_own_up_and_right
    // 与 test_golden_scene_textured 的 the_texture_reads_right_side_up_on_screen。
    //
    // 顶面/底面没有 +Y 可当"上"，所以这里**选**一个并写下来：+Y 面朝 -Z、-Y 面朝 +Z。
    const bool side = std::fabs(f.normal.y) < 0.5f;
    const Vec3 up = side ? Vec3{0.0f, 1.0f, 0.0f}
                         : (f.normal.y > 0.0f ? Vec3{0.0f, 0.0f, -1.0f} : Vec3{0.0f, 0.0f, 1.0f});
    // 从外面看的"右" = 上 × 法线（右手系：+Y × +Z = +X，正对 +Z 面时右就是 +X）。
    const Vec3 right = normalize(cross(up, f.normal));
    const Vec3 center{(f.v[0].x + f.v[1].x + f.v[2].x + f.v[3].x) * 0.25f,
                      (f.v[0].y + f.v[1].y + f.v[2].y + f.v[3].y) * 0.25f,
                      (f.v[0].z + f.v[1].z + f.v[2].z + f.v[3].z) * 0.25f};

    uint32_t q[4];
    for (int i = 0; i < 4; ++i) {
        // 六个面都是轴对齐的正方形，所以 d 在 right/up 上的投影非零、符号就是角色。
        // （若哪天用这个函数塞任意四边形，这里要换成重心坐标而不是取符号。）
        const Vec3 d = f.v[i] - center;
        const Vec2 uv{dot(d, right) > 0.0f ? 1.0f : 0.0f, dot(d, up) > 0.0f ? 0.0f : 1.0f};
        uint32_t found = 0xFFFFFFFFu;
        if (!perFaceNormals) {
            // 共享顶点：法线取归一化位置（立方体角上是三个面的平均方向）。
            // 代价是 uv 不可能同时对 6 个面正确，所以这条路径只用于不需要纹理的调试几何体。
            for (size_t k = 0; k < m.positions.size(); ++k) {
                const Vec3& p = m.positions[k];
                if (std::fabs(p.x - f.v[i].x) < 1e-6f && std::fabs(p.y - f.v[i].y) < 1e-6f &&
                    std::fabs(p.z - f.v[i].z) < 1e-6f) {
                    found = static_cast<uint32_t>(k);
                    break;
                }
            }
        }
        if (found != 0xFFFFFFFFu) {
            q[i] = found;
        } else {
            q[i] = static_cast<uint32_t>(m.positions.size());
            m.positions.push_back(f.v[i]);
            m.normals.push_back(perFaceNormals ? f.normal : normalize(f.v[i]));
            m.uvs.push_back(uv);
            m.colors.push_back(faceColor);
        }
    }
    m.indices.insert(m.indices.end(), {q[0], q[1], q[2], q[0], q[2], q[3]});
}

}  // namespace

void Mesh::recomputeBounds() { bounds = BBox::fromPoints(positions); }

Mesh makeCube(float size, bool perFaceNormals) {
    const float h = size * 0.5f;
    Mesh m;
    const QuadFace faces[6] = {
        {{ {h, -h, -h}, {h, h, -h}, {h, h, h}, {h, -h, h} }, {1, 0, 0}},    // +X
        {{ {-h, -h, h}, {-h, h, h}, {-h, h, -h}, {-h, -h, -h} }, {-1, 0, 0}},  // -X
        {{ {-h, h, -h}, {-h, h, h}, {h, h, h}, {h, h, -h} }, {0, 1, 0}},    // +Y
        {{ {-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {-h, -h, h} }, {0, -1, 0}},  // -Y
        {{ {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h} }, {0, 0, 1}},    // +Z
        {{ {h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h} }, {0, 0, -1}},  // -Z
    };
    for (int i = 0; i < 6; ++i) appendQuad(m, faces[i], perFaceNormals, kFaceColors[i]);
    m.recomputeBounds();
    return m;
}

Mesh makeUVSphere(float radius, int stacks, int slices) {
    Mesh m;
    if (stacks < 2) stacks = 2;
    if (slices < 3) slices = 3;

    const int vertsPerRing = slices + 1;   // 极点每经度一份（duplicatedPoleVerts 曾在这里两个分支同值，是死参数）
    for (int i = 0; i <= stacks; ++i) {
        const float phi = kPi * static_cast<float>(i) / static_cast<float>(stacks);
        const float sy = std::cos(phi);
        const float sr = std::sin(phi);
        for (int j = 0; j <= slices; ++j) {
            const float theta = 2.0f * kPi * static_cast<float>(j) / static_cast<float>(slices);
            const Vec3 dir{sr * std::cos(theta), sy, sr * std::sin(theta)};
            m.positions.push_back(dir * radius);
            m.normals.push_back(dir);
            m.uvs.push_back(Vec2{static_cast<float>(j) / static_cast<float>(slices),
                                 static_cast<float>(i) / static_cast<float>(stacks)});
            m.colors.push_back(Vec4{1, 1, 1, 1});
        }
    }

    const auto idx = [vertsPerRing](int i, int j) -> uint32_t {
        return static_cast<uint32_t>(i * vertsPerRing + j);
    };
    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < slices; ++j) {
            const uint32_t a = idx(i, j), b = idx(i, j + 1), c = idx(i + 1, j + 1),
                           d = idx(i + 1, j);
            if (i == 0) {
                m.indices.insert(m.indices.end(), {a, b, c});          // 顶极扇形
            } else if (i == stacks - 1) {
                m.indices.insert(m.indices.end(), {a, c, d});          // 底极扇形
            } else {
                m.indices.insert(m.indices.end(), {a, b, c, a, c, d});
            }
        }
    }
    m.recomputeBounds();
    return m;
}

Mesh makeTorus(float majorRadius, float minorRadius, int ringSegments, int sideSegments) {
    Mesh m;
    if (ringSegments < 3) ringSegments = 3;
    if (sideSegments < 3) sideSegments = 3;

    for (int i = 0; i <= ringSegments; ++i) {
        const float theta = 2.0f * kPi * static_cast<float>(i) / static_cast<float>(ringSegments);
        const float ct = std::cos(theta), st = std::sin(theta);
        for (int j = 0; j <= sideSegments; ++j) {
            const float phi = 2.0f * kPi * static_cast<float>(j) / static_cast<float>(sideSegments);
            const float cp = std::cos(phi), sp = std::sin(phi);
            m.positions.push_back(Vec3{(majorRadius + minorRadius * cp) * ct, minorRadius * sp,
                                       (majorRadius + minorRadius * cp) * st});
            m.normals.push_back(normalize(Vec3{cp * ct, sp, cp * st}));
            m.uvs.push_back(Vec2{static_cast<float>(i) / static_cast<float>(ringSegments),
                                 static_cast<float>(j) / static_cast<float>(sideSegments)});
            m.colors.push_back(Vec4{1, 1, 1, 1});
        }
    }
    const int stride = sideSegments + 1;
    const auto idx = [stride](int i, int j) -> uint32_t {
        return static_cast<uint32_t>(i * stride + j);
    };
    for (int i = 0; i < ringSegments; ++i) {
        for (int j = 0; j < sideSegments; ++j) {
            const uint32_t a = idx(i, j), b = idx(i, j + 1), c = idx(i + 1, j + 1),
                           d = idx(i + 1, j);
            m.indices.insert(m.indices.end(), {a, b, c, a, c, d});
        }
    }
    m.recomputeBounds();
    return m;
}

void applyPositionColors(Mesh& m) {
    if (m.bounds.empty()) m.recomputeBounds();
    const Vec3 c0 = m.bounds.center();
    const float r = m.bounds.radius() > 1e-6f ? m.bounds.radius() : 1.0f;
    for (size_t i = 0; i < m.positions.size(); ++i) {
        const Vec3 d = (m.positions[i] - c0) / r;
        m.colors[i] = Vec4{0.5f + 0.5f * d.x, 0.5f + 0.5f * d.y, 0.5f + 0.5f * d.z, 1.0f};
    }
}

}  // namespace sr
