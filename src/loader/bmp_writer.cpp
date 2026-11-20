#include "loader/bmp_writer.h"

#include <cstdint>
#include <fstream>
#include <vector>

namespace sr {
namespace {

constexpr uint32_t kHeaderSize = 54;
constexpr uint32_t kInfoHeaderSize = 40;
constexpr uint32_t kBitsPerPixel = 24;
// 96 DPI。BMP 的分辨率字段只影响"打印时多大"，对我们毫无意义，但填 0 会让某些
// 看图软件把图片当成损坏，所以按最常见的 96 写。
constexpr int32_t kPixelsPerMeter = 2835;

inline void putLE16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

inline void putLE32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

inline uint8_t byteOf(uint32_t packed, int shift) {
    return static_cast<uint8_t>((packed >> shift) & 0xFFu);
}

}  // namespace

bool writeBmp(const std::string& path, const ColorTarget& target, std::string& err) {
    err.clear();

    // ---- 校验：必须在动任何文件之前完成 -----------------------------------
    // 顺序是接口的一部分：先校验、再在内存里拼完整帧、最后才打开文件。
    // 若先打开文件再发现参数不对，已存在的同名截图会被截断成 0 字节 ——
    // 而 golden 比对失败时看起来像"渲染回归"，实际是上一次截图把自己删了。
    if (target.width <= 0) {
        err = "writeBmp: target width must be > 0";
        return false;
    }
    if (target.height <= 0) {
        err = "writeBmp: target height must be > 0";
        return false;
    }
    if (target.pitch != target.width * 4) {
        err = "writeBmp: only pitch == width*4 is supported (screenshots come from a DIB)";
        return false;
    }

    const uint32_t rowBytes = static_cast<uint32_t>(target.width) * 3u;
    const uint32_t stride = (rowBytes + 3u) & ~3u;  // 每行补到 4 的倍数
    const uint32_t padding = stride - rowBytes;
    const uint32_t imageSize = stride * static_cast<uint32_t>(target.height);
    const uint32_t fileSize = kHeaderSize + imageSize;

    std::vector<uint8_t> out;
    out.reserve(fileSize);

    // ---- BITMAPFILEHEADER (14) ----
    out.push_back('B');
    out.push_back('M');
    putLE32(out, fileSize);
    putLE32(out, 0);  // reserved
    putLE32(out, kHeaderSize);

    // ---- BITMAPINFOHEADER (40) ----
    putLE32(out, kInfoHeaderSize);
    putLE32(out, static_cast<uint32_t>(target.width));
    putLE32(out, static_cast<uint32_t>(target.height));  // 正值 ⇒ 行自底向上
    putLE16(out, 1);                                     // planes
    putLE16(out, kBitsPerPixel);
    putLE32(out, 0);                                     // BI_RGB
    putLE32(out, imageSize);
    putLE32(out, static_cast<uint32_t>(kPixelsPerMeter));
    putLE32(out, static_cast<uint32_t>(kPixelsPerMeter));
    putLE32(out, 0);  // biClrUsed
    putLE32(out, 0);  // biClrImportant

    // ---- 像素：这里做本文件唯一一次行序转换 ----
    // core 的目标是 top-down（C8），BMP 自底向上，所以 y 从 height-1 递减到 0。
    // alpha 被丢弃：24bpp 没有 alpha 通道，截图与 golden 都只关心颜色。
    const uint32_t* src = target.pixels;
    const uint32_t rowStrideWords = static_cast<uint32_t>(target.pitch) / 4u;
    for (int y = target.height - 1; y >= 0; --y) {
        const uint32_t* row = src + static_cast<size_t>(y) * rowStrideWords;
        for (int x = 0; x < target.width; ++x) {
            const uint32_t packed = row[x];
            // packBGRA 的内存字节序是 B,G,R,A，所以文件里依次写 B、G、R。
            out.push_back(byteOf(packed, 0));   // B
            out.push_back(byteOf(packed, 8));   // G
            out.push_back(byteOf(packed, 16));  // R
        }
        for (uint32_t p = 0; p < padding; ++p) out.push_back(0);
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        err = "writeBmp: cannot open '" + path + "' for writing (does the directory exist?)";
        return false;
    }
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    file.close();
    if (!file) {
        err = "writeBmp: write to '" + path + "' failed";
        return false;
    }
    return true;
}

}  // namespace sr
