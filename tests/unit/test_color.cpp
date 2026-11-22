#include "render/color.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>  // std::abs(int)

#include "harness.h"

namespace {

// IEC 61966-2-1 的解析式，作为 LUT 的独立参照。刻意写在测试里而不是生产代码里：
// 生产 API 只有一条路径，参照值由测试自己算，这样 LUT 填错时测试仍然可信。
double analyticSrgb(double v) {
    if (v <= 0.0) return 0.0;
    if (v >= 1.0) return 1.0;
    return v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

}  // namespace

SR_TEST(srgb_encode_endpoints_and_midpoint) {
    SR_ASSERT_EQ(sr::linearToSrgb8(0.0f), 0);
    SR_ASSERT_EQ(sr::linearToSrgb8(1.0f), 255);
    // 0.5 线性 → 0.7353 sRGB → 187.5。Review Focus 5：写反 gamma 的两种症状都在这里现形。
    SR_ASSERT(sr::linearToSrgb8(0.5f) >= 186 && sr::linearToSrgb8(0.5f) <= 188);
}

SR_TEST(srgb_encode_stays_within_one_lsb_of_the_iec_analytic_curve) {
    // 生产实现是 12bit 输入域的 LUT（为了每像素三次 pow 不成为瓶颈），所以这里断言的
    // 是"逼近程度"，不是"逐位相等"。允许 1 LSB：8 个通道的 LUT 索引舍入 + 生产用 float
    // 而参照用 double 的 pow，两处都可能把落在 .5 边界的值推到隔壁一格。
    // 真正要求逐位一致的是 gamma *语义*（端点、单调性、往返），由其它三条测试守住。
    int worst = 0;
    for (int i = 0; i <= 255; ++i) {
        const double v = i / 255.0;
        const int expected = static_cast<int>(std::lround(analyticSrgb(v) * 255.0));
        const int delta = std::abs(static_cast<int>(sr::linearToSrgb8(static_cast<float>(v))) - expected);
        if (delta > worst) worst = delta;
    }
    SR_ASSERT(worst <= 1);
}

SR_TEST(srgb_encode_is_monotonic) {
    uint8_t prev = 0;
    for (int i = 0; i <= 255; ++i) {
        const uint8_t e = sr::linearToSrgb8(static_cast<float>(i) / 255.0f);
        SR_ASSERT(e >= prev);
        prev = e;
    }
}

SR_TEST(srgb_roundtrip_is_identity_within_one_lsb) {
    for (int i = 0; i <= 255; ++i) {
        SR_ASSERT_EQ(sr::linearToSrgb8(sr::srgb8ToLinear(static_cast<uint8_t>(i))),
                     static_cast<uint8_t>(i));
    }
}

SR_TEST(srgb_encode_clamps_out_of_range_input) {
    SR_ASSERT_EQ(sr::linearToSrgb8(-3.5f), 0);
    SR_ASSERT_EQ(sr::linearToSrgb8(17.0f), 255);
    // NaN 必须落在一个合法字节上。热路径里 NaN 会变成 NaN 深度再顺着插值污染整块区域，
    // 所以"未定义输入也有定义输出"是接口的一部分：这里定死为 0。
    SR_ASSERT_EQ(sr::linearToSrgb8(std::nanf("")), 0);
}

SR_TEST(encodeSRGB_packs_as_0AARRGGBB) {
    // 值的格式是 0xAARRGGBB（因为内存字节序是 B,G,R,A + 小端）。
    // 纯红 = A:FF R:FF G:00 B:00 → 0xFFFF0000；纯蓝 = A:FF R:00 G:00 B:FF → 0xFF0000FF。
    // 计划原文把这两条写反了（红蓝互换），由本任务发现并更正，见 ledger Ruling 7。
    SR_ASSERT_EQ(sr::encodeSRGB({1.0f, 0.0f, 0.0f}, 1.0f), 0xFFFF0000u);  // 纯红
    SR_ASSERT_EQ(sr::encodeSRGB({0.0f, 1.0f, 0.0f}, 1.0f), 0xFF00FF00u);  // 纯绿
    SR_ASSERT_EQ(sr::encodeSRGB({0.0f, 0.0f, 1.0f}, 1.0f), 0xFF0000FFu);  // 纯蓝
    SR_ASSERT_EQ(sr::packBGRA(0x11, 0x22, 0x33, 0xFF), 0xFF112233u);
}

SR_TEST(packBGRA_memory_bytes_are_B_G_R_A) {
    // 名字说的是**内存字节序**，值说的是 little-endian 读出来的数。这两件事很容易混，
    // 而 DIB 要求的恰好是字节序：写反的症状是红蓝互换。
    const uint32_t packed = sr::packBGRA(0x11, 0x22, 0x33, 0xFF);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&packed);
    SR_ASSERT_EQ(bytes[0], 0x33);  // B
    SR_ASSERT_EQ(bytes[1], 0x22);  // G
    SR_ASSERT_EQ(bytes[2], 0x11);  // R
    SR_ASSERT_EQ(bytes[3], 0xFF);  // A
}

