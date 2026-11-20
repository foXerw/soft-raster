#include "render/clipping.h"

#include <cmath>
#include <cstring>

#include "harness.h"
#include "math/vec.h"
#include "render/vertex.h"

namespace {

constexpr float kEps = 1e-6f;

sr::ClipVertex makeCV(float x, float y, float z, float w, float uOverW) {
    sr::ClipVertex cv;
    cv.clip = sr::Vec4{x, y, z, w};
    cv.vOverW.color = sr::Vec4{uOverW, 0, 0, 1};
    return cv;
}

// 裁剪结果的总不变式：视锥内的点必须 w > 0 且每个分量都在 ±w 之内。
// 这条比任何单点参考值都重要，因为它对任意输入都成立，不需要手算。
void expectInsideFrustum(const sr::ClipVertex* out, int count) {
    for (int i = 0; i < count; ++i) {
        const sr::Vec4& c = out[i].clip;
        SR_ASSERT(c.w > 0.0f);
        SR_ASSERT_NEAR(c.x, c.x, kEps);   // 保证读到了值（防止未初始化）
        SR_ASSERT(c.x >= -c.w - 1e-5f && c.x <= c.w + 1e-5f);
        SR_ASSERT(c.y >= -c.w - 1e-5f && c.y <= c.w + 1e-5f);
        SR_ASSERT(c.z >= -c.w - 1e-5f && c.z <= c.w + 1e-5f);
    }
}

// clip 空间 x-y 平面上的多边形面积（shoelace）。裁剪是在 clip 空间做的线性插值，
// 所以这个面积是有意义的检查量（它对应"与视锥的交"，不是投影后的面积）。
float polygonAreaXY(const sr::ClipVertex* poly, int count) {
    if (count < 3) return 0.0f;
    float sum = 0.0f;
    for (int i = 0; i < count; ++i) {
        const sr::Vec4& a = poly[i].clip;
        const sr::Vec4& b = poly[(i + 1) % count].clip;
        sum += a.x * b.y - b.x * a.y;
    }
    return std::fabs(sum) * 0.5f;
}

}  // namespace

SR_TEST(clip_keeps_a_fully_inside_triangle_unchanged) {
    const sr::ClipVertex in[3] = {makeCV(0, 0, -0.5f, 1, 1), makeCV(0.5f, 0.5f, -0.9f, 1, 2),
                                  makeCV(-0.5f, 0.2f, -0.2f, 1, 3)};
    sr::ClipVertex out[sr::kMaxClippedVerts];
    const int n = sr::clipPolygonToFrustum(in, out);
    SR_ASSERT_EQ(n, 3);
    expectInsideFrustum(out, n);
    for (int i = 0; i < 3; ++i) {
        SR_ASSERT_NEAR(out[i].clip.x, in[i].clip.x, 1e-6f);
        SR_ASSERT_NEAR(out[i].clip.y, in[i].clip.y, 1e-6f);
        SR_ASSERT_NEAR(out[i].vOverW.color.x, in[i].vOverW.color.x, 1e-6f);
    }
}

SR_TEST(clip_drops_a_triangle_fully_outside_on_posX) {
    const sr::ClipVertex in[3] = {makeCV(3, 0, 0, 1, 1), makeCV(4, 1, 0, 1, 2), makeCV(3, -1, 0, 1, 3)};
    sr::ClipVertex out[sr::kMaxClippedVerts];
    SR_ASSERT_EQ(sr::clipPolygonToFrustum(in, out), 0);
}

SR_TEST(clip_drops_a_triangle_behind_the_camera) {
    // w < 0 的整个三角形必须丢掉：透视除法在这里会把符号翻转，
    // 不丢的话"背后的东西"会出现在屏幕中间（症状是画面里出现不该有的镜像几何）。
    const sr::ClipVertex in[3] = {makeCV(0, 0, 0, -1, 1), makeCV(1, 1, 0, -2, 2), makeCV(-1, 1, 0, -3, 3)};
    sr::ClipVertex out[sr::kMaxClippedVerts];
    SR_ASSERT_EQ(sr::clipPolygonToFrustum(in, out), 0);
}

