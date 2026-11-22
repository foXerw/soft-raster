#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "harness.h"
#include "loader/image_tga.h"
#include "render/texture.h"

namespace {

// TGA 头 18 字节。这里只填测试关心的字段，其余为 0。
std::vector<uint8_t> header(int type, int w, int h, int bpp, int descriptor) {
    std::vector<uint8_t> d(18, 0);
    d[2] = static_cast<uint8_t>(type);
    d[12] = static_cast<uint8_t>(w & 0xFF);  d[13] = static_cast<uint8_t>((w >> 8) & 0xFF);
    d[14] = static_cast<uint8_t>(h & 0xFF);  d[15] = static_cast<uint8_t>((h >> 8) & 0xFF);
    d[16] = static_cast<uint8_t>(bpp);
    d[17] = static_cast<uint8_t>(descriptor);
    return d;
}

void appendPixel(std::vector<uint8_t>& d, int r, int g, int b, int a) {
    // TGA 的字节序是 **B,G,R**（与 BMP 同源），我们的 ImageRGBA8 约定是 R,G,B,A
    d.push_back(static_cast<uint8_t>(b));
    d.push_back(static_cast<uint8_t>(g));
    d.push_back(static_cast<uint8_t>(r));
    if (a >= 0) d.push_back(static_cast<uint8_t>(a));
}

bool parse(const std::vector<uint8_t>& bytes, sr::ImageRGBA8& out, std::string& err) {
    return sr::parseTga(bytes.data(), bytes.size(), out, err);
}

// 与 test_image_stb.cpp 同一条护栏：解析失败时 img 是空的，
// 而对空 vector 取 rgba[0] 会让**整个测试进程** abort —— 一次崩溃吃掉所有用例的结果。
bool hasBytes(const sr::ImageRGBA8& img, int n) {
    SR_ASSERT_EQ(static_cast<int>(img.rgba.size()), n);
    return static_cast<int>(img.rgba.size()) >= n;
}

}  // namespace

SR_TEST(tga_payload_guard_is_a_pure_predicate_with_exact_boundaries) {
    // 这个守卫存在的理由：**分配必须发生在检查之后**。RLE 分支以前只检查了未压缩的长度，
    // 于是一个 18 字节的头声称 65535x65535 就能让解析器先 assign 16 GiB 再决定拒绝
    // （终审抓到；M5 要载入网上下载的图片，那时它就是真的攻击面）。
    // 抽成纯函数是因为"边界恰好够/恰好差一字节"这种事只有直接问才测得准
    // —— 走 parseTga 的话，两种失败都给 false，只是消息不同。
    SR_ASSERT(sr::tgaPayloadFits(2, 2, 3, false, 12));        // 未压缩：正好 4*3
    SR_ASSERT_FALSE(sr::tgaPayloadFits(2, 2, 3, false, 11));
    // RLE 最省的编码是每包 128 个像素，每包至少 1 + bytesPP 字节（run 包）
    SR_ASSERT(sr::tgaPayloadFits(128, 1, 4, true, 5));
    SR_ASSERT_FALSE(sr::tgaPayloadFits(128, 1, 4, true, 4));
    SR_ASSERT(sr::tgaPayloadFits(129, 1, 4, true, 10));       // 两个包
    SR_ASSERT_FALSE(sr::tgaPayloadFits(129, 1, 4, true, 9));
    // 非法输入 ⇒ false，而不是"随便给个数"
    SR_ASSERT_FALSE(sr::tgaPayloadFits(0, 4, 3, true, 1000));
    SR_ASSERT_FALSE(sr::tgaPayloadFits(4, 4, 0, true, 1000));
    SR_ASSERT_FALSE(sr::tgaPayloadFits(-1, 4, 3, false, 1000));
    // 尺寸大到乘集会回绕时也必须说"放不下"（这里靠除法而不是乘法来问，见实现）
    SR_ASSERT_FALSE(sr::tgaPayloadFits(65535, 65535, 4, false, 1024));
    SR_ASSERT_FALSE(sr::tgaPayloadFits(65535, 65535, 4, true, 0));
    SR_ASSERT(sr::tgaPayloadFits(65535, 65535, 4, true, 1u << 30));   // 16 GiB 像素 ⇒ 至少 ~134 MB 包
}

