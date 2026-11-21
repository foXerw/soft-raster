#pragma once
// 测试专用的 24bpp BMP 读回。刻意放在 tests/ 而不是 loader/：
// 渲染器只需要写 BMP，读回来验证是"回归框架"的职责，不是产品的。
// 写一个 PNG 解码器才有意义地省空间，但那是渲染之外的一大块（设计文档 C2）。
#include <cstdint>
#include <fstream>
#include <vector>

namespace srtest {

inline bool readBmp24(const std::string& path, int& width, int& height,
                      std::vector<uint32_t>& pixels, std::string& err) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { err = "打不开 " + path; return false; }
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (b.size() < 54 || b[0] != 'B' || b[1] != 'M') { err = "不是 BMP 或太短"; return false; }
    const auto le32 = [&b](size_t at) {
        return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) |
               (static_cast<uint32_t>(b[at + 2]) << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
    };
    width = static_cast<int>(le32(18));
    const int32_t h = static_cast<int32_t>(le32(22));
    const bool bottomUp = h > 0;
    height = bottomUp ? h : -h;
    if (le32(30) != 0 || b[28] != 24) { err = "只支持 BI_RGB 24bpp"; return false; }
    const size_t dataAt = le32(10);
    const int stride = (width * 3 + 3) & ~3;
    if (b.size() < dataAt + static_cast<size_t>(stride) * height) { err = "数据段不完整"; return false; }

    pixels.assign(static_cast<size_t>(width) * height, 0u);
    for (int y = 0; y < height; ++y) {
        const int fileRow = bottomUp ? (height - 1 - y) : y;   // 这里做唯一一次行序还原
        for (int x = 0; x < width; ++x) {
            const size_t at = dataAt + static_cast<size_t>(fileRow) * stride + static_cast<size_t>(x) * 3;
            pixels[static_cast<size_t>(y) * width + x] = 0xFF000000u |
                (static_cast<uint32_t>(b[at + 2]) << 16) | (static_cast<uint32_t>(b[at + 1]) << 8) |
                static_cast<uint32_t>(b[at]);
        }
    }
    return true;
}

}  // namespace srtest
