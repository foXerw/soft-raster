#include "render/texture.h"

#include <cmath>

#include "harness.h"

namespace {

sr::ImageRGBA8 makeImage(int w, int h, uint8_t (*fn)(int x, int y)) {
    sr::ImageRGBA8 img;
    img.width = w;
    img.height = h;
    img.rgba.assign(static_cast<size_t>(w) * h * 4u, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t v = fn(x, y);
            const size_t at = (static_cast<size_t>(y) * w + x) * 4u;
            img.rgba[at] = v;
            img.rgba[at + 1] = v;
            img.rgba[at + 2] = v;
            img.rgba[at + 3] = 255;
        }
    }
    return img;
}

uint8_t constant200(int, int) { return 200; }
uint8_t leftRight(int x, int) { return static_cast<uint8_t>(x < 2 ? 0 : 255); }

}  // namespace

SR_TEST(mip_chain_halves_until_one_dimension_reaches_one) {
    const sr::ImageRGBA8 base = makeImage(8, 4, constant200);
    const std::vector<sr::ImageRGBA8> chain = sr::generateMipChain(base);
    SR_ASSERT_EQ(static_cast<int>(chain.size()), 4);   // 8x4 → 4x2 → 2x1 → 1x1
    SR_ASSERT_EQ(chain[0].width, 8); SR_ASSERT_EQ(chain[0].height, 4);
    SR_ASSERT_EQ(chain[1].width, 4); SR_ASSERT_EQ(chain[1].height, 2);
    SR_ASSERT_EQ(chain[2].width, 2); SR_ASSERT_EQ(chain[2].height, 1);
    SR_ASSERT_EQ(chain[3].width, 1); SR_ASSERT_EQ(chain[3].height, 1);
}

SR_TEST(box_downsample_averages_each_2x2_block) {
    const sr::ImageRGBA8 base = makeImage(2, 2, [](int x, int y) -> uint8_t {
        if (x == 0 && y == 0) return 0;
        if (x == 1 && y == 0) return 100;
        if (x == 0 && y == 1) return 200;
        return 60;
    });
    const sr::ImageRGBA8 mip = sr::downsampleBox2(base);
    SR_ASSERT_EQ(mip.width, 1);
    SR_ASSERT_EQ(mip.height, 1);
    SR_ASSERT_EQ(mip.rgba[0], 90);      // (0+100+200+60)/4 = 90
    SR_ASSERT_EQ(mip.rgba[1], 90);
    SR_ASSERT_EQ(mip.rgba[2], 90);
    SR_ASSERT_EQ(mip.rgba[3], 255);     // alpha 也做平均还是保留？约定：平均，这里全 255 所以看不出
}

SR_TEST(box_downsample_of_odd_width_drops_the_last_column) {
    // 奇数尺寸是 mip 链最容易写错的地方。约定：**向下取整**（丢掉最后一条不完整的行列），
    // 与 GPU 的行为一致；四舍五入向上会让链的总像素数反而增加。
    const sr::ImageRGBA8 base = makeImage(5, 3, constant200);
    const sr::ImageRGBA8 mip = sr::downsampleBox2(base);
    SR_ASSERT_EQ(mip.width, 2);
    SR_ASSERT_EQ(mip.height, 1);
}

SR_TEST(mip_chain_of_a_gradient_keeps_the_mean_constant) {
    // 平均保持性：整条链上"所有像素的平均值"应当基本不变（盒式滤波是保均值的）。
    // 这一条能抓住"把平均写成取点"或"除数用了错误的面积"。
    const sr::ImageRGBA8 base = makeImage(8, 8, [](int x, int) -> uint8_t {
        return static_cast<uint8_t>(x * 30);
    });
    const std::vector<sr::ImageRGBA8> chain = sr::generateMipChain(base);
    auto meanOf = [](const sr::ImageRGBA8& im) {
        double sum = 0.0;
        for (size_t i = 0; i < im.rgba.size(); i += 4) sum += im.rgba[i];
        return sum / (static_cast<double>(im.width) * im.height);
    };
    const double m0 = meanOf(chain[0]);
    for (size_t l = 1; l < chain.size(); ++l) {
        SR_ASSERT_NEAR(meanOf(chain[l]), m0, 1.0);   // 每级重新量化，允许 ±1
    }
}

SR_TEST(mip_level_from_derivatives_picks_the_lod_that_keeps_one_texel_per_pixel) {
    // 一个 256x256 的纹理铺满 256 像素：dP/dx = 1 ⇒ level 0。
    SR_ASSERT_EQ(sr::mipLevelFromDerivatives(256.0f, 256.0f, 1.0f, 0.0f, 0.0f, 1.0f), 0);
    // 铺满 64 像素：每像素跨 4 个纹素 ⇒ level 2（log2(4) = 2）
    SR_ASSERT_EQ(sr::mipLevelFromDerivatives(256.0f, 256.0f, 4.0f, 0.0f, 0.0f, 4.0f), 2);
    // 各向异性：x 跨 16、y 跨 1 ⇒ 取 max（不做各向异性过滤时这是标准做法）
    SR_ASSERT_EQ(sr::mipLevelFromDerivatives(256.0f, 256.0f, 16.0f, 0.0f, 0.0f, 1.0f), 4);
    // 被放大（每像素不足一个纹素）⇒ level 0，不能是负数
    SR_ASSERT_EQ(sr::mipLevelFromDerivatives(256.0f, 256.0f, 0.1f, 0.0f, 0.0f, 0.1f), 0);
}

SR_TEST(mip_level_is_clamped_to_the_chain_length) {
    // 链只有 4 级时，导数再大也不能越界读到 levels[9]（症状是崩溃或读到垃圾）。
    const sr::ImageRGBA8 base = makeImage(8, 8, constant200);
    const std::vector<sr::ImageRGBA8> chain = sr::generateMipChain(base);
    const int level = sr::mipLevelFromDerivatives(8.0f, 8.0f, 1000.0f, 0.0f, 0.0f, 1000.0f);
    SR_ASSERT(level >= 0);
    SR_ASSERT(level < static_cast<int>(chain.size()));
}

SR_TEST(texture_texel_returns_a_readable_pointer) {
    sr::Texture t;
    t.width = 4;
    t.height = 4;
    t.levels.push_back(makeImage(4, 4, leftRight));
    const uint8_t* p = t.texel(0, 0, 0);
    SR_ASSERT(p != nullptr);
    SR_ASSERT_EQ(p[0], 0);
    const uint8_t* q = t.texel(0, 3, 3);
    SR_ASSERT(q != nullptr);
    SR_ASSERT_EQ(q[0], 255);
    SR_ASSERT(t.texel(0, 99, 0) == nullptr);   // 越界返回 nullptr 而不是读到界外
    SR_ASSERT(t.texel(5, 0, 0) == nullptr);    // 不存在的层级同理
}
