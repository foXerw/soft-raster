#include "loader/tga_writer.h"

#include "loader/lex.h"   // lex::displayPath（跨 TU 共用，M5.3 评审 Important 2）

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <vector>

namespace sr {
namespace {

void putLE16(std::vector<uint8_t>& out, int v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

}  // namespace

bool writeTga(const std::filesystem::path& path, const ImageRGBA8& img, std::string& err) {
    err.clear();
    const std::string display = lex::displayPath(path);   // 共用一份，理由见 lex.h

    // ---- 校验：必须在动任何文件之前完成（bmp_writer 的同款纪律）-------------
    // 先打开文件再发现参数不对，已存在的同名资产会被截断成 0 字节 —— 而锚 2 失败时
    // 看起来像"行序回归"，实际是上一次写出把资产自己删了。
    if (img.width <= 0 || img.height <= 0) {
        err = "writeTga: 图像尺寸必须为正，得到 " + std::to_string(img.width) + "x" +
              std::to_string(img.height);
        return false;
    }
    // TGA 的宽高字段是 u16。不拒绝就会静默截断：65536 宽写出去变成 0 宽，
    // 读回来是另一张图，而写出方没有任何信号。
    if (img.width > 65535 || img.height > 65535) {
        err = "writeTga: TGA 宽高是 16 位，最大 65535，得到 " + std::to_string(img.width) +
              "x" + std::to_string(img.height);
        return false;
    }
    const size_t need = static_cast<size_t>(img.width) * static_cast<size_t>(img.height) * 4u;
    if (img.rgba.size() != need) {
        err = "writeTga: rgba 字节数与宽高不符，需要 " + std::to_string(need) + "，实际 " +
              std::to_string(img.rgba.size());
        return false;
    }

    std::vector<uint8_t> out;
    out.reserve(18 + need);

    // ---- 18 字节头 ----------------------------------------------------------
    out.push_back(0);   // idLength：无 identification field
    out.push_back(0);   // color map type：无调色板
    out.push_back(2);   // image type：未压缩 truecolor（不做 RLE，见头文件）
    for (int i = 0; i < 5; ++i) out.push_back(0);   // color map spec（无调色板，全 0）
    putLE16(out, 0);    // x origin
    putLE16(out, 0);    // y origin
    putLE16(out, img.width);
    putLE16(out, img.height);
    out.push_back(32);  // bits per pixel
    // descriptor：**bit5 = 0 ⇒ bottom-up**（故意，见头文件：锚 2 需要读回时翻一次行）。
    // 低 4 位 = 8：每像素 8 个 alpha 位。写 0 也能被我们的读侧接受，但某些看图软件
    // 会据此把 alpha 当"未定义"，32bpp 带 alpha 的诚实写法是 8。
    out.push_back(0x08);

    // ---- 像素：本文件唯一一次行序转换 + 通道转换 -----------------------------
    // ImageRGBA8 是 top-down（C8），文件要 bottom-up ⇒ y 从 height-1 递减到 0。
    // 字节序 B,G,R,A（TGA 与 BMP 同源），alpha 原样第 4 字节，不 premultiply。
    for (int y = img.height - 1; y >= 0; --y) {
        for (int x = 0; x < img.width; ++x) {
            const size_t at = (static_cast<size_t>(y) * static_cast<size_t>(img.width) +
                               static_cast<size_t>(x)) * 4u;
            out.push_back(img.rgba[at + 2]);   // B
            out.push_back(img.rgba[at + 1]);   // G
            out.push_back(img.rgba[at + 0]);   // R
            out.push_back(img.rgba[at + 3]);   // A
        }
    }

    // ofstream 的 fs::path 重载：MSVC 下走宽字符打开（D12 写侧）。
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        err = "writeTga: cannot open '" + display + "' for writing (does the directory exist?)";
        return false;
    }
    file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    file.close();
    if (!file) {
        err = "writeTga: write to '" + display + "' failed";
        return false;
    }
    return true;
}

}  // namespace sr
