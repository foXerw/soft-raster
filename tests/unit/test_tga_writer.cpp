#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "harness.h"
#include "loader/image_tga.h"
#include "loader/tga_writer.h"
#include "render/texture.h"
#include "scene/golden_scene.h"   // kGoldenCheckerCells / kGoldenCheckerCellPx

namespace {

const char* kRoundtripPath = "sr_tmp_bmp/m5_tga_writer_roundtrip.tga";
const char* kBottomUpPath = "sr_tmp_bmp/m5_tga_writer_bottom_up.tga";
const char* kEmptyGuardPath = "sr_tmp_bmp/m5_tga_writer_empty_guard.tga";

// 3x2、**每个像素 R≠G≠B 且 alpha 各不相同**。非灰度是刻意的：
// 灰度像素在 B/G/R 任意置换下不变，通道序写错了也测不出来（M4 夹具同一家族的坑）。
// alpha 取 {255,128,0,200,64,255}：顺带钉住"alpha 原样存第 4 字节，不 premultiply、不丢"。
sr::ImageRGBA8 makeFixture3x2() {
    sr::ImageRGBA8 img;
    img.width = 3;
    img.height = 2;
    const uint8_t px[6][4] = {
        {10, 20, 30, 255}, {40, 50, 60, 128}, {70, 80, 90, 0},      // 上行
        {200, 100, 50, 255}, {11, 222, 33, 64}, {99, 88, 77, 200},  // 下行
    };
    for (const auto& p : px)
        for (int c = 0; c < 4; ++c) img.rgba.push_back(p[c]);
    return img;
}

bool readFileBytes(const char* path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff len = f.tellg();
    if (len <= 0) return false;
    out.resize(static_cast<size_t>(len));
    f.seekg(0, std::ios::beg);
    return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), len));
}

// 与 test_image_stb.cpp 同一条护栏：SR_ASSERT 记录失败后**继续执行**，
// 对空 vector 索引会让整个测试进程 abort，一次崩溃吃掉所有用例的结果。
bool hasBytes(const std::vector<uint8_t>& v, size_t n) {
    SR_ASSERT_EQ(static_cast<long long>(v.size()), static_cast<long long>(n));
    return v.size() >= n;
}

}  // namespace

SR_TEST(tga_writer_roundtrips_a_small_image_byte_for_byte) {
    const sr::ImageRGBA8 src = makeFixture3x2();
    std::string err;
    SR_ASSERT(sr::writeTga(kRoundtripPath, src, err));
    SR_ASSERT(err.empty());

    std::vector<uint8_t> bytes;
    SR_ASSERT(readFileBytes(kRoundtripPath, bytes));
    sr::ImageRGBA8 back;
    SR_ASSERT(sr::parseTga(bytes.data(), bytes.size(), back, err));
    SR_ASSERT_EQ(back.width, 3);
    SR_ASSERT_EQ(back.height, 2);
    if (!hasBytes(back.rgba, src.rgba.size())) return;
    for (size_t i = 0; i < src.rgba.size(); ++i) SR_ASSERT_EQ(back.rgba[i], src.rgba[i]);
}

