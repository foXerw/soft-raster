#include "math/mat.h"

#include <cmath>
#include <cstdlib>
#include <initializer_list>

#include "harness.h"
#include "math/vec.h"

namespace {

// MSVC 只在定义了 _USE_MATH_DEFINES 时才给 M_PI，测试里不依赖那个宏。
constexpr float kPi = 3.14159265358979323846f;

// 16 个值按行主序填进矩阵。用 initializer_list 而不是手写 16 个下标：
// 后者数错行列时会把测试自己写错，而"测试写错"比"代码写错"更难发现。
sr::Mat4 makeMat4(std::initializer_list<float> values) {
    sr::Mat4 m{};
    int i = 0;
    for (const float v : values) {
        m.m[i / 4][i % 4] = v;
        ++i;
    }
    SR_ASSERT_EQ(i, 16);
    return m;
}

void expectNearMatrix(const sr::Mat4& a, const sr::Mat4& b, float tol) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) SR_ASSERT_NEAR(a.m[r][c], b.m[r][c], tol);
}

}  // namespace

SR_TEST(mat4_identity_times_identity_is_identity) {
    const sr::Mat4 i = sr::Mat4::identity();
    expectNearMatrix(i * i, i, 1e-6f);
    SR_ASSERT_NEAR(i.m[0][0], 1.0f, 1e-6f);
    SR_ASSERT_NEAR(i.m[1][1], 1.0f, 1e-6f);
    SR_ASSERT_NEAR(i.m[2][2], 1.0f, 1e-6f);
    SR_ASSERT_NEAR(i.m[3][3], 1.0f, 1e-6f);
    SR_ASSERT_NEAR(i.m[0][1], 0.0f, 1e-6f);
}

