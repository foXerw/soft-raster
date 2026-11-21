#include <cmath>
#include <cstdint>
#include <utility>

#include "harness.h"
#include "render/color.h"
#include "render/sampler.h"
#include "render/texture.h"
#include "test_util.h"

namespace {

// 2x2，四个纹素 32 / 64 / 128 / 255。
// 为什么**不能**用等差的 0/64/128/192：等差数列让"权重镜像"这类错误算出同一个数
// （0.25*0 + 0.75*128 == 0.75*64 + 0.25*192），那条断言就永远不可能红。
// 这不是理论担忧 —— 变异 D（把 fu 与 1-fu 对调）抓红了同一测试里的另外三条，唯独漏了 v 方向那条。
// 倍增序列下任意权重凸组合都不重合，任何 tap/权重错配都会改变结果。
uint8_t quad064(int x, int y) {
    static const uint8_t kVals[4] = {32, 64, 128, 255};
    return kVals[x + 2 * y];
}

float byteOf(int x, int y) { return static_cast<float>(quad064(x, y)) / 255.0f; }

sr::Texture linearTexture(sr::ImageRGBA8 img) {
    return sr::makeTexture(std::move(img), /*colorDataIsSrgb=*/false);
}

sr::Texture quad() { return linearTexture(makeGrayImage(2, 2, quad064)); }

// 4x1：值 1,2,3,4（byteValue 用的那套），专门测跨边界。
sr::Texture strip() { return linearTexture(makeGrayImage(4, 1, grayXPlus1)); }

}  // namespace

using sr::WrapMode;

SR_TEST(bilinear_at_a_texel_center_returns_that_texel_exactly) {
    const sr::Texture t = quad();
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / 2.0f;
            const float v = (static_cast<float>(y) + 0.5f) / 2.0f;
            const sr::TexelF s =
                sr::sampleBilinear(t, WrapMode::Repeat, WrapMode::Repeat, u, v);
            SR_ASSERT_NEAR(s.r, byteOf(x, y), 1e-6f);
            // 中心点上两种过滤必须给同一个答案：bilinear 在中心"退化"成 nearest，
            // 这条不成立就说明 off-by-half 挪了半个纹素（整张贴图偏移的根源）。
            const sr::TexelF n = sr::sampleNearest(t, WrapMode::Repeat, WrapMode::Repeat, u, v);
            SR_ASSERT_NEAR(s.r, n.r, 1e-6f);
        }
    }
}

SR_TEST(bilinear_weight_is_the_distance_from_the_lower_texel) {
    // u = 0.375 ⇒ x = u*2 - 0.5 = 0.25 ⇒ 下 tap 权重 0.75、上 tap 0.25。
    // 写反（fu ↔ 1-fu）会得到 0.25*32 + 0.75*64 = 56/255，而不是 0.75*32 + 0.25*64 = 40/255
    // —— 这条断言就是为了区分这两个数。
    const sr::Texture t = quad();
    const sr::TexelF s = sr::sampleBilinear(t, WrapMode::Repeat, WrapMode::Repeat, 0.375f, 0.25f);
    SR_ASSERT_NEAR(s.r, 0.75f * byteOf(0, 0) + 0.25f * byteOf(1, 0), 1e-6f);
    SR_ASSERT_NEAR(s.r, 40.0f / 255.0f, 1e-6f);
    // 另一个方向：纹素中心在 v = 0.25 与 0.75，所以 v=0.625 处的比例是
    // t = (0.625-0.25)/0.5 = 0.75 ⇒ 第二行占 3/4（第一版这里我把权重写反了，
    // 反推依据是中心的定义，不是"实现给了什么"）。
    const sr::TexelF d = sr::sampleBilinear(t, WrapMode::Repeat, WrapMode::Repeat, 0.25f, 0.625f);
    SR_ASSERT_NEAR(d.r, 0.25f * byteOf(0, 0) + 0.75f * byteOf(0, 1), 1e-6f);
    SR_ASSERT_NEAR(d.r, 104.0f / 255.0f, 1e-6f);   // 镜像权重会给 111.75/255（等差夹具下两者相同）
}

SR_TEST(bilinear_midway_between_centers_averages_the_two) {
    const sr::Texture t = quad();
    // 两个中心的中点：u=0.5、v=0.25 ⇒ (0,0) 与 (1,0) 各半 = (32+64)/2 = 48
    const sr::TexelF s = sr::sampleBilinear(t, WrapMode::Repeat, WrapMode::Repeat, 0.5f, 0.25f);
    SR_ASSERT_NEAR(s.r, 48.0f / 255.0f, 1e-6f);
    // 双方向的中点：u=v=0.5 ⇒ 四个各 1/4 = (32+64+128+255)/4 = 119.75
    const sr::TexelF c = sr::sampleBilinear(t, WrapMode::Repeat, WrapMode::Repeat, 0.5f, 0.5f);
    SR_ASSERT_NEAR(c.r, 119.75f / 255.0f, 1e-6f);
}

