#include <cmath>
#include <cstdint>
#include <utility>

#include "harness.h"
#include "math/vec.h"
#include "render/sampler.h"
#include "render/texture.h"
#include "test_util.h"

namespace {

using sr::WrapMode;

// 4x1：[0, 0, 255, 255]。它的三级链每一级都能算出确切数字，而且三级互不相同：
//   level 0 = 4x1 [0, 0, 255, 255]
//   level 1 = 2x1 [0, 255]
//   level 2 = 1x1 [128]      （(0+255+0+255)/4，边缘复制的那一级）
// u = 0.125（第 0 个纹素的中心）处三级的双线性值分别是 0、63.75、128 —— 三个不同的数，
// 于是"选错层级"和"根本没混合"这两种错误都能被分辨出来。
sr::Texture strip4() {
    sr::ImageRGBA8 img = makeGrayImage(4, 1, [](int x, int) -> uint8_t {
        return x < 2 ? uint8_t{0} : uint8_t{255};
    });
    return sr::makeTexture(std::move(img), /*colorDataIsSrgb=*/false);
}

// rho = |dU/dx| * W（其余导数为 0），所以梯度直接写成 rho/W。
sr::Vec2 gradUFor(float rho) { return sr::Vec2{rho / 4.0f, 0.0f}; }

constexpr float kSqrt2 = 1.4142135623730951f;

float red(const sr::TexelF& t) { return t.r; }

}  // namespace

SR_TEST(lod_level_and_fraction_split_the_log2_distance) {
    const sr::Texture t = strip4();
    const int n = t.levelCount();
    SR_ASSERT_EQ(n, 3);

    sr::Lod l0 = sr::lodFromGradients(4.0f, 1.0f, n, gradUFor(1.0f), sr::Vec2{0.0f, 0.0f});
    SR_ASSERT_EQ(l0.level, 0);
    SR_ASSERT_NEAR(l0.frac, 0.0f, 1e-5f);

    sr::Lod l1 = sr::lodFromGradients(4.0f, 1.0f, n, gradUFor(2.0f), sr::Vec2{0.0f, 0.0f});
    SR_ASSERT_EQ(l1.level, 1);
    SR_ASSERT_NEAR(l1.frac, 0.0f, 1e-5f);

    // 半级：log2(2*sqrt(2)) = 1.5 ⇒ 下一级占一半
    sr::Lod lh = sr::lodFromGradients(4.0f, 1.0f, n, gradUFor(2.0f * kSqrt2), sr::Vec2{0.0f, 0.0f});
    SR_ASSERT_EQ(lh.level, 1);
    SR_ASSERT_NEAR(lh.frac, 0.5f, 1e-4f);

    // 放大（每像素不到一个纹素）：level 0 且 frac 0 —— 绝不能是 {-1, ...} 或者往下混
    sr::Lod mag = sr::lodFromGradients(4.0f, 1.0f, n, gradUFor(0.25f), sr::Vec2{0.0f, 0.0f});
    SR_ASSERT_EQ(mag.level, 0);
    SR_ASSERT_NEAR(mag.frac, 0.0f, 1e-5f);
}

SR_TEST(lod_clamps_so_the_next_level_always_exists) {
    // 三线性要读 level 与 level+1，所以"夹到末级"必须**同时把 frac 归零**：
    // 只夹 level 的话 frac 还是 0.7，就会去读那并不存在的第 4 级（症状是崩溃或读到垃圾）。
    const sr::Texture t = strip4();
    const int n = t.levelCount();
    for (const float rho : {4.0f, 1000.0f, 1e20f}) {
        const sr::Lod l = sr::lodFromGradients(4.0f, 1.0f, n, gradUFor(rho), sr::Vec2{0.0f, 0.0f});
        SR_ASSERT_EQ(l.level, n - 1);
        SR_ASSERT_NEAR(l.frac, 0.0f, 1e-6f);
        SR_ASSERT(l.level + 1 < n || l.frac == 0.0f);   // 这个不变式是三线性不越界的全部依据
    }
}

SR_TEST(lod_from_gradients_maps_each_component_to_its_own_axis) {
    // Ruling B 的守卫：gradU 与 gradV 互换必须给出**不同**答案。
    // 128x32：W/H = 4，所以"30 纹素/像素"放在 u 上是 level 4，误当成 v 就变成 30/128*32 = 7.5 ⇒ level 2。
    const float W = 128.0f, H = 32.0f;
    const sr::Vec2 g{30.0f / W, 0.0f};
    const sr::Vec2 zero{0.0f, 0.0f};
    SR_ASSERT_EQ(sr::lodFromGradients(W, H, 8, g, zero).level, 4);
    SR_ASSERT_EQ(sr::lodFromGradients(W, H, 8, zero, g).level, 2);
    // 同时钉住它与标量版的一致性（两个入口必须走同一份 log2，不能各写一遍）
    const sr::Lod byVec = sr::lodFromGradients(W, H, 8, sr::Vec2{0.1f, 0.2f}, sr::Vec2{0.3f, 0.4f});
    const sr::Lod byScalar = sr::lodFromDerivatives(W, H, 8, 0.1f, 0.3f, 0.2f, 0.4f);
    SR_ASSERT_EQ(byVec.level, byScalar.level);
    SR_ASSERT_NEAR(byVec.frac, byScalar.frac, 1e-6f);
}