SR_TEST(clip_drops_a_degenerate_triangle_outside) {
    const sr::ClipVertex in[3] = {makeCV(9, 9, 0, 1, 1), makeCV(9, 9, 0, 1, 1), makeCV(9, 9, 0, 1, 1)};
    sr::ClipVertex out[sr::kMaxClippedVerts];
    SR_ASSERT_EQ(sr::clipPolygonToFrustum(in, out), 0);
}

SR_TEST(clip_across_posX_produces_the_hand_derived_result) {
    // 手工推导（实现之前算好，这是本任务唯一的精确参考用例）：
    //   平面是 x <= w，距离取 d = w - x（内部为正）。
    //   边 A→B：d_A = 1-1.5 = -0.5, d_B = 1-0 = 1 ⇒ t = d_A/(d_A-d_B) = 1/3
    //          ⇒ 交点 = A + t(B-A) = (1.0, 1/3)，uOverW = lerp(3, 0, 1/3) = 2
    //   边 C→A：d_C = 1, d_A = -0.5 ⇒ t = 1/1.5 = 2/3
    //          ⇒ 交点 = C + t(A-C) = (1.0, -1/3)，uOverW = lerp(0, 3, 2/3) = 2
    //   于是输出 4 个顶点；原三角形面积 3/2，裁剪后 4/3，比值 8/9。
    const sr::ClipVertex in[3] = {makeCV(1.5f, 0, 0, 1, 3), makeCV(0, 1, 0, 1, 0),
                                  makeCV(0, -1, 0, 1, 0)};
    sr::ClipVertex out[sr::kMaxClippedVerts];
    const int n = sr::clipPolygonToFrustum(in, out);
    SR_ASSERT_EQ(n, 4);
    expectInsideFrustum(out, n);

    // 顶点顺序由实现的多边形走向决定，所以这里用"多重集 + 相邻性"而不是硬索引：
    // 硬索引会让测试对无关的实现细节敏感。
    bool hasB = false, hasC = false, hasP = false, hasQ = false;
    for (int i = 0; i < n; ++i) {
        const sr::Vec4& c = out[i].clip;
        const bool isB = std::fabs(c.x - 0) < 1e-5f && std::fabs(c.y - 1) < 1e-5f;
        const bool isC = std::fabs(c.x - 0) < 1e-5f && std::fabs(c.y + 1) < 1e-5f;
        const bool isP = std::fabs(c.x - 1) < 1e-5f && std::fabs(c.y - 1.0f / 3.0f) < 1e-5f;
        const bool isQ = std::fabs(c.x - 1) < 1e-5f && std::fabs(c.y + 1.0f / 3.0f) < 1e-5f;
        hasB |= isB;
        hasC |= isC;
        hasP |= isP;
        hasQ |= isQ;
        // 两个交点必须正好贴在平面上（x == w）
        if (isP || isQ) SR_ASSERT_NEAR(c.x, c.w, 1e-6f);
        // 交点上的 uOverW 必须是 2 —— 这条证明 vOverW 与 clip 用的是同一个 t（Review Focus 2）
        if (isP || isQ) SR_ASSERT_NEAR(out[i].vOverW.color.x, 2.0f, 1e-5f);
    }
    SR_ASSERT(hasB && hasC && hasP && hasQ);

    const float area = polygonAreaXY(out, n);
    SR_ASSERT_NEAR(area, 4.0f / 3.0f, 1e-5f);
    SR_ASSERT_NEAR(area / 1.5f, 8.0f / 9.0f, 1e-5f);
}

