#include "loader/bmp_writer.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "harness.h"
#include "render/color.h"
#include "render/render_target.h"

namespace {

std::string gTmpDir = "sr_tmp_bmp";

// Fixture 必须由**调用方持有**，不能按值返回：RenderTarget 里存的是指向 colorMem/depthMem
// 的裸指针，而返回一个"内部指针指向自己成员"的对象会在返回那一刻把指针变成悬垂。
// （第一版就是这么写的，症状是测试进程直接挂住 —— 这就是那种"看起来像渲染 bug"的内存错误。）
struct Fixture {
    std::vector<uint8_t> colorMem;
    std::vector<float> depthMem;
    sr::RenderTarget rt;
    std::string path;
};

void fillFixture(Fixture& f, int w, int h, const std::string& name) {
    f.colorMem.assign(static_cast<size_t>(w) * h * 4, 0);
    f.depthMem.assign(static_cast<size_t>(w) * h, 1.0f);
    f.rt.color = sr::ColorTarget{reinterpret_cast<uint32_t*>(f.colorMem.data()), w, h, w * 4};
    f.rt.depth = sr::DepthTarget{f.depthMem.data(), w, h};
    f.path = gTmpDir + "/" + name;
}

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

int32_t le32(const std::vector<uint8_t>& b, size_t at) {
    return static_cast<int32_t>(static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) |
                                (static_cast<uint32_t>(b[at + 2]) << 16) |
                                (static_cast<uint32_t>(b[at + 3]) << 24));
}
int16_t le16(const std::vector<uint8_t>& b, size_t at) {
    return static_cast<int16_t>(static_cast<uint16_t>(b[at]) | (static_cast<uint16_t>(b[at + 1]) << 8));
}

void removeTmp(const std::string& path) { std::remove(path.c_str()); }

}  // namespace

SR_TEST(bmp_header_fields) {
    Fixture f;
    fillFixture(f, 5, 3, "header.bmp");
    f.rt.color.clear(0xFF112233u);
    std::string err;
    SR_ASSERT(sr::writeBmp(f.path, f.rt.color, err));

    // 每行 5*3 = 15 字节，补到 4 的倍数 ⇒ 16；3 行 ⇒ 48；加 54 字节头 ⇒ 102。
    const auto b = readFile(f.path);
    SR_ASSERT_EQ(b.size(), 102u);
    if (b.size() < 102) return;   // 长度不对就别再往下读，否则是在空 vector 上取元素（UB）
    SR_ASSERT_EQ(b[0], 'B');
    SR_ASSERT_EQ(b[1], 'M');
    SR_ASSERT_EQ(le32(b, 2), 102);      // 文件总长
    SR_ASSERT_EQ(le32(b, 10), 54);      // 像素数据起点
    SR_ASSERT_EQ(le32(b, 14), 40);      // BITMAPINFOHEADER 大小
    SR_ASSERT_EQ(le32(b, 18), 5);       // width
    SR_ASSERT_EQ(le32(b, 22), 3);       // height，正值 ⇒ 自底向上
    SR_ASSERT_EQ(le16(b, 26), 1);       // planes
    SR_ASSERT_EQ(le16(b, 28), 24);      // bpp
    SR_ASSERT_EQ(le32(b, 30), 0);       // BI_RGB，无压缩
    removeTmp(f.path);
}

