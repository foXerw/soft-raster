#include "render/sampler.h"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <utility>
#include <vector>

#include "harness.h"
#include "render/color.h"
#include "render/texture.h"
#include "test_util.h"

namespace {

// 4x1 的"每个纹素一个可区分的数"：值 = x + 1（+1 是为了让第 0 个纹素不是 0 ——
// 读回 0 既可能是"取到了纹理的第一个纹素"也可能是"什么都没读到"，那种歧义必须一开始就消掉）。
sr::Texture strip4() {
    return sr::makeTexture(makeGrayImage(4, 1, grayXPlus1), /*colorDataIsSrgb=*/false);
}

// 8x2：两个方向的坐标都用得上，才能看出 u 有没有错用高度当分母。
sr::Texture sheet8x2() {
    return sr::makeTexture(makeGrayImage(8, 2, grayXPlus1), /*colorDataIsSrgb=*/false);
}

float byteValue(int x, int y) {
    return static_cast<float>(grayXPlus1(x, y)) / 255.0f;
}

}  // namespace

using sr::WrapMode;

SR_TEST(nearest_at_a_texel_center_returns_that_texel) {
    const sr::Texture t = sheet8x2();
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 8; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / 8.0f;
            const float v = (static_cast<float>(y) + 0.5f) / 2.0f;
            const sr::TexelF s = sr::sampleNearest(t, WrapMode::Repeat, WrapMode::Repeat, u, v);
            // 每个纹素都要被点到：只测 (0,0) 的话，"索引恒为 0"这种实现也能通过。
            SR_ASSERT_NEAR(s.r, byteValue(x, y), 1e-6f);
            SR_ASSERT_NEAR(s.g, byteValue(x, y), 1e-6f);
            SR_ASSERT_NEAR(s.b, byteValue(x, y), 1e-6f);
            SR_ASSERT_NEAR(s.a, 1.0f, 1e-6f);
        }
    }
}

SR_TEST(texel_boundaries_sit_on_texel_edges_not_centers) {
    // 约定（这条测试就是它的全部意义）：floored 下标 = floor(u * W)，
    // 所以 u=0 是第 0 个纹素的**左边缘**，边界落在 k/W 上。
    // 另一种常见写法 round(u*W) 会把边界挪到纹素中心，两者在 0.25 处结论相反。
    const sr::Texture t = strip4();
    auto at = [&t](float u) {
        return sr::sampleNearest(t, WrapMode::Repeat, WrapMode::Repeat, u, 0.5f).r;
    };
    SR_ASSERT_NEAR(at(0.24f), byteValue(0, 0), 1e-6f);   // 0.96 ⇒ 第 0 个
    SR_ASSERT_NEAR(at(0.25f), byteValue(1, 0), 1e-6f);   // 正好在边界上 ⇒ 属于右边那个
    SR_ASSERT_NEAR(at(0.26f), byteValue(1, 0), 1e-6f);   // 1.04 ⇒ 第 1 个
    SR_ASSERT_NEAR(at(0.49f), byteValue(1, 0), 1e-6f);
    SR_ASSERT_NEAR(at(0.51f), byteValue(2, 0), 1e-6f);
    // 区分力就在 `at(0.24) == 第 0 个纹素` 那一条：round(u*W) 会给出 1，直接红。
    // （不写"期望失败"的断言 —— 一条测试里不该有需要失败的断言，那只会让下次
    //   真的坏了的时候没人看得出多了什么。）
}

SR_TEST(repeat_wraps_uv_outside_the_unit_interval) {
    const sr::Texture t = strip4();
    auto at = [&t](float u) {
        return sr::sampleNearest(t, WrapMode::Repeat, WrapMode::Repeat, u, 0.5f).r;
    };
    SR_ASSERT_NEAR(at(1.25f), at(0.25f), 1e-6f);
    SR_ASSERT_NEAR(at(2.0f), at(0.0f), 1e-6f);
    // 负数这一条是 C++ `%` 的坑：-1 % 4 在 C++ 里是 **-1** 而不是 3，
    // 直接拿来当索引会得到 texel(-1) ⇒ 越界。必须与 +0.75 落在同一个纹素上。
    SR_ASSERT_NEAR(at(-0.25f), at(0.75f), 1e-6f);
    SR_ASSERT_NEAR(at(-1.0f), at(0.0f), 1e-6f);
    SR_ASSERT_NEAR(at(-0.25f), byteValue(3, 0), 1e-6f);
    // 上面几个负数乘完 W 都是**整** -1.0/-4.0，向零截断与 floor 给出同一个数 ⇒ 抓不出这两种写法。
    // -0.1*4 = -0.4 才分得开：floor = -1 ⇒ 最后一个纹素，截断 = 0 ⇒ 第一个。
    SR_ASSERT_NEAR(at(-0.1f), byteValue(3, 0), 1e-6f);
    SR_ASSERT_NEAR(at(-0.1f), at(0.9f), 1e-6f);
}

