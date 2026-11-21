#include <cstdint>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "harness.h"
#include "loader/bmp_writer.h"
#include "loader/image_stb.h"
#include "render/color.h"
#include "render/sampler.h"   // makeTexture / sampleNearest / TexelF 在这里，不在 texture.h
#include "render/render_target.h"
#include "render/texture.h"
#include "test_util.h"

namespace {

const char* kRoundtripPath = "sr_tmp_bmp/m4_stb_roundtrip.bmp";
const char* kNotAnImagePath = "sr_tmp_bmp/m4_stb_not_an_image.bin";

// 一张"行颜色不同"的图：上红下蓝。
// 选这个形状的理由：它能同时揭穿两类错误 ——
//   ① 行序翻转（BMP 文件是自底向上存的，解码器给我们的是 top-down 还是 bottom-up？）
//   ② 通道顺序（BMP 内存里是 B,G,R；我们的 ImageRGBA8 约定是 R,G,B,A）
// 任何一种错了，(0,0) 与 (0,1) 的值都会对不上。
void writeRedBlueBmp(const std::string& path, std::string& err) {
    std::vector<uint8_t> mem(4 * 2 * 4, 0);
    sr::ColorTarget t{reinterpret_cast<uint32_t*>(mem.data()), 4, 2, 4 * 4};
    for (int x = 0; x < 4; ++x) {
        t.writePacked(x, 0, sr::packBGRA(255, 0, 0, 255));     // 上一行：红
        t.writePacked(x, 1, sr::packBGRA(0, 0, 255, 255));     // 下一行：蓝
    }
    if (!sr::writeBmp(path, t, err)) err = "写出 BMP 失败: " + err;
}

// 先确认字节数够，不够就**立刻结束这条测试**。
// 直接 img.rgba[0] 会在加载失败时对空 vector 取元素 → 整个测试进程 abort（rc=3），
// 一次崩溃连带吃掉其它所有用例的结果 —— 这正是 Ruling 26 记过的那条坑，我在 M4.6 又踩了一遍。
bool hasBytes(const sr::ImageRGBA8& img, int n) {
    SR_ASSERT_EQ(static_cast<int>(img.rgba.size()), n);
    return static_cast<int>(img.rgba.size()) >= n;
}

}  // namespace

SR_TEST(stb_loader_missing_file_reports_error) {
    sr::ImageRGBA8 img;
    std::string err;
    SR_ASSERT_FALSE(sr::loadImageFile("sr_tmp_bmp/definitely_not_here_9182.png", img, err));
    SR_ASSERT_FALSE(err.empty());
    SR_ASSERT_EQ(img.width, 0);        // 失败时不得留下半张图
    SR_ASSERT_EQ(static_cast<int>(img.rgba.size()), 0);
}

SR_TEST(stb_loader_rejects_a_file_that_is_not_an_image) {
    {
        std::ofstream f(kNotAnImagePath, std::ios::binary);
        f << "this is not any image format at all\n";
    }
    sr::ImageRGBA8 img;
    std::string err;
    SR_ASSERT_FALSE(sr::loadImageFile(kNotAnImagePath, img, err));
    SR_ASSERT_FALSE(err.empty());
    // err 里带上文件名才有用：加载失败通常发生在运行时用户 --obj 指了个怪路径
    SR_ASSERT(err.find(kNotAnImagePath) != std::string::npos);
}

SR_TEST(stb_loader_roundtrips_a_bmp_written_by_our_own_writer) {
    std::string err;
    writeRedBlueBmp(kRoundtripPath, err);
    SR_ASSERT(err.empty());

    sr::ImageRGBA8 img;
    SR_ASSERT(sr::loadImageFile(kRoundtripPath, img, err));
    SR_ASSERT_EQ(img.width, 4);
    SR_ASSERT_EQ(img.height, 2);
    if (!hasBytes(img, 4 * 2 * 4)) return;   // 尺寸不对就别再往下索引了

    // 第 0 行必须是红色 ⇒ top-down（与 C8 一致），且 stb 已经把 BGR 翻成 RGB
    const uint8_t* top = &img.rgba[0];
    SR_ASSERT_EQ(top[0], 255);   // r
    SR_ASSERT_EQ(top[1], 0);     // g
    SR_ASSERT_EQ(top[2], 0);     // b
    SR_ASSERT_EQ(top[3], 255);   // a：24bpp 源没有 alpha，必须补成不透明而不是 0
    const uint8_t* bottom = &img.rgba[4 * 4];
    SR_ASSERT_EQ(bottom[0], 0);
    SR_ASSERT_EQ(bottom[1], 0);
    SR_ASSERT_EQ(bottom[2], 255);
    SR_ASSERT_EQ(bottom[3], 255);

    // 拿它当真纹理用一遍：链要能生成，末级是 1x1
    const sr::Texture tex = sr::makeTexture(std::move(img), /*colorDataIsSrgb=*/false);
    SR_ASSERT(tex.srgb == false);
    SR_ASSERT_EQ(tex.levelCount(), 3);            // 4x2 → 2x1 → 1x1
    SR_ASSERT(tex.texel(2, 0, 0) != nullptr);
}

SR_TEST(stb_loader_can_be_used_as_a_texture_source) {
    // 这一条把"文件 → ImageRGBA8 → Texture → 能采样"整条链走通一次。
    // 单独测每环都不难，难的是三环接起来时宽高/行跨度错位 —— 那是 M4 可见成果的地基。
    std::string err;
    writeRedBlueBmp(kRoundtripPath, err);
    sr::ImageRGBA8 img;
    SR_ASSERT(sr::loadImageFile(kRoundtripPath, img, err));
    const sr::Texture tex = sr::makeTexture(std::move(img), /*colorDataIsSrgb=*/false);

    // 上半（v=0.25）红、下半（v=0.75）蓝；srgb=false 所以数值直接是 1.0 / 0.0
    const sr::TexelF up =
        sr::sampleNearest(tex, sr::WrapMode::ClampEdge, sr::WrapMode::ClampEdge, 0.5f, 0.25f);
    SR_ASSERT_NEAR(up.r, 1.0f, 1e-6f);
    SR_ASSERT_NEAR(up.b, 0.0f, 1e-6f);
    const sr::TexelF down =
        sr::sampleNearest(tex, sr::WrapMode::ClampEdge, sr::WrapMode::ClampEdge, 0.5f, 0.75f);
    SR_ASSERT_NEAR(down.r, 0.0f, 1e-6f);
    SR_ASSERT_NEAR(down.b, 1.0f, 1e-6f);
}