SR_TEST(tga_writer_marks_bottom_up_so_reading_must_flip) {
    // 锚 2 的一半：**故意**写 bottom-up（descriptor bit5 = 0，TGA 的历史默认），
    // 于是"读回来"必然要走一次行翻转 —— 那条翻转路径才有东西可守（设计 §10）。
    // 这里不经过 writeTga→parseTga 的往返自证，而是直接检查**文件字节**：
    // 存储的第一行必须是图像的底行、且字节序是 B,G,R,A。这是独立于读回路径的 oracle。
    const sr::ImageRGBA8 src = makeFixture3x2();
    std::string err;
    SR_ASSERT(sr::writeTga(kBottomUpPath, src, err));

    std::vector<uint8_t> raw;
    SR_ASSERT(readFileBytes(kBottomUpPath, raw));
    if (!hasBytes(raw, 18 + 3 * 2 * 4)) return;
    SR_ASSERT_EQ(raw[2], 2);                     // 未压缩 truecolor
    SR_ASSERT_EQ(raw[16], 32);                   // 32bpp
    SR_ASSERT_EQ(raw[17] & 0x20, 0);             // origin bit = 0 ⇒ bottom-up
    SR_ASSERT_EQ(raw[12] | (raw[13] << 8), 3);   // 宽 LE
    SR_ASSERT_EQ(raw[14] | (raw[15] << 8), 2);   // 高 LE
    // 文件第一个存储像素 = 图像**左下**角 (200,100,50,255)，字节序 B,G,R,A。
    // 非灰度 ⇒ R/B 互换在这四条断言下必红。
    SR_ASSERT_EQ(raw[18], 50);
    SR_ASSERT_EQ(raw[19], 100);
    SR_ASSERT_EQ(raw[20], 200);
    SR_ASSERT_EQ(raw[21], 255);

    sr::ImageRGBA8 back;
    SR_ASSERT(sr::parseTga(raw.data(), raw.size(), back, err));
    if (!hasBytes(back.rgba, src.rgba.size())) return;
    // 读回的 row0 == 写进文件的**最后**一行（= 图像顶行）：读回必须翻一次。
    const size_t lastStoredRow = 18 + 3 * 4;     // 第二存储行（顶行）的文件偏移
    for (int x = 0; x < 3; ++x) {
        const size_t f = lastStoredRow + static_cast<size_t>(x) * 4;
        const size_t m = static_cast<size_t>(x) * 4;
        SR_ASSERT_EQ(back.rgba[m + 0], raw[f + 2]);   // R ← 文件第 3 字节
        SR_ASSERT_EQ(back.rgba[m + 1], raw[f + 1]);
        SR_ASSERT_EQ(back.rgba[m + 2], raw[f + 0]);
        SR_ASSERT_EQ(back.rgba[m + 3], raw[f + 3]);
        SR_ASSERT_EQ(back.rgba[m + 0], src.rgba[m + 0]);
        SR_ASSERT_EQ(back.rgba[m + 3], src.rgba[m + 3]);
    }
}

SR_TEST(tga_writer_rejects_an_empty_image) {
    std::string err;
    sr::ImageRGBA8 bad;
    bad.width = 0; bad.height = 2;
    SR_ASSERT_FALSE(sr::writeTga(kEmptyGuardPath, bad, err));
    SR_ASSERT_FALSE(err.empty());
    bad.width = 2; bad.height = 0;
    SR_ASSERT_FALSE(sr::writeTga(kEmptyGuardPath, bad, err));
    SR_ASSERT_FALSE(err.empty());
    bad.width = -1; bad.height = 2;
    SR_ASSERT_FALSE(sr::writeTga(kEmptyGuardPath, bad, err));
    SR_ASSERT_FALSE(err.empty());
    // 宽高与字节数不符也要拒绝（半张图比没有图更糟，与 loadImageFile 同一条理由）
    bad.width = 2; bad.height = 2; bad.rgba.assign(4, 0);
    SR_ASSERT_FALSE(sr::writeTga(kEmptyGuardPath, bad, err));
    SR_ASSERT_FALSE(err.empty());
    // TGA 宽高是 u16：65536 不拒绝就会静默截断成 0 宽，读回来是另一张图
    bad.width = 65536; bad.height = 1; bad.rgba.clear();
    SR_ASSERT_FALSE(sr::writeTga(kEmptyGuardPath, bad, err));
    SR_ASSERT_FALSE(err.empty());

    // 校验必须发生在打开文件**之前**（bmp_writer 的同款纪律）：
    // 先放一个哨兵文件，非法调用之后它必须原样还在，而不是被截断成 0 字节。
    {
        std::ofstream f(kEmptyGuardPath, std::ios::binary);
        f << "sentinel";
    }
    bad.width = 0; bad.height = 0; bad.rgba.clear();
    SR_ASSERT_FALSE(sr::writeTga(kEmptyGuardPath, bad, err));
    std::vector<uint8_t> raw;
    SR_ASSERT(readFileBytes(kEmptyGuardPath, raw));
    if (hasBytes(raw, 8)) {
        SR_ASSERT_EQ(raw[0], 's');
        SR_ASSERT_EQ(raw[7], 'l');
    }
}