SR_TEST(clamp_edge_pins_outside_uv_to_the_border_texel) {
    const sr::Texture t = strip4();
    auto at = [&t](float u) {
        return sr::sampleNearest(t, WrapMode::ClampEdge, WrapMode::ClampEdge, u, 0.5f).r;
    };
    SR_ASSERT_NEAR(at(-1.0f), byteValue(0, 0), 1e-6f);
    SR_ASSERT_NEAR(at(-0.01f), byteValue(0, 0), 1e-6f);
    SR_ASSERT_NEAR(at(2.0f), byteValue(3, 0), 1e-6f);
    SR_ASSERT_NEAR(at(0.999f), byteValue(3, 0), 1e-6f);
    // 与 Repeat 的**差别**也要断言：同一坐标两种 wrap 必须给出不同纹素，
    // 否则"ClampEdge 其实没生效"这种失败会被两边同时取到同一个值掩盖掉。
    const float repeated = sr::sampleNearest(t, WrapMode::Repeat, WrapMode::Repeat, -0.25f, 0.5f).r;
    const float clamped = sr::sampleNearest(t, WrapMode::ClampEdge, WrapMode::ClampEdge, -0.25f,
                                            0.5f).r;
    // 阈值要按**夹具的值域**校准，不是按"看起来够大"：TexelF 归一化到 [0,1]，
    // 这张 4x1 的相邻纹素差只有 1/255 ≈ 3.9e-3，写 0.1 会永远失败（第一版就是这么写的）。
    // 1e-3 仍然能区分"两种 wrap 撞回同一个纹素"（差为 0），而具体落在哪个纹素由上面四行钉。
    SR_ASSERT(std::fabs(repeated - clamped) > 1e-3f);
}

SR_TEST(u_uses_the_width_and_v_uses_the_height) {
    const sr::Texture t = sheet8x2();
    // (7,1) 的中心：u=7.5/8、v=1.5/2。如果 u 错用了高度 2 当乘数，floor(7.5/8*2)=1 ⇒ 取到 x=1。
    const sr::TexelF s = sr::sampleNearest(t, WrapMode::Repeat, WrapMode::Repeat, 7.5f / 8.0f,
                                           1.5f / 2.0f);
    SR_ASSERT_NEAR(s.r, byteValue(7, 1), 1e-6f);
    // 反过来确认第二维也走自己的尺寸：(1,0) 与 (1,1) 的值差 16/255，不会撞。
    const sr::TexelF low = sr::sampleNearest(t, WrapMode::Repeat, WrapMode::Repeat, 1.5f / 8.0f,
                                             0.25f);
    SR_ASSERT_NEAR(low.r, byteValue(1, 0), 1e-6f);
}

SR_TEST(color_texture_samples_are_decoded_to_linear) {
    // 位图里的字节是 sRGB 编码的（PNG/JPG/TGA 都如此）。FS 要在线性空间着色（spec §7.3），
    // 所以解码必须发生在采样时。
    sr::ImageRGBA8 img = makeGrayImage(1, 1, grayConstant200);
    img.rgba[3] = 0x80;   // alpha 故意不是 255：它要证明"alpha 不被 gamma 解码"
    const sr::Texture t = sr::makeTexture(std::move(img), /*colorDataIsSrgb=*/true);

    const sr::TexelF s = sr::sampleNearest(t, WrapMode::Repeat, WrapMode::Repeat, 0.5f, 0.5f);
    SR_ASSERT_NEAR(s.r, sr::srgb8ToLinear(200), 1e-6f);
    SR_ASSERT_NEAR(s.g, sr::srgb8ToLinear(200), 1e-6f);
    SR_ASSERT_NEAR(s.b, sr::srgb8ToLinear(200), 1e-6f);
    // 区分力：200/255 = 0.784，解码后 ≈ 0.580 —— 差 0.2，不会被容差吞掉。
    SR_ASSERT(std::fabs(s.r - 200.0f / 255.0f) > 0.1f);
    // alpha 是覆盖率而不是颜色，**不**做 gamma 解码（GPU 的 SRGB8_ALPHA8 也是这么处理的）。
    SR_ASSERT_NEAR(s.a, 0x80 / 255.0f, 1e-6f);
}