SR_TEST(tga_rle_huge_claim_is_rejected_before_allocating) {
    // 行为侧的同一条：错误消息必须来自**分配之前**的那个分支，
    // 而不是循环里读到一半的"被截断"（后者意味着已经分配过了）。
    std::vector<uint8_t> d = header(10, 2048, 2048, 32, 0x20);   // 18 字节，没有任何像素数据
    sr::ImageRGBA8 img;
    std::string err;
    SR_ASSERT_FALSE(parse(d, img, err));
    SR_ASSERT_FALSE(err.empty());
    if (err.find("像素数据不足") == std::string::npos) std::printf("[tga] 错误消息是：%s\n", err.c_str());
    SR_ASSERT(err.find("像素数据不足") != std::string::npos);
    SR_ASSERT_EQ(img.rgba.size(), 0u);
}

SR_TEST(tga_24bit_bottom_up_storage_becomes_top_down) {
    // 2x2，type 2（未压缩 truecolor），descriptor = 0 ⇒ **行自底向上存储**（TGA 的默认）。
    // 输出要求 top-down（C8），所以文件里的第一行必须落到输出的**最后一行**。
    std::vector<uint8_t> d = header(2, 2, 2, 24, 0);
    appendPixel(d, 0, 0, 255, -1);      // 存储行 0 = 图像**下**行：蓝
    appendPixel(d, 0, 255, 0, -1);
    appendPixel(d, 255, 0, 0, -1);      // 存储行 1 = 图像**上**行：红
    appendPixel(d, 0, 255, 255, -1);

    sr::ImageRGBA8 img;
    std::string err;
    SR_ASSERT(parse(d, img, err));
    SR_ASSERT_EQ(img.width, 2);
    SR_ASSERT_EQ(img.height, 2);
    if (!hasBytes(img, 2 * 2 * 4)) return;
    SR_ASSERT_EQ(img.rgba[0], 255); SR_ASSERT_EQ(img.rgba[1], 0);   SR_ASSERT_EQ(img.rgba[2], 0);
    SR_ASSERT_EQ(img.rgba[3], 255);                                  // 没有 alpha 通道 ⇒ 补成不透明
    // 行跨度是 width*4 = 8，不是 4 —— 上一版把"第二行"当成了 offset 4，
    // 那其实是第一行的第二个像素（黄），于是实现是对的、测试是错的。
    SR_ASSERT_EQ(img.rgba[4], 0);   SR_ASSERT_EQ(img.rgba[5], 255); SR_ASSERT_EQ(img.rgba[6], 255);
    const size_t lastRow = 8;                                         // (h-1) * w * 4
    SR_ASSERT_EQ(img.rgba[lastRow], 0); SR_ASSERT_EQ(img.rgba[lastRow + 1], 0);
    SR_ASSERT_EQ(img.rgba[lastRow + 2], 255);                        // 下行第一个 = 蓝
}

SR_TEST(tga_descriptor_top_down_bit_is_honored) {
    std::vector<uint8_t> d = header(2, 2, 2, 24, 0x20);   // bit5 = 1 ⇒ 存储顺序已经是 top-down
    appendPixel(d, 255, 0, 0, -1);
    appendPixel(d, 0, 255, 0, -1);
    appendPixel(d, 0, 0, 255, -1);
    appendPixel(d, 0, 255, 255, -1);
    sr::ImageRGBA8 img;
    std::string err;
    SR_ASSERT(parse(d, img, err));
    if (!hasBytes(img, 2 * 2 * 4)) return;
    SR_ASSERT_EQ(img.rgba[0], 255); SR_ASSERT_EQ(img.rgba[2], 0);     // 第一行第一个 = 红
    SR_ASSERT_EQ(img.rgba[4], 0);   SR_ASSERT_EQ(img.rgba[5], 255);   // 第一行第二个 = 绿
    SR_ASSERT_EQ(img.rgba[8], 0);   SR_ASSERT_EQ(img.rgba[10], 255);  // 第二行第一个 = 蓝
}

