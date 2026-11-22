#include "render/texture.h"

#include <cmath>

#include "harness.h"
#include "render/sampler.h"   // makeTexture / sampleNearestLevel（deferred #2 的越界采样）
#include "test_util.h"

namespace {

uint8_t leftRight(int x, int) { return static_cast<uint8_t>(x < 2 ? 0 : 255); }

}  // namespace

SR_TEST(mip_chain_halves_until_one_dimension_reaches_one) {
    const sr::ImageRGBA8 base = makeGrayImage(8, 4, grayConstant200);
    const std::vector<sr::ImageRGBA8> chain = sr::generateMipChain(base);
    SR_ASSERT_EQ(static_cast<int>(chain.size()), 4);   // 8x4 → 4x2 → 2x1 → 1x1
    // 尺寸断言必须被 size 保护：链短一级就会越界读 chain[3]，
    // 症状是**整个测试进程崩掉**而不是这一条失败 —— 上一轮"rc=3 且没有任何 FAIL 行"就是它。
    const int expect[4][2] = {{8, 4}, {4, 2}, {2, 1}, {1, 1}};
    for (size_t l = 0; l < chain.size() && l < 4; ++l) {
        SR_ASSERT_EQ(chain[l].width, expect[l][0]);
        SR_ASSERT_EQ(chain[l].height, expect[l][1]);
    }
}

SR_TEST(no_mip_level_is_ever_empty) {
    // 链的每一级都必须宽>=1 且高>=1。0 厚的一级既没法采样（texel() 全返回 nullptr），
    // 又会让"级数 = log2(边长)+1"这个直觉在所有非 2 的幂尺寸上失效。
    const int shapes[][2] = {{8, 4}, {5, 3}, {1, 8}, {2, 1}, {1, 1}, {3, 3}, {7, 2}};
    for (const auto& s : shapes) {
        const sr::ImageRGBA8 base = makeGrayImage(s[0], s[1], grayConstant200);
        const std::vector<sr::ImageRGBA8> chain = sr::generateMipChain(base);
        SR_ASSERT_FALSE(chain.empty());
        if (chain.empty()) continue;
        SR_ASSERT_EQ(chain[0].width, s[0]);
        SR_ASSERT_EQ(chain[0].height, s[1]);
        for (size_t l = 0; l < chain.size(); ++l) {
            SR_ASSERT(chain[l].width >= 1);
            SR_ASSERT(chain[l].height >= 1);
            // 每一级的字节数必须与它自己的宽高一致，否则 texel() 的行跨度是错的
            SR_ASSERT_EQ(static_cast<int>(chain[l].rgba.size()),
                         chain[l].width * chain[l].height * 4);
        }
        // 收尾必须是 1x1（GPU 的完整链定义）
        SR_ASSERT_EQ(chain.back().width, 1);
        SR_ASSERT_EQ(chain.back().height, 1);
    }
}