SR_TEST(mat4_product_matches_a_hand_computed_case) {
    // 行主序 + 列向量右乘：result[r][c] = sum_k a[r][k] * b[k][c]
    const sr::Mat4 a = makeMat4({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
    const sr::Mat4 b = makeMat4({1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
    expectNearMatrix(a * b, a, 1e-6f);       // 右乘单位阵不变
    expectNearMatrix(b * a, a, 1e-6f);       // 左乘也不变（单位阵是唯一两边都成立的）

    const sr::Mat4 c = makeMat4({1, 0, 0, 2, 0, 1, 0, 3, 0, 0, 1, 4, 0, 0, 0, 1});  // 平移
    const sr::Mat4 d = makeMat4({2, 0, 0, 0, 0, 3, 0, 0, 0, 0, 4, 0, 0, 0, 0, 1});  // 缩放
    // 先缩放再平移（c * d 作用在点上 = 先 d 后 c）：对角 2,3,4，平移列 2,3,4
    const sr::Mat4 cd = c * d;
    SR_ASSERT_NEAR(cd.m[0][0], 2.0f, 1e-6f);
    SR_ASSERT_NEAR(cd.m[1][1], 3.0f, 1e-6f);
    SR_ASSERT_NEAR(cd.m[2][2], 4.0f, 1e-6f);
    SR_ASSERT_NEAR(cd.m[0][3], 2.0f, 1e-6f);
    SR_ASSERT_NEAR(cd.m[3][3], 1.0f, 1e-6f);
    // 顺序反过来则平移被缩放：这条断言是"矩阵乘法不可交换"的守门人，
    // 写反了的症状是所有物体位置按缩放系数漂移。
    const sr::Mat4 dc = d * c;
    SR_ASSERT_NEAR(dc.m[0][3], 4.0f, 1e-6f);
    SR_ASSERT_NEAR(dc.m[1][3], 9.0f, 1e-6f);
    SR_ASSERT_NEAR(dc.m[2][3], 16.0f, 1e-6f);
}

SR_TEST(mat4_times_vector_uses_columns_correctly) {
    const sr::Mat4 a = makeMat4({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
    const sr::Vec4 e3 = a * sr::Vec4{0, 0, 0, 1};      // 基向量 ⇒ 第 4 列
    SR_ASSERT_NEAR(e3.x, 4.0f, 1e-6f);
    SR_ASSERT_NEAR(e3.y, 8.0f, 1e-6f);
    SR_ASSERT_NEAR(e3.z, 12.0f, 1e-6f);
    SR_ASSERT_NEAR(e3.w, 16.0f, 1e-6f);

    const sr::Vec4 e0 = a * sr::Vec4{1, 0, 0, 0};      // 第 1 列
    SR_ASSERT_NEAR(e0.x, 1.0f, 1e-6f);
    SR_ASSERT_NEAR(e0.y, 5.0f, 1e-6f);
    SR_ASSERT_NEAR(e0.z, 9.0f, 1e-6f);
    SR_ASSERT_NEAR(e0.w, 13.0f, 1e-6f);

    // 一般点：行与向量的点积
    const sr::Vec4 p = a * sr::Vec4{1, 1, 1, 1};
    SR_ASSERT_NEAR(p.x, 10.0f, 1e-6f);
    SR_ASSERT_NEAR(p.y, 26.0f, 1e-6f);
    SR_ASSERT_NEAR(p.z, 42.0f, 1e-6f);
    SR_ASSERT_NEAR(p.w, 58.0f, 1e-6f);
}

SR_TEST(mat4_translate_moves_positions_and_not_directions) {
    const sr::Mat4 t = sr::Mat4::translation({2, 3, 4});
    const sr::Vec4 pos = t * sr::Vec4{1, 1, 1, 1};
    SR_ASSERT_NEAR(pos.x, 3.0f, 1e-6f);
    SR_ASSERT_NEAR(pos.y, 4.0f, 1e-6f);
    SR_ASSERT_NEAR(pos.z, 5.0f, 1e-6f);
    const sr::Vec4 dir = t * sr::Vec4{1, 1, 1, 0};     // w=0 ⇒ 方向不受平移影响
    SR_ASSERT_NEAR(dir.x, 1.0f, 1e-6f);
    SR_ASSERT_NEAR(dir.z, 1.0f, 1e-6f);
}

SR_TEST(mat4_inverse_round_trips) {
    // 平移 + 非均匀缩放 + 一个旋转的复合，是最容易暴露余子式索引错误的组合。
    const sr::Mat4 a = sr::Mat4::translation({1.5f, -2.0f, 3.5f}) *
                       sr::Mat4::scale({2.0f, 0.5f, 4.0f}) *
                       sr::Mat4::rotationY(0.7f);
    sr::Mat4 inv;
    SR_ASSERT(sr::Mat4::tryInvert(a, inv));
    expectNearMatrix(a * inv, sr::Mat4::identity(), 1e-4f);
    expectNearMatrix(inv * a, sr::Mat4::identity(), 1e-4f);

    // 逆变换真的把点带回来
    const sr::Vec4 p{0.25f, 0.5f, -0.75f, 1.0f};
    const sr::Vec4 back = inv * (a * p);
    SR_ASSERT_NEAR(back.x, p.x, 1e-4f);
    SR_ASSERT_NEAR(back.y, p.y, 1e-4f);
    SR_ASSERT_NEAR(back.z, p.z, 1e-4f);
}

SR_TEST(mat4_tryInvert_rejects_singular_and_leaves_output_untouched) {
    // 约定（被这条测试钉死）：失败时 **不写 out**，返回 false。
    // 为什么要在意：如果失败时写进 NaN 或垃圾，调用方（法线矩阵、fit 相机）
    // 会静默地用坏矩阵继续画，症状是"某个模型偶尔变形"而不是"这次构建失败"。
    sr::Mat4 singular = makeMat4({1, 2, 3, 4, 2, 4, 6, 8, 0, 0, 1, 0, 0, 0, 0, 1});
    sr::Mat4 out = sr::Mat4::identity();
    SR_ASSERT_FALSE(sr::Mat4::tryInvert(singular, out));
    expectNearMatrix(out, sr::Mat4::identity(), 0.0f);

    // 整行为零 ⇒ 秩亏，同样必须被拒
    const sr::Mat4 zeroRow = makeMat4({0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1});
    sr::Mat4 out2 = sr::Mat4::identity();
    SR_ASSERT_FALSE(sr::Mat4::tryInvert(zeroRow, out2));
    expectNearMatrix(out2, sr::Mat4::identity(), 0.0f);

    // 大坐标端：毫米尺度下的压扁阵，det = 1.0（> 固定阈值 1e-8）但条件数 1e9，必须拒绝——
    // 这条是"eps 改成固定 1e-8"这个变异的杀手（mat.h:161-167 的注释就是这个实测）。
    const sr::Mat4 squashedMm = sr::Mat4::scale({1e3f, 1e3f, 1e-6f});
    sr::Mat4 outSq = sr::Mat4::identity();
    SR_ASSERT_FALSE(sr::Mat4::tryInvert(squashedMm, outSq));
    expectNearMatrix(outSq, sr::Mat4::identity(), 0.0f);

    // 小坐标端的**已知边界**（不是期望行为，是把边界钉住，见 mat.h 注释与账本 M7 残留）：
    // maxAbs 至少是 m[3][3] 的那个 1，所以阈值在小坐标端退化成固定 1e-8，
    // 悬崖就落在 s³ = 1e-8 即 s ≈ 2.154e-3。实测 s=3e-3 可逆、s=1e-3 被拒。
    // 谁将来改成按 3x3 块的量级算 maxAbs，这两条会红——那是有意的变更该付的价格。
    sr::Mat4 outOk = sr::Mat4::identity();
    SR_ASSERT(sr::Mat4::tryInvert(sr::Mat4::scale({3e-3f, 3e-3f, 3e-3f}), outOk));
    expectNearMatrix(sr::Mat4::scale({3e-3f, 3e-3f, 3e-3f}) * outOk, sr::Mat4::identity(), 1e-3f);
    sr::Mat4 outTiny = sr::Mat4::identity();
    SR_ASSERT_FALSE(sr::Mat4::tryInvert(sr::Mat4::scale({1e-3f, 1e-3f, 1e-3f}), outTiny));
    expectNearMatrix(outTiny, sr::Mat4::identity(), 0.0f);
}

SR_TEST(mat4_transposed_is_element_mirror) {
    const sr::Mat4 a = makeMat4({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
    const sr::Mat4 t = a.transposed();
    SR_ASSERT_NEAR(t.m[0][1], 5.0f, 1e-6f);
    SR_ASSERT_NEAR(t.m[1][0], 2.0f, 1e-6f);
    SR_ASSERT_NEAR(t.m[2][3], 15.0f, 1e-6f);
    SR_ASSERT_NEAR(t.m[3][2], 12.0f, 1e-6f);
    SR_ASSERT_NEAR(t.m[2][2], 11.0f, 1e-6f);
    // 转置两次回到原矩阵
    expectNearMatrix(t.transposed(), a, 1e-6f);
}

SR_TEST(mat4_upper3x3_drops_translation_but_keeps_rotation_scale) {
    const sr::Mat4 a = sr::Mat4::translation({7, 8, 9}) * sr::Mat4::scale({2, 3, 4});
    const sr::Mat4 u = a.upper3x3AsMat4();
    SR_ASSERT_NEAR(u.m[0][0], 2.0f, 1e-6f);
    SR_ASSERT_NEAR(u.m[1][1], 3.0f, 1e-6f);
    SR_ASSERT_NEAR(u.m[2][2], 4.0f, 1e-6f);
    SR_ASSERT_NEAR(u.m[0][3], 0.0f, 1e-6f);   // 平移列被丢掉
    SR_ASSERT_NEAR(u.m[3][0], 0.0f, 1e-6f);   // 最后一行只剩 w=1
    SR_ASSERT_NEAR(u.m[3][3], 1.0f, 1e-6f);
}

SR_TEST(mat4_scale_and_rotation_are_not_swapped) {
    const sr::Mat4 r = sr::Mat4::rotationZ(kPi / 2.0f);
    const sr::Vec4 x = r * sr::Vec4{1, 0, 0, 1};
    SR_ASSERT_NEAR(x.x, 0.0f, 1e-5f);
    SR_ASSERT_NEAR(x.y, 1.0f, 1e-5f);
    const sr::Vec4 y = r * sr::Vec4{0, 1, 0, 1};
    SR_ASSERT_NEAR(y.x, -1.0f, 1e-5f);
    SR_ASSERT_NEAR(y.y, 0.0f, 1e-5f);

    const sr::Mat4 s = sr::Mat4::scale({3, 1, 1});
    const sr::Vec4 sx = s * sr::Vec4{1, 0, 0, 1};
    SR_ASSERT_NEAR(sx.x, 3.0f, 1e-6f);
    // 方向（w=0）也要被缩放，但不应被平移：这是法线矩阵存在的理由之一。
    const sr::Vec4 dir = s * sr::Vec4{1, 0, 0, 0};
    SR_ASSERT_NEAR(dir.x, 3.0f, 1e-6f);
}