SR_TEST(tga_32bit_keeps_alpha) {
    std::vector<uint8_t> d = header(2, 1, 1, 32, 0x20);
    appendPixel(d, 10, 20, 30, 128);
    sr::ImageRGBA8 img;
    std::string err;
    SR_ASSERT(parse(d, img, err));
    if (!hasBytes(img, 4)) return;
    SR_ASSERT_EQ(img.rgba[0], 10);
    SR_ASSERT_EQ(img.rgba[1], 20);
    SR_ASSERT_EQ(img.rgba[2], 30);
    SR_ASSERT_EQ(img.rgba[3], 128);        // alpha 原样保留，不做 gamma、也不做 premultiply
}

SR_TEST(tga_rle_decodes_run_and_raw_packets) {
    // type 10 = RLE truecolor。包头的 bit7 是 run 标志，低 7 位是"含本像素共 count 个"，
    // 所以 0x82 表示"紧跟的 1 个像素重复 3 次"，0x01 表示"紧跟 2 个原始像素"。
    std::vector<uint8_t> d = header(10, 5, 1, 24, 0x20);
    d.push_back(0x82);
    appendPixel(d, 255, 0, 0, -1);          // 3 × 红
    d.push_back(0x01);
    appendPixel(d, 0, 255, 0, -1);          // 2 × (绿, 蓝)
    appendPixel(d, 0, 0, 255, -1);
    sr::ImageRGBA8 img;
    std::string err;
    SR_ASSERT(parse(d, img, err));
    SR_ASSERT_EQ(img.width, 5);
    SR_ASSERT_EQ(img.height, 1);
    if (!hasBytes(img, 5 * 4)) return;
    for (int x = 0; x < 3; ++x) {
        SR_ASSERT_EQ(img.rgba[x * 4 + 0], 255);
        SR_ASSERT_EQ(img.rgba[x * 4 + 2], 0);
    }
    SR_ASSERT_EQ(img.rgba[3 * 4 + 1], 255);   // x=3 绿
    SR_ASSERT_EQ(img.rgba[4 * 4 + 2], 255);   // x=4 蓝
}

SR_TEST(tga_truncated_input_fails_instead_of_reading_past_the_end) {
    // 声明 8x8 但只给了头 + 几个像素。**必须**返回 false，而不是读出越界内存 ——
    // 加载器处理的是用户/网络给来的文件，这条是纯粹的健壮性底线。
    std::vector<uint8_t> d = header(2, 8, 8, 24, 0x20);
    appendPixel(d, 1, 2, 3, -1);
    sr::ImageRGBA8 img;
    std::string err;
    SR_ASSERT_FALSE(parse(d, img, err));
    SR_ASSERT_FALSE(err.empty());
    SR_ASSERT_EQ(img.width, 0);              // 失败不留半成品

    // RLE 的截断走的是另一条循环，也要单独喂一遍
    std::vector<uint8_t> r = header(10, 8, 8, 24, 0x20);
    r.push_back(0x7F);                        // 声称有 128 个原始像素，后面什么都没有
    sr::ImageRGBA8 img2;
    std::string err2;
    SR_ASSERT_FALSE(parse(r, img2, err2));
    SR_ASSERT_FALSE(err2.empty());
}