SR_TEST(trilinear_at_an_integer_level_is_bilinear_on_that_level) {
    const sr::Texture t = strip4();
    // rho = 2 ⇒ 正好 level 1、frac 0 ⇒ 应该等于"直接对 level 1 做双线性"
    const sr::TexelF tri = sr::sampleTrilinear(t, WrapMode::Repeat, WrapMode::Repeat, 0.125f, 0.5f,
                                               gradUFor(2.0f), sr::Vec2{0.0f, 0.0f});
    const sr::TexelF one = sr::sampleBilinearLevel(t, 1, WrapMode::Repeat, WrapMode::Repeat, 0.125f,
                                                   0.5f);
    SR_ASSERT_NEAR(red(tri), red(one), 1e-6f);
    SR_ASSERT_NEAR(red(tri), 63.75f / 255.0f, 1e-5f);   // 手算值，不是"实现给了什么"
}

SR_TEST(trilinear_halfway_between_levels_averages_them) {
    const sr::Texture t = strip4();
    const sr::TexelF tri = sr::sampleTrilinear(t, WrapMode::Repeat, WrapMode::Repeat, 0.125f, 0.5f,
                                               gradUFor(2.0f * kSqrt2), sr::Vec2{0.0f, 0.0f});
    const float l1 =
        red(sr::sampleBilinearLevel(t, 1, WrapMode::Repeat, WrapMode::Repeat, 0.125f, 0.5f));
    const float l2 =
        red(sr::sampleBilinearLevel(t, 2, WrapMode::Repeat, WrapMode::Repeat, 0.125f, 0.5f));
    SR_ASSERT_NEAR(l2, 128.0f / 255.0f, 1e-5f);       // 末级是那四个字节的全局平均
    SR_ASSERT_NEAR(red(tri), 0.5f * (l1 + l2), 1e-3f);
    // 区分力：只取一级的实现（没混合）会给出 l1，与正确答案差 (l2-l1)/2 ≈ 0.127
    SR_ASSERT(std::fabs(red(tri) - l1) > 0.05f);
}

SR_TEST(filter_dispatch_honours_the_mip_switch_and_the_filter_mode) {
    const sr::Texture t = strip4();
    sr::SamplerState s;
    s.wrapU = s.wrapV = WrapMode::Repeat;
    s.filter = sr::FilterMode::Trilinear;
    s.useMip = true;
    const sr::Vec2 big = gradUFor(64.0f);             // 足以选到末级
    const sr::Vec2 zero{0.0f, 0.0f};

    // useMip=false ⇒ 无论导数多大都停在 level 0（`1` 键的全部语义）
    s.useMip = false;
    const sr::TexelF noMip = sr::sample(t, s, 0.125f, 0.5f, big, zero);
    const sr::TexelF level0 = sr::sampleBilinear(t, WrapMode::Repeat, WrapMode::Repeat, 0.125f, 0.5f);
    SR_ASSERT_NEAR(red(noMip), red(level0), 1e-6f);
    SR_ASSERT_NEAR(red(noMip), 0.0f, 1e-6f);

    s.useMip = true;
    const sr::TexelF withMip = sr::sample(t, s, 0.125f, 0.5f, big, zero);
    SR_ASSERT_NEAR(red(withMip), 128.0f / 255.0f, 1e-5f);   // 末级，与 noMip 明显不同
    SR_ASSERT(std::fabs(red(withMip) - red(noMip)) > 0.2f);

    // Nearest 也要跟着 mip 走：它读的是**被选中那一级**的单个纹素，不是永远 level 0
    s.filter = sr::FilterMode::Nearest;
    const sr::TexelF nearTop = sr::sample(t, s, 0.125f, 0.5f, big, zero);
    SR_ASSERT_NEAR(red(nearTop), 128.0f / 255.0f, 1e-5f);
    // 而 level 0 上 nearest 与 bilinear 在纹素中心必须一致
    s.filter = sr::FilterMode::Nearest;
    const sr::TexelF near0 = sr::sample(t, s, 0.125f, 0.5f, zero, zero);
    s.filter = sr::FilterMode::Bilinear;
    const sr::TexelF bil0 = sr::sample(t, s, 0.125f, 0.5f, zero, zero);
    SR_ASSERT_NEAR(red(near0), red(bil0), 1e-6f);
    SR_ASSERT_NEAR(red(near0), 0.0f, 1e-6f);
}

SR_TEST(nearest_at_a_level_reads_that_level_not_the_base_image) {
    const sr::Texture t = strip4();
    // level 1 是 2x1 的 [0, 255]：u=0.125 在其上是"第 0 个纹素中心偏左"⇒ 混合到 255 那边
    const sr::TexelF n1 = sr::sampleNearestLevel(t, 1, WrapMode::Repeat, WrapMode::Repeat, 0.125f,
                                                 0.5f);
    SR_ASSERT_NEAR(red(n1), 0.0f, 1e-6f);              // floor(0.125*2)=0 ⇒ 第 0 个 = 0
    const sr::TexelF n2 = sr::sampleNearestLevel(t, 2, WrapMode::Repeat, WrapMode::Repeat, 0.125f,
                                                 0.5f);
    SR_ASSERT_NEAR(red(n2), 128.0f / 255.0f, 1e-6f);
    // 不存在的那一级必须返回全 0（有定义），而不是读越界
    const sr::TexelF bogus = sr::sampleNearestLevel(t, 9, WrapMode::Repeat, WrapMode::Repeat, 0.5f,
                                                    0.5f);
    SR_ASSERT_NEAR(red(bogus), 0.0f, 1e-6f);
}
