#include "loader/image_tga.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <vector>

namespace sr {
namespace {

// 每个字段都过一次长度检查：加载器读的是别人给的文件，"截断"是常态而不是意外。
bool readU16LE(const uint8_t* d, size_t size, size_t at, int& out) {
    if (at + 2 > size) return false;
    out = static_cast<int>(d[at]) | (static_cast<int>(d[at + 1]) << 8);
    return true;
}

std::string fail(const std::string& path, const std::string& why) {
    return (path.empty() ? std::string("tga: ") : path + ": tga: ") + why;
}

}  // namespace

bool parseTga(const uint8_t* data, size_t size, ImageRGBA8& out, std::string& err) {
    out = ImageRGBA8{};
    err.clear();
    if (data == nullptr || size < 18) {
        err = fail("", "头部不足 18 字节");
        return false;
    }
    const size_t idLength = data[0];
    const int cmapType = data[1];
    const int imageType = data[2];
    int w = 0, h = 0;
    if (!readU16LE(data, size, 12, w) || !readU16LE(data, size, 14, h)) {
        err = fail("", "读不到宽高");
        return false;
    }
    const int bpp = data[16];
    const int descriptor = data[17];

    if (w <= 0 || h <= 0) {
        err = fail("", "尺寸非法 " + std::to_string(w) + "x" + std::to_string(h));
        return false;
    }
    if (cmapType != 0 || imageType == 1 || imageType == 9) {
        err = fail("", "不支持调色板型 TGA（imageType=" + std::to_string(imageType) + "）");
        return false;
    }
    // 只支持真彩色与灰度。16bpp（5-5-5）刻意不做：它的位域布局有 A1RGB5 / R5G5B5 两种变体，
    // 猜错会画出偏色的图，而"看起来只是颜色不太对"是最难被发现的一类错。
    const bool gray = (imageType == 3);
    const int bytesPP = gray ? 1 : (bpp == 24 ? 3 : (bpp == 32 ? 4 : 0));
    if (bytesPP == 0) {
        err = fail("", "只支持 24/32bpp 真彩色与 8bit 灰度，得到 imageType=" +
                           std::to_string(imageType) + " bpp=" + std::to_string(bpp));
        return false;
    }
    const bool rle = (imageType == 10);
    if (imageType != 2 && !rle && !gray) {
        err = fail("", "不认识的 imageType=" + std::to_string(imageType));
        return false;
    }

    size_t at = 18 + idLength;                        // 跳过 identification field
    if (at > size || at < 18) {
        err = fail("", "缺 identification field（文件被截断）");
        return false;
    }

    const size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
    if (!rle) {                                        // 未压缩：整块像素数据必须在文件里
        if (pixelCount > (size - at) / static_cast<size_t>(bytesPP)) {
            err = fail("", "像素数据不足：需要 " + std::to_string(pixelCount * bytesPP) +
                               " 字节，只有 " + std::to_string(size - at));
            return false;
        }
    }

    out.width = w;
    out.height = h;
    out.rgba.assign(pixelCount * 4u, 0);

    // bit5 = 1 ⇒ 文件存的就是 top-down；否则第一行是图像**底**行（TGA 的默认）。
    const bool storedTopDown = (descriptor & 0x20) != 0;
    auto put = [&](size_t idx, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        const int row = static_cast<int>(idx / static_cast<size_t>(w));
        const int col = static_cast<int>(idx % static_cast<size_t>(w));
        const int dstRow = storedTopDown ? row : (h - 1 - row);
        const size_t o = (static_cast<size_t>(dstRow) * static_cast<size_t>(w) +
                          static_cast<size_t>(col)) * 4u;
        out.rgba[o] = r;
        out.rgba[o + 1] = g;
        out.rgba[o + 2] = b;
        out.rgba[o + 3] = a;
    };

    // 读一个像素到 px（B,G,R[,A] → R,G,B,A）。越界即失败 —— 未压缩分支在上面已经整体
    // 检查过长度，这里再查一次是为了让两条分支都不依赖对方的前提。
    auto readPixel = [&](uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) -> bool {
        if (at + static_cast<size_t>(bytesPP) > size) return false;
        if (gray) {
            r = g = b = data[at];
            a = 255;
        } else {
            r = data[at + 2];
            g = data[at + 1];
            b = data[at + 0];
            a = (bytesPP == 4) ? data[at + 3] : 255;
        }
        at += static_cast<size_t>(bytesPP);
        return true;
    };

    if (!rle) {
        for (size_t idx = 0; idx < pixelCount; ++idx) {
            uint8_t r = 0, g = 0, b = 0, a = 255;
            if (!readPixel(r, g, b, a)) {              // 上面的整体检查让它几乎不可达，留着兜底
                err = fail("", "像素数据被截断");
                out = ImageRGBA8{};
                return false;
            }
            put(idx, r, g, b, a);
        }
        return true;
    }

    size_t done = 0;
    while (done < pixelCount) {
        if (at >= size) {
            err = fail("", "RLE 包头被截断");
            out = ImageRGBA8{};
            return false;
        }
        const uint8_t packet = data[at++];
        const int count = (packet & 0x7F) + 1;         // 低 7 位是"含本像素共 count 个"
        const bool run = (packet & 0x80) != 0;         // 高位：run=重复一份，raw=原样跟 count 个
        if (done + static_cast<size_t>(count) > pixelCount) {
            err = fail("", "RLE 包超出图像尺寸（文件损坏）");
            out = ImageRGBA8{};
            return false;
        }
        uint8_t r = 0, g = 0, b = 0, a = 255;
        for (int i = 0; i < count; ++i) {
            if (!run || i == 0) {                      // run 包只有一份像素数据
                if (!readPixel(r, g, b, a)) {
                    err = fail("", "RLE 像素数据被截断");
                    out = ImageRGBA8{};
                    return false;
                }
            }
            put(done + static_cast<size_t>(i), r, g, b, a);
        }
        done += static_cast<size_t>(count);
    }
    return true;
}

bool loadTgaFile(const std::string& path, ImageRGBA8& out, std::string& err) {
    out = ImageRGBA8{};
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        err = path + ": 打不开文件";
        return false;
    }
    const std::streamoff len = f.tellg();
    if (len <= 0) {
        err = path + ": 文件为空";
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(len));
    f.seekg(0, std::ios::beg);
    if (!f.read(reinterpret_cast<char*>(bytes.data()), len)) {
        err = path + ": 读取失败";
        return false;
    }
    if (!parseTga(bytes.data(), bytes.size(), out, err)) {
        err = path + ": " + err;                       // 带上是哪个文件
        out = ImageRGBA8{};
        return false;
    }
    return true;
}

}  // namespace sr