SR_TEST(data_texture_skips_the_srgb_decode) {
    // uv 调试图、法线贴图这类**数据**纹理不能解码：把 0.5 解码成 0.73 之后，
    // "uv 视图"就不再是 uv 视图了。这一条把 Texture::srgb 的 false 分支钉住。
    sr::ImageRGBA8 img = makeGrayImage(1, 1, grayConstant200);
    const sr::Texture t = sr::makeTexture(std::move(img), /*colorDataIsSrgb=*/false);
    const sr::TexelF s = sr::sampleNearest(t, WrapMode::Repeat, WrapMode::Repeat, 0.5f, 0.5f);
    SR_ASSERT_NEAR(s.r, 200.0f / 255.0f, 1e-6f);
    SR_ASSERT_NEAR(s.a, 1.0f, 1e-6f);
}

SR_TEST(make_texture_builds_the_full_mip_chain) {
    const sr::Texture t =
        sr::makeTexture(makeGrayImage(8, 4, grayConstant200), /*colorDataIsSrgb=*/false);
    SR_ASSERT_EQ(t.width, 8);
    SR_ASSERT_EQ(t.height, 4);
    SR_ASSERT_EQ(t.levelCount(), 4);            // 8x4 → 4x2 → 2x1 → 1x1（M4.1 的链规则）
    SR_ASSERT(t.texel(3, 0, 0) != nullptr);     // 链尾必须可读
    SR_ASSERT(t.texel(4, 0, 0) == nullptr);     // 不存在的那一级必须是 nullptr
}

SR_TEST(garbage_uv_yields_finite_values_and_no_oob_read) {
    // 热路径上 UV 可能来自退化三角形或 NaN 顶点：一个 NaN 纹素乘进 blend 会污染整块区域
    // （M3 的 interpClamped 是同一个道理）。这里不要求"对"，只要求**有定义且不越界**。
    const sr::Texture t = strip4();
    // 不用 0.0f/0.0f 写 NaN：MSVC 把常量除零当成**编译错误** C2124，
    // 于是这条"运行时健壮性"测试会以构建失败的形式出现（本项目里构建失败 = 跑的是旧二进制）。
    const float junk[] = {std::numeric_limits<float>::quiet_NaN(),
                          std::numeric_limits<float>::infinity(),
                          -std::numeric_limits<float>::infinity(), 1e30f, -1e30f};
    for (const float u : junk) {
        for (const WrapMode w : {WrapMode::Repeat, WrapMode::ClampEdge}) {
            const sr::TexelF s = sr::sampleNearest(t, w, w, u, u);
            SR_ASSERT(std::isfinite(s.r));
            SR_ASSERT(std::isfinite(s.g));
            SR_ASSERT(std::isfinite(s.b));
            SR_ASSERT(std::isfinite(s.a));
        }
    }
}

SR_TEST(wrap_coord_handles_negative_and_overflowing_indices_directly) {
    // wrapCoord 是 bilinear 也要复用的那一半，所以单独钉：不等 uv 换算参与进来。
    SR_ASSERT_EQ(sr::wrapCoord(0, 4, WrapMode::Repeat), 0);
    SR_ASSERT_EQ(sr::wrapCoord(4, 4, WrapMode::Repeat), 0);
    SR_ASSERT_EQ(sr::wrapCoord(-1, 4, WrapMode::Repeat), 3);
    SR_ASSERT_EQ(sr::wrapCoord(-5, 4, WrapMode::Repeat), 3);
    SR_ASSERT_EQ(sr::wrapCoord(9, 4, WrapMode::Repeat), 1);
    SR_ASSERT_EQ(sr::wrapCoord(-1, 4, WrapMode::ClampEdge), 0);
    SR_ASSERT_EQ(sr::wrapCoord(9, 4, WrapMode::ClampEdge), 3);
    SR_ASSERT_EQ(sr::wrapCoord(2, 4, WrapMode::ClampEdge), 2);
}