SR_TEST(bmp_first_data_row_is_the_bottom_edge) {
    // Review Focus 5 的第二处翻转：core 是 top-down（C8），BMP 是自底向上。
    // 两个方向必须在写文件这一次转换里对上，否则窗口与 golden 会互相上下颠倒。
    Fixture f;
    fillFixture(f, 2, 2, "flip.bmp");
    f.rt.color.writePacked(0, 0, sr::encodeSRGB({1.0f, 0.0f, 0.0f}));   // 上边缘 = 红
    f.rt.color.writePacked(1, 0, sr::encodeSRGB({1.0f, 0.0f, 0.0f}));
    f.rt.color.writePacked(0, 1, sr::encodeSRGB({0.0f, 0.0f, 1.0f}));   // 下边缘 = 蓝
    f.rt.color.writePacked(1, 1, sr::encodeSRGB({0.0f, 0.0f, 1.0f}));
    std::string err;
    SR_ASSERT(sr::writeBmp(f.path, f.rt.color, err));

    const auto b = readFile(f.path);
    // 2x2 的几何：每行 2*3 = 6 字节，补到 8；两行 ⇒ 16；加 54 字节头 ⇒ 70。
    // （计划里的期望值把"每行补齐 8"误写成了"整个数据段 8"，实施时更正，见 ledger Ruling 10。）
    SR_ASSERT_EQ(b.size(), 70u);
    if (b.size() < 70u) return;
    const size_t fileRowBottom = 54;   // 文件第一行数据 == 图像最下边
    const size_t fileRowTop = 54 + 8;  // 第二行 == 图像上边

    // 下边缘 = 蓝：packed 0xFF0000FF ⇒ 字节 B,G,R = FF,00,00
    SR_ASSERT_EQ(b[fileRowBottom + 0], 0xFF);
    SR_ASSERT_EQ(b[fileRowBottom + 1], 0x00);
    SR_ASSERT_EQ(b[fileRowBottom + 2], 0x00);
    SR_ASSERT_EQ(b[fileRowBottom + 3], 0xFF);  // 同一行的第二个像素
    // 上边缘 = 红：packed 0xFFFF0000 ⇒ 字节 B,G,R = 00,00,FF
    SR_ASSERT_EQ(b[fileRowTop + 0], 0x00);
    SR_ASSERT_EQ(b[fileRowTop + 1], 0x00);
    SR_ASSERT_EQ(b[fileRowTop + 2], 0xFF);
    SR_ASSERT_EQ(b[fileRowTop + 5], 0xFF);
    // 每行尾的两个 padding 字节必须是 0
    SR_ASSERT_EQ(b[fileRowBottom + 6], 0);
    SR_ASSERT_EQ(b[fileRowBottom + 7], 0);
    SR_ASSERT_EQ(b[fileRowTop + 6], 0);
    SR_ASSERT_EQ(b[fileRowTop + 7], 0);
    removeTmp(f.path);
}

SR_TEST(bmp_pixel_bytes_match_the_source_target_channel_by_channel) {
    Fixture f;
    fillFixture(f, 3, 2, "roundtrip.bmp");
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 3; ++x)
            f.rt.color.writePacked(x, y, sr::packBGRA(static_cast<uint8_t>(0x10 + x), static_cast<uint8_t>(0x20 + y),
                                          static_cast<uint8_t>(0x30 + x * 3 + y), 0xFF));
    std::string err;
    SR_ASSERT(sr::writeBmp(f.path, f.rt.color, err));

    const auto b = readFile(f.path);
    if (b.size() < 54u + 24u) return;
    // 每行 3 像素 * 3 = 9 字节 ⇒ 补到 12
    for (int y = 0; y < 2; ++y) {
        const int fileRow = 1 - y;  // 自底向上
        for (int x = 0; x < 3; ++x) {
            const size_t at = 54 + static_cast<size_t>(fileRow) * 12 + static_cast<size_t>(x) * 3;
            const uint32_t src = f.rt.color.readPacked(x, y);
            SR_ASSERT_EQ(b[at], src & 0xFFu);                // B
            SR_ASSERT_EQ(b[at + 1], (src >> 8) & 0xFFu);     // G
            SR_ASSERT_EQ(b[at + 2], (src >> 16) & 0xFFu);    // R
        }
    }
    removeTmp(f.path);
}

SR_TEST(bmp_rejects_a_target_whose_pitch_is_not_width_times_4) {
    // 截图路径刻意不支持带额外 stride 的目标：DIB 的 pitch 恰好等于 width*4（负 biHeight 的
    // 32bpp 不会有额外对齐要求），而支持任意 pitch 要引入一次整帧重排，那是 M10 的事。
    std::vector<uint8_t> mem(64 * 4 + 16);
    sr::ColorTarget t{reinterpret_cast<uint32_t*>(mem.data()), 16, 1, 64 + 16};
    std::string err;
    SR_ASSERT_FALSE(sr::writeBmp(gTmpDir + "/rejected.bmp", t, err));
    SR_ASSERT_FALSE(err.empty());
}

SR_TEST(bmp_rejects_an_empty_target) {
    std::vector<uint8_t> mem(4, 0);
    sr::ColorTarget t{reinterpret_cast<uint32_t*>(mem.data()), 0, 0, 0};
    std::string err;
    SR_ASSERT_FALSE(sr::writeBmp(gTmpDir + "/empty.bmp", t, err));
    SR_ASSERT(err.find("width") != std::string::npos);
}

SR_TEST(bmp_reports_a_failure_when_the_directory_does_not_exist) {
    Fixture f;
    fillFixture(f, 2, 2, "missing_dir.bmp");
    f.path = "this_directory_does_not_exist_zzz/out.bmp";
    std::string err;
    SR_ASSERT_FALSE(sr::writeBmp(f.path, f.rt.color, err));
    SR_ASSERT_FALSE(err.empty());
}