SR_TEST(decodeSRGB_inverts_encodeSRGB) {
    // 往返经过一次 8bit 量化，所以不可能要求高精度。误差上界的推导值得留在这里：
    // 字节量化误差最大 0.5 LSB，线性域里的放大系数是 d(linear)/d(srgb) ≈ 2.4·s^1.4/1.055，
    // 在最亮端（s→1）约为 2.28 ⇒ 最坏线性误差 ≈ 0.5 · 2.28/255 ≈ 0.0045。
    // 因此容差取 2/255 ≈ 0.0078：比理论最坏值宽，但任何一次"多编码一遍 gamma"都会超出它。
    const float tol = 2.0f / 255.0f;
    const float probes[] = {0.0f, 0.002f, 0.05f, 0.2f, 0.5f, 0.7f, 0.9f, 0.95f, 0.99f, 1.0f};
    for (const float v : probes) {
        const sr::LinearColor c{v, v, v};
        const sr::LinearColor back = sr::decodeSRGB(sr::encodeSRGB(c, 1.0f));
        SR_ASSERT_NEAR(back.r, v, tol);
    }
}

SR_TEST(srgb_lut_error_stays_below_one_lsb_of_the_analytic_curve) {
    // 这条测试是"为什么 LUT 必须有 4096 项而不是 256 项"的理由：输入域再细分一档，
    // 输出误差就锁在 1 LSB 以内。若把 LUT 缩到 256 项，暗部会出现 2~3 LSB 的偏差，
    // 平滑渐变上变成可见的带状条纹。
    int worstDelta = 0;
    for (int i = 0; i <= 4096; ++i) {
        const double v = i / 4096.0;
        const int analyticByte = static_cast<int>(std::lround(analyticSrgb(v) * 255.0));
        const int delta = std::abs(static_cast<int>(sr::linearToSrgb8(static_cast<float>(v))) - analyticByte);
        if (delta > worstDelta) worstDelta = delta;
    }
    SR_ASSERT(worstDelta <= 1);
}

SR_TEST(encodeSRGB_uses_alpha_verbatim) {
    SR_ASSERT_EQ(sr::encodeSRGB({0.0f, 0.0f, 0.0f}, 0.5f) >> 24, 0x80u);
    SR_ASSERT_EQ(sr::encodeSRGB({0.0f, 0.0f, 0.0f}, 0.0f) >> 24, 0x00u);
}

// ---- 浮点 sRGB→linear（M5.1）：MTL 的 Kd 本来就是浮点，不能先量化成字节 ----
// 注意 0.0 与 1.0 是这条曲线的不动点：只测这两端的测试对"直接 return v"这种错误
// 永远绿（变异①实测过），所以中点 0.5 与单调性才是真正有鉴别力的断言。

SR_TEST(srgbToLinear_float_endpoints_are_exact) {
    // 两端必须**精确**（容差 0）：Kd 1 1 1 解成非 1 会让"无 tint"这件事不再成立，
    // 而 golden 是零容差比对，整体偏一档会被当成光照问题去查。
    SR_ASSERT_NEAR(sr::srgbToLinear(0.0f), 0.0f, 0.0f);
    SR_ASSERT_NEAR(sr::srgbToLinear(1.0f), 1.0f, 0.0f);
}

SR_TEST(srgbToLinear_float_midpoint_matches_iec_curve) {
    // 0.5 是 display-referred 的中间灰，IEC 解出来 ≈ 0.21404（不是 0.5，也不是 0.25）。
    // 走字节版会先把 0.5 量化成 128/255≈0.50196 再解码，白白吃掉一档 —— 这条钉住浮点路径。
    SR_ASSERT_NEAR(sr::srgbToLinear(0.5f), 0.21404f, 1e-4f);
}

SR_TEST(srgbToLinear_float_clamps_out_of_range_and_nan) {
    SR_ASSERT_NEAR(sr::srgbToLinear(1.5f), 1.0f, 1e-5f);
    SR_ASSERT_NEAR(sr::srgbToLinear(-0.3f), 0.0f, 1e-5f);
    // NaN 必须有定义输出（与 linearToSrgb8 同一条理由）：否则顺着 tint 污染整块像素。
    SR_ASSERT_NEAR(sr::srgbToLinear(std::nanf("")), 0.0f, 0.0f);
}

SR_TEST(srgbToLinear_float_is_monotonic) {
    float prev = -1.0f;
    for (int i = 0; i <= 32; ++i) {  // 33 个采样点，逐点不减
        const float v = sr::srgbToLinear(static_cast<float>(i) / 32.0f);
        SR_ASSERT(v >= prev);
        prev = v;
    }
}

SR_TEST(srgbToLinear_color_decodes_each_channel) {
    const sr::LinearColor c = sr::srgbToLinear(sr::LinearColor{0.0f, 0.5f, 1.0f});
    SR_ASSERT_NEAR(c.r, 0.0f, 0.0f);
    SR_ASSERT_NEAR(c.g, 0.21404f, 1e-4f);
    SR_ASSERT_NEAR(c.b, 1.0f, 0.0f);
}