SR_TEST(clip_is_idempotent) {
    // 把结果重组成三角形再裁一次，必须不变。这条比任何单点参考值都更能抓住
    // "漏裁了一个平面"——漏掉的那个平面在第二次裁剪时会让顶点数或面积变化。
    const sr::ClipVertex in[3] = {makeCV(1.5f, 0, 0, 1, 3), makeCV(0, 1, 0, 1, 0),
                                  makeCV(0, -1, 0, 1, 0)};
    sr::ClipVertex once[sr::kMaxClippedVerts];
    const int n1 = sr::clipPolygonToFrustum(in, once);
    SR_ASSERT(n1 >= 3);

    // 扇形重三角化后逐个再裁
    for (int t = 1; t + 1 < n1; ++t) {
        const sr::ClipVertex tri[3] = {once[0], once[t], once[t + 1]};
        sr::ClipVertex twice[sr::kMaxClippedVerts];
        const int n2 = sr::clipPolygonToFrustum(tri, twice);
        SR_ASSERT_EQ(n2, 3);
        expectInsideFrustum(twice, n2);
        // 面积必须与"从原多边形直接取这块扇形"一致
        SR_ASSERT_NEAR(polygonAreaXY(twice, n2), polygonAreaXY(tri, 3), 1e-5f);
    }
}

SR_TEST(clip_does_not_emit_duplicate_vertices_when_an_edge_lies_on_a_plane) {
    // 一条边正好落在 x == w 上：两个端点都"恰好在边界"，不能因此产生重复顶点。
    // 重复顶点的症状是后面出现零面积三角形（被 Setup 拒掉，白做工），
    // 更糟的是某些实现会让相邻两次裁剪互相"生产"退化三角形。
    const sr::ClipVertex in[3] = {makeCV(1, 0.5f, 0, 1, 1), makeCV(1, -0.5f, 0, 1, 2),
                                  makeCV(0, 0, 0, 1, 3)};
    sr::ClipVertex out[sr::kMaxClippedVerts];
    const int n = sr::clipPolygonToFrustum(in, out);
    SR_ASSERT(n >= 3 && n <= 4);
    expectInsideFrustum(out, n);
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            const float d = sr::length(sr::xyz(out[i].clip - out[j].clip));
            SR_ASSERT(d > 1e-6f);
        }
    }
}

SR_TEST(clip_of_a_near_plane_band_stays_within_the_invariant) {
    // 近平面把三角形切成窄带：参考值容易算错，所以这里只要求"顶点数在 [3,5] + 总不变式"。
    const sr::ClipVertex in[3] = {makeCV(0, 0, -2.0f, 2.0f, 1), makeCV(1, 0, -0.5f, 0.5f, 2),
                                  makeCV(-1, 0, -0.05f, 0.05f, 3)};
    sr::ClipVertex out[sr::kMaxClippedVerts];
    const int n = sr::clipPolygonToFrustum(in, out);
    SR_ASSERT(n >= 3 && n <= 5);
    expectInsideFrustum(out, n);
}

SR_TEST(clip_stays_within_the_documented_vertex_capital) {
    // 一个同时跨过多个平面的三角形是最坏情况。断言两件事：
    // 结果不超过头文件承诺的上界（9），且每个输出顶点都在视锥内。
    // 上界写小的后果是静默截断，症状是"模型某些角缺一块"—— 比崩溃更难查。
    const sr::ClipVertex in[3] = {makeCV(2, 2, 2, 1, 1), makeCV(-2, 2, -2, 1, 2), makeCV(0, -3, 0, 1, 3)};
    sr::ClipVertex out[sr::kMaxClippedVerts];
    const int n = sr::clipPolygonToFrustum(in, out);
    SR_ASSERT(n <= sr::kMaxClippedVerts);
    expectInsideFrustum(out, n);
}

SR_TEST(vertex_inside_frustum_agrees_with_the_clipper) {
    SR_ASSERT(sr::vertexInsideFrustum(sr::Vec4{0, 0, -0.5f, 1}));
    SR_ASSERT_FALSE(sr::vertexInsideFrustum(sr::Vec4{2, 0, 0, 1}));
    SR_ASSERT_FALSE(sr::vertexInsideFrustum(sr::Vec4{0, 0, 0, -1}));
    // 正好在边界上算"在内"（与 fill rule 一样，边界归属必须唯一）
    SR_ASSERT(sr::vertexInsideFrustum(sr::Vec4{1, 1, -1, 1}));
}
