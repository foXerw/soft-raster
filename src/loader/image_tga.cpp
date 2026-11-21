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

bool tgaPayloadFits(int w, int h, int bytesPP, bool rle, size_t available) {
    if (w <= 0 || h <= 0 || bytesPP <= 0) return false;
    const size_t pixelCount = static_cast<size_t>(w) * static_cast<size_t>(h);
    const size_t bpp = static_cast<size_t>(bytesPP);
    // 两处都用**除法**问，不用乘法算出"需要多少字节"再比较：
    // 65535x65535x4 在 32 位 size_t 上会回绕成小数，于是"放不下"被判成"放得下" ——
    // 而那正是这个守卫唯一要防的事。
    if (!rle) return pixelCount <= available / bpp;
    // RLE 的下界：每包最多 128 个像素 ⇒ 包数 >= ceil(pixelCount/128)；
    // 每包最少 1 字节包头 + 1 个像素（run 包）= 1 + bpp 字节。
    const size_t packets = (pixelCount + 127u) / 128u;
    return available / (1u + bpp) >= packets;
}

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
    // **先问再分配**（两条分支都问）。以前只有未压缩分支检查，于是 RLE 分支会先
    // `assign(pixelCount*4)` 再决定拒绝：一个 18 字节的头声称 65535x65535 就能换走
    // 16 GiB 的分配请求（终审抓到的；M5 要载入网上下载的图片，那时这是真的攻击面）。
    if (!tgaPayloadFits(w, h, bytesPP, rle, size - at)) {
        // 消息里的数字只做展示，所以允许饱和；判断本身在上面用除法做，不会回绕。
        size_t need = 0;
        if (rle) {
            need = ((pixelCount + 127u) / 128u) * (1u + static_cast<size_t>(bytesPP));
        } else if (pixelCount > SIZE_MAX / static_cast<size_t>(bytesPP)) {
            need = SIZE_MAX;
        } else {
            need = pixelCount * static_cast<size_t>(bytesPP);
        }
        err = fail("", "像素数据不足：至少需要 " + std::to_string(need) + " 字节，只有 " +
                           std::to_string(size - at));
        return false;
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