SR_TEST(bilinear_across_the_repeat_seam_blends_with_the_other_end) {
    const sr::Texture t = strip();   // 4x1，值 1,2,3,4
    // u = 0.9375 ⇒ x = 3.75 - 0.5 = 3.25 ⇒ 下 tap 是最后一个纹素，上 tap 越过右边界。
    // Repeat 必须绕回索引 0（值 1）；ClampEdge 必须钉在最后一个（值 4）。
    // 两者在**同一个 UV** 上给出不同答案，这才叫 wrap 真的参与了 taps 的选择。
    const float u = 0.9375f;
    const sr::TexelF rep = sr::sampleBilinear(t, WrapMode::Repeat, WrapMode::Repeat, u, 0.5f);
    SR_ASSERT_NEAR(rep.r, (0.75f * 4.0f + 0.25f * 1.0f) / 255.0f, 1e-6f);
    const sr::TexelF cl = sr::sampleBilinear(t, WrapMode::ClampEdge, WrapMode::ClampEdge, u, 0.5f);
    SR_ASSERT_NEAR(cl.r, 4.0f / 255.0f, 1e-6f);
    // u = 0 处：Repeat 要把左边界外的 tap 绕到最后一个纹素（对称的另一半）
    const sr::TexelF left = sr::sampleBilinear(t, WrapMode::Repeat, WrapMode::Repeat, 0.0f, 0.5f);
    SR_ASSERT_NEAR(left.r, (0.5f * 4.0f + 0.5f * 1.0f) / 255.0f, 1e-6f);
    const sr::TexelF leftCl =
        sr::sampleBilinear(t, WrapMode::ClampEdge, WrapMode::ClampEdge, 0.0f, 0.5f);
    SR_ASSERT_NEAR(leftCl.r, 1.0f / 255.0f, 1e-6f);
}

SR_TEST(bilinear_interpolates_in_linear_space_not_in_srgb) {
    // 左黑右灰（字节 0 与 128）的**颜色**纹理。正确答案是把两个字节都解码到线性空间再平均：
    //   (srgb8ToLinear(0) + srgb8ToLinear(128)) / 2 ≈ 0.1079
    // 在编码空间平均再解码会得到 (0 + 0.5020)/2 ≈ 0.2510 —— 差 0.14，肉眼就是"中间调发灰"。
    // 这条是 spec §7.3"线性空间着色"在采样环节的落地。
    sr::ImageRGBA8 img = makeGrayImage(2, 1, [](int x, int) -> uint8_t {
        return x == 0 ? 0 : 128;
    });
    const sr::Texture t = sr::makeTexture(std::move(img), /*colorDataIsSrgb=*/true);
    const sr::TexelF mid = sr::sampleBilinear(t, WrapMode::ClampEdge, WrapMode::ClampEdge, 0.5f,
                                              0.5f);
    const float correct = 0.5f * (sr::srgb8ToLinear(0) + sr::srgb8ToLinear(128));
    SR_ASSERT_NEAR(mid.r, correct, 1e-6f);
    SR_ASSERT(std::fabs(mid.r - 0.2510f) > 0.05f);   // 明确不是"编码空间平均"
}

SR_TEST(bilinear_interpolates_alpha_too) {
    // alpha 是覆盖率，插值时不能丢：丢了的后果是半透明纹理的边缘变成硬边。
    sr::ImageRGBA8 img = makeGrayImage(2, 1, grayConstant200);
    img.rgba[3] = 0x00;        // 左纹素全透明
    img.rgba[7] = 0x80;        // 右纹素半透明
    const sr::Texture t = linearTexture(std::move(img));
    const sr::TexelF mid = sr::sampleBilinear(t, WrapMode::Repeat, WrapMode::Repeat, 0.5f, 0.5f);
    SR_ASSERT_NEAR(mid.a, 0.5f * (0x80 / 255.0f), 1e-6f);
}

SR_TEST(bilinear_never_reads_outside_the_texture) {
    // 越界的 tap 一律由 wrapCoord 处理；这里用一组"边界外很远"的 UV 确认它不崩、不返回垃圾。
    const sr::Texture t = strip();
    const float pts[] = {-3.5f, -0.0001f, 0.0f, 1.0f, 1.0001f, 7.25f};
    for (const WrapMode w : {WrapMode::Repeat, WrapMode::ClampEdge}) {
        for (const float u : pts) {
            const sr::TexelF s = sr::sampleBilinear(t, w, w, u, u);
            SR_ASSERT(std::isfinite(s.r));
            SR_ASSERT(s.r >= 0.0f && s.r <= 1.0f);   // 插值不可能跑出原始值域
            SR_ASSERT(std::isfinite(s.a));
        }
    }
}