SR_TEST(box_downsample_averages_each_2x2_block) {
    const sr::ImageRGBA8 base = makeGrayImage(2, 2, [](int x, int y) -> uint8_t {
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

SR_TEST(box_downsample_averages_alpha_not_just_rgb) {
    // deferred #3（M5.4 关闭）：上面那条夹具 alpha 恒 255，分不出"平均"与"保持"。
    // 这里 alpha 交替 0/255 ⇒ 2x2 平均 = (0+255+255+0+2)/4 = 128。rgb 恒 100 ⇒ 同一次平均得 100。
    // 变异：alpha 若被"保持"（复制首像素 0）或强制 255，本条立刻红 —— M4 看不见的正是这个。
    sr::ImageRGBA8 base;
    base.width = 2;
    base.height = 2;
    base.rgba.assign(16, 0);
    const auto set = [&](int x, int y, uint8_t a) {
        const size_t at = (static_cast<size_t>(y) * 2 + static_cast<size_t>(x)) * 4u;
        base.rgba[at] = 100;
        base.rgba[at + 1] = 100;
        base.rgba[at + 2] = 100;
        base.rgba[at + 3] = a;
    };
    set(0, 0, 0);
    set(1, 0, 255);
    set(0, 1, 255);
    set(1, 1, 0);
    const sr::ImageRGBA8 mip = sr::downsampleBox2(base);
    SR_ASSERT_EQ(mip.width, 1);
    SR_ASSERT_EQ(mip.height, 1);
    SR_ASSERT_EQ(mip.rgba[0], 100);          // rgb 与 alpha 走同一次 2x2 平均
    SR_ASSERT_NEAR(mip.rgba[3], 128.0, 1.0); // 127/128 都接受（±1 量化）
}

SR_TEST(box_downsample_of_odd_width_drops_the_last_column) {
    // 奇数尺寸是 mip 链最容易写错的地方。约定：**向下取整**（丢掉最后一条不完整的行列），
    // 与 GPU 的行为一致；四舍五入向上会让链的总像素数反而增加。
    const sr::ImageRGBA8 base = makeGrayImage(5, 3, grayConstant200);
    const sr::ImageRGBA8 mip = sr::downsampleBox2(base);
    SR_ASSERT_EQ(mip.width, 2);
    SR_ASSERT_EQ(mip.height, 1);
}

SR_TEST(mip_chain_of_a_gradient_keeps_the_mean_constant) {
    // 平均保持性：整条链上"所有像素的平均值"应当基本不变（盒式滤波是保均值的）。
    // 这一条能抓住"把平均写成取点"或"除数用了错误的面积"。
    const sr::ImageRGBA8 base = makeGrayImage(8, 8, [](int x, int) -> uint8_t {
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

// 单位约定（这两个测试把它钉死）：传进来的导数是 **归一化 UV / 像素**，
// 也就是 TriangleSetup::dudx/dvdy 那一套（Mesh 的 uv 是 j/slices，取值 [0,1)）。
// 换算成"纹素/像素"是 mipLevelFromDerivatives 自己的事，因为它手里同时有纹理尺寸。
// 上一轮测试按"纹素/像素"传值，于是四条断言整整齐齐差 log2(256) = 8 级 ——
// 差一个固定常数而不是随机错，就是单位错的指纹。
constexpr float kTexels = 256.0f;

SR_TEST(mip_level_from_derivatives_picks_the_lod_that_keeps_one_texel_per_pixel) {
    // 256 纹素宽的纹理铺满 256 像素 ⇒ 每像素跨 1 个纹素 ⇒ level 0。
    SR_ASSERT_EQ(sr::mipLevelFromDerivatives(kTexels, kTexels, 4, 1.0f / kTexels, 0.0f, 0.0f,
                                             1.0f / kTexels), 0);
    // 铺满 64 像素：每像素跨 4 个纹素 ⇒ level 2（log2 4）
    SR_ASSERT_EQ(sr::mipLevelFromDerivatives(kTexels, kTexels, 4, 4.0f / kTexels, 0.0f, 0.0f,
                                             4.0f / kTexels), 2);
    // 各向异性：x 跨 16 纹素、y 跨 1 ⇒ 取 max（不做各向异性过滤时这是标准做法）
    SR_ASSERT_EQ(sr::mipLevelFromDerivatives(kTexels, kTexels, 8, 16.0f / kTexels, 0.0f, 0.0f,
                                             1.0f / kTexels), 4);
    // 被放大（每像素不足一个纹素）⇒ level 0，不能是负数
    SR_ASSERT_EQ(sr::mipLevelFromDerivatives(kTexels, kTexels, 8, 0.1f / kTexels, 0.0f, 0.0f,
                                             0.1f / kTexels), 0);
    // 区分力：同一个数字按"纹素/像素"理解会是 level 0，按 UV/像素理解是 8。
    // 谁把单位改了，这条会立刻说出是哪一边。
    SR_ASSERT_EQ(sr::mipLevelFromDerivatives(kTexels, kTexels, 16, 1.0f, 0.0f, 0.0f, 0.0f), 8);
    // 导数是**有符号**的：V 翻转（负缩放模型、OBJ 的 uv 原点差异）不该改变层级选择
    SR_ASSERT_EQ(sr::mipLevelFromDerivatives(kTexels, kTexels, 8, -4.0f / kTexels, 0.0f, 0.0f,
                                             -4.0f / kTexels), 2);
}

SR_TEST(mip_level_scales_each_derivative_by_its_own_texture_dimension) {
    // 128x32 的纹理：W/H = 4 = 正好两级，所以"u 的分量错乘了 H"这类抄写错
    // 会把结果挪到 level 2 或 level 6，不会藏在同一个 level 里。
    // 四行各自只让**一个**导数分量非零 —— 上一版把两个方向都填了数，结果
    // 交叉项是 0，一个真实存在的变异（dVdx 错乘 W）照样通过：一项要能被检出，
    // 它得先是那行里唯一说话的人。
    const float W = 128.0f, H = 32.0f;
    SR_ASSERT_EQ(sr::mipLevelFromDerivatives(W, H, 8, 30.0f / W, 0.0f, 0.0f, 0.0f), 4);  // dU/dx
    SR_ASSERT_EQ(sr::mipLevelFromDerivatives(W, H, 8, 0.0f, 30.0f / H, 0.0f, 0.0f), 4);  // dV/dx
    SR_ASSERT_EQ(sr::mipLevelFromDerivatives(W, H, 8, 0.0f, 0.0f, 30.0f / W, 0.0f), 4);  // dU/dy
    SR_ASSERT_EQ(sr::mipLevelFromDerivatives(W, H, 8, 0.0f, 0.0f, 0.0f, 30.0f / H), 4);  // dV/dy
    // 各向异性取 max 而不是平均：两个方向各跨 30 与 1 个纹素 ⇒ 跟着 30 走
    SR_ASSERT_EQ(sr::mipLevelFromDerivatives(W, H, 8, 30.0f / W, 0.0f, 0.0f, 1.0f / H), 4);
}

SR_TEST(mip_level_is_clamped_to_the_chain_length) {
    // 链只有 4 级时，导数再大也不能越界读到 levels[9]（症状是崩溃或读到垃圾）。
    // 夹取必须在函数内部：让每个调用方"记得"夹一次，等于安排它哪天忘一次。
    const sr::ImageRGBA8 base = makeGrayImage(8, 8, grayConstant200);
    const std::vector<sr::ImageRGBA8> chain = sr::generateMipChain(base);
    SR_ASSERT_EQ(static_cast<int>(chain.size()), 4);   // 8x8 → 4x4 → 2x2 → 1x1
    const int level = sr::mipLevelFromDerivatives(8.0f, 8.0f, static_cast<int>(chain.size()),
                                                  1000.0f, 0.0f, 0.0f, 1000.0f);
    SR_ASSERT(level >= 0);
    SR_ASSERT(level < static_cast<int>(chain.size()));
    SR_ASSERT_EQ(level, 3);                            // 不只是"在范围内"，要夹到最末一级
}

SR_TEST(texture_texel_returns_a_readable_pointer) {
    sr::Texture t;
    t.width = 4;
    t.height = 4;
    t.levels.push_back(makeGrayImage(4, 4, leftRight));
    const uint8_t* p = t.texel(0, 0, 0);
    SR_ASSERT(p != nullptr);
    SR_ASSERT_EQ(p[0], 0);
    const uint8_t* q = t.texel(0, 3, 3);
    SR_ASSERT(q != nullptr);
    SR_ASSERT_EQ(q[0], 255);
    SR_ASSERT(t.texel(0, 99, 0) == nullptr);   // 越界返回 nullptr 而不是读到界外
    SR_ASSERT(t.texel(5, 0, 0) == nullptr);    // 不存在的层级同理
}

SR_TEST(texture_counts_out_of_range_texel_samples_in_debug) {
#ifdef NDEBUG
    // Ruling 48：计数是 debug-only 观测器，Release 下成员都不存在 ⇒ 打印 [skip] 后 return，
    // 不留空体、也不留一条恒真断言（"绿得没有内容"）。Debug 是主判定路径（C1′）。
    std::printf("[skip] 越界纹素计数是 debug-only 观测器，Release 不判定（Ruling 48）\n");
    return;
#else
    // deferred #2：故意构造一个 levels 少一级的 Texture，采那个不存在的级 ⇒ texel() 交回
    // nullptr ⇒ toTexelF 静默给全 0，但 debug 计数必须 bump（把"reachable 的静默"变可观测）。
    sr::Texture t = sr::makeTexture(makeGrayImage(4, 4, grayConstant200), true);
    SR_ASSERT_EQ(t.levelCount(), 3);          // 4x4 → 2x2 → 1x1
    t.levels.pop_back();                      // 少一级
    SR_ASSERT_EQ(t.levelCount(), 2);
    const sr::TexelF z = sr::sampleNearestLevel(t, 2, sr::WrapMode::ClampEdge,
                                                sr::WrapMode::ClampEdge, 0.5f, 0.5f);
    SR_ASSERT_NEAR(z.r, 0.0f, 1e-6f);         // 越界仍返回有定义的全 0（不放 NaN）
    SR_ASSERT(t.outOfRangeTexelSamples > 0);  // 变异：删掉 toTexelF 的 bump ⇒ 本条红
#endif
}