SR_TEST(tga_rejects_unsupported_types_and_dimensions) {
    sr::ImageRGBA8 img;
    std::string err;
    // 调色板型（1/9）与 16bpp：本项目不支持，且要说清为什么（返回 false 比画错颜色诚实）
    std::vector<uint8_t> cmap = header(1, 1, 1, 8, 0x20);
    cmap[1] = 1;                              // color map type
    cmap.push_back(0);
    SR_ASSERT_FALSE(parse(cmap, img, err));
    SR_ASSERT_FALSE(err.empty());

    std::vector<uint8_t> bpp16 = header(2, 1, 1, 16, 0x20);
    bpp16.push_back(0); bpp16.push_back(0);
    SR_ASSERT_FALSE(parse(bpp16, img, err));
    SR_ASSERT_FALSE(err.empty());

    std::vector<uint8_t> zeroW = header(2, 0, 4, 24, 0x20);
    SR_ASSERT_FALSE(parse(zeroW, img, err));
    SR_ASSERT_FALSE(err.empty());

    std::vector<uint8_t> tooShort = {1, 2, 3};
    SR_ASSERT_FALSE(parse(tooShort, img, err));
    SR_ASSERT_FALSE(err.empty());
}

SR_TEST(tga_grayscale_replicates_into_rgb) {
    std::vector<uint8_t> d = header(3, 2, 1, 8, 0x20);
    d.push_back(40);
    d.push_back(200);
    sr::ImageRGBA8 img;
    std::string err;
    SR_ASSERT(parse(d, img, err));
    if (!hasBytes(img, 2 * 4)) return;
    SR_ASSERT_EQ(img.rgba[0], 40); SR_ASSERT_EQ(img.rgba[1], 40); SR_ASSERT_EQ(img.rgba[2], 40);
    SR_ASSERT_EQ(img.rgba[3], 255);
    SR_ASSERT_EQ(img.rgba[4], 200); SR_ASSERT_EQ(img.rgba[6], 200);
}

SR_TEST(tga_loads_from_a_chinese_named_directory) {
#ifndef _MSC_VER
    // C1′：与 stb_loads_a_png_from_a_chinese_named_directory 同一条理由 ——
    // D12 的宽字符打开路径只在 MSVC 主路径存在（ifstream 的 fs::path 重载）。
    std::printf("[skip] 中文路径用例只在 MSVC 判定（C1′）\n");
    return;
#else
    // D12 的 TGA 侧：loadTgaFile 收 fs::path，MSVC 的 ifstream(path) 走宽字符打开。
    // 夹具用本文件已有的 header()/appendPixel() 手工拼字节（不依赖 writeTga，
    // 读侧测试不该把写侧当 oracle），经 ofstream(fs::path) 写进中文目录。
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(L"sr_tmp_bmp") / L"中文tga目录_m5";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        std::printf("[skip] 无法创建中文目录: %s\n", ec.message().c_str());
        return;
    }
    std::vector<uint8_t> d = header(2, 2, 1, 32, 0);   // bottom-up，2x1
    appendPixel(d, 10, 200, 30, 255);
    appendPixel(d, 200, 10, 30, 128);
    const fs::path p = dir / L"棋盘.tga";
    {
        std::ofstream f(p, std::ios::binary);
        f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
        SR_ASSERT(static_cast<bool>(f));
    }
    sr::ImageRGBA8 img;
    std::string err;
    const bool ok = sr::loadTgaFile(p, img, err);
    SR_ASSERT(ok);
    if (!ok) std::printf("[tga] 中文路径加载失败: %s\n", err.c_str());
    if (ok && hasBytes(img, 2 * 4)) {
        SR_ASSERT_EQ(img.rgba[0], 10);
        SR_ASSERT_EQ(img.rgba[1], 200);
        SR_ASSERT_EQ(img.rgba[4], 200);
        SR_ASSERT_EQ(img.rgba[7], 128);   // alpha 原样
    }
    fs::remove_all(dir, ec);
#endif
}
