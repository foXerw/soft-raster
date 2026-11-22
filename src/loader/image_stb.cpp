#include "loader/image_stb.h"

#include "loader/lex.h"   // lex::displayPath（跨 TU 共用，M5.3 评审 Important 2）

#include <cstddef>
#include <cstdint>
#include <cstdio>

// 第三方头在自己的翻译单元里关掉警告（设计文档要求构建输出零警告，为的是"新警告一眼可见"；
// 一个 7 千行的 vendored 解码器在 /W4 + -Wall -Wextra 下会产出几十条我们无法上游修复的噪声，
// 那反而会埋掉真正属于本项目的新警告）。push,0 只影响这个 include，不影响本文件其余代码。
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif

// STB_IMAGE_IMPLEMENTATION 只允许出现在**这一个**翻译单元：两处定义 = 重复符号。
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace sr {

bool loadImageFile(const std::filesystem::path& path, ImageRGBA8& out, std::string& err) {
    out = ImageRGBA8{};
    err.clear();
    const std::string display = lex::displayPath(path);   // 共用一份，理由见 lex.h

    // D12：FILE 由我们自己开。解码器的窄字符 fopen 在 MSVC 上是 ANSI 代码页，
    // 中文目录名会被误解码成"打不开文件"（G-M5.5）。_wfopen 是 CRT（<cstdio>），
    // 不是 Win32 —— 这里不出现 <windows.h>（C4）。
#if defined(_MSC_VER)
    // _wfopen_s 而不是 _wfopen：后者在 /W4 下触发 C4996（零警告是硬约束）。
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0) f = nullptr;
#elif defined(_WIN32)
    // MinGW/MSYS2 的 libstdc++ 里 path::value_type 同样是 wchar_t（Windows 风味），
    // 所以"非 MSVC 就走窄 fopen"在这台机器的 g++ 侧编译不过。_wfopen 是 CRT，
    // MinGW 也提供；这里没有 /W4 的 C4996 问题。
    FILE* f = _wfopen(path.c_str(), L"rb");
#else
    FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (f == nullptr) {
        err = display + ": 打不开文件";
        return false;
    }

    // D13：**先问后分配**。只读头部拿宽高（不解码、不分配像素缓冲），
    // 超过 kMaxTextureSide 直接拒绝 —— 与 tgaPayloadFits 同一条纪律：
    // 一个 18 字节的头不该换走一次 8192x8192x4 = 256 MiB 的分配。
    int w = 0, h = 0, sourceChannels = 0;
    if (!stbi_info_from_file(f, &w, &h, &sourceChannels)) {
        std::fclose(f);
        err = display + ": 无法识别图片头 (" + stbi_failure_reason() + ")";
        return false;
    }
    if (w > kMaxTextureSide || h > kMaxTextureSide) {
        std::fclose(f);
        err = display + ": 尺寸 " + std::to_string(w) + "x" + std::to_string(h) +
              " 超过 kMaxTextureSide=" + std::to_string(kMaxTextureSide) +
              "（先问后分配：拒绝发生在解码之前）";
        return false;
    }
    std::rewind(f);   // 头探测只消费了头部字节，解码从头开始

    // 显式不要竖直翻转：stb 默认输出的第一行就是图像上边缘，与我们的 C8 约定一致。
    // 写出来而不是依赖默认值，是因为"谁翻了一次行"这个问题在 BMP/DIB/PNG 三处都出现过。
    stbi_set_flip_vertically_on_load(0);
    // 请求 4 通道 = 让 stb 替我们把灰度、调色板、16bit 一律归一到 8bit RGBA
    // （灰度复制到 r/g/b、调色板展开、16bit 取高位）。计划里"M4 必须回答"的那条，
    // 答案是用它的 request 参数而不是自己再写一遍位深转换。
    uint8_t* data = static_cast<uint8_t*>(
        stbi_load_from_file(f, &w, &h, &sourceChannels, 4));
    std::fclose(f);
    if (data == nullptr) {
        err = display + ": 无法解码 (" + stbi_failure_reason() + ")";
        return false;
    }
    if (w <= 0 || h <= 0) {
        stbi_image_free(data);
        err = display + ": 尺寸非法";
        return false;
    }
    out.width = w;
    out.height = h;
    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4u;
    out.rgba.assign(data, data + bytes);
    stbi_image_free(data);
    return true;
}

}  // namespace sr