SR_TEST(anchor2_the_committed_tga_equals_the_program_checker) {
    // ---------------------------------------------------------------------
    // 锚 2（设计 §10）：committed 的 checker.tga 读回来必须**逐字节等于**
    // makeCheckerImage 在内存里生成的那张图。
    //
    // 为什么它不是"与往返测试重复"—— 往返（write→parse→compare）对**成对反转**是盲的：
    //   * 成对通道互换：writer 写 R,G,B,A、reader 也按 R,G,B,A 读 ⇒ 往返全绿；
    //   * 成对行序互换：writer 按 top-down 存却标 bit5=0、reader 把 bit5=0 当 top-down
    //     ⇒ 两次错误精确抵消，往返全绿。
    // 本条对着**独立 oracle**（内存棋盘 + 由正确代码生成后冻结在 git 里的资产）比对，
    // 上面两种成对错误在这里都会红：资产里的 B,G,R,A 与底行在前是被钉死的物理字节。
    // 三角标记色（origin 红 / uAxis 绿 / vAxis 蓝）保证竖直翻转在字节级可见 ——
    // 纯棋盘在 y 翻转下要么逐字节不变（奇数 cells）要么只是明暗互换（偶数 cells）。
    //
    // 纪律：本条变红时**不许**用 --emit-checker-tga 重新生成资产来"修"它 ——
    // 那等于把 oracle 换成被告自己的证词（重新生成参考资产需要产品方肉眼验收）。
    // ---------------------------------------------------------------------
    const sr::ImageRGBA8 expect =
        sr::makeCheckerImage(sr::kGoldenCheckerCells, sr::kGoldenCheckerCellPx, {});
    sr::ImageRGBA8 got;
    std::string err;
    const bool ok = sr::loadTgaFile(std::string(SR_SOURCE_DIR) + "/assets/tga/checker.tga",
                                    got, err);
    SR_ASSERT(ok);
    if (!ok) {
        std::printf("[anchor2] 加载失败: %s\n", err.c_str());
        return;
    }
    SR_ASSERT_EQ(got.width, sr::kGoldenCheckerCells * sr::kGoldenCheckerCellPx);
    SR_ASSERT_EQ(got.height, sr::kGoldenCheckerCells * sr::kGoldenCheckerCellPx);
    SR_ASSERT_EQ(got.width, 128);
    SR_ASSERT_EQ(got.height, 128);
    if (!hasBytes(got.rgba, expect.rgba.size())) return;
    size_t diffs = 0;
    size_t firstAt = 0;
    for (size_t i = 0; i < expect.rgba.size(); ++i) {
        if (got.rgba[i] != expect.rgba[i]) {
            if (diffs == 0) firstAt = i;
            ++diffs;
        }
    }
    if (diffs > 0)
        std::printf("[anchor2] %zu 字节不同，第一处在偏移 %zu（像素 %zu,%zu 通道 %zu）\n",
                    diffs, firstAt, firstAt / 4 / 128, firstAt / 4 % 128, firstAt % 4);
    SR_ASSERT_EQ(static_cast<long long>(diffs), 0);
    // 尺寸顺带钉住 mip 链：128x128 ⇒ 8 级（128→64→…→1）
    const std::vector<sr::ImageRGBA8> chain = sr::generateMipChain(got);
    SR_ASSERT_EQ(static_cast<int>(chain.size()), 8);
}
