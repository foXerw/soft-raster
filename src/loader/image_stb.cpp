#include "loader/image_stb.h"

#include <cstddef>
#include <cstdint>

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

bool loadImageFile(const std::string& path, ImageRGBA8& out, std::string& err) {
    out = ImageRGBA8{};
    err.clear();

    int w = 0, h = 0, sourceChannels = 0;
    // 显式不要竖直翻转：stb 默认输出的第一行就是图像上边缘，与我们的 C8 约定一致。
    // 写出来而不是依赖默认值，是因为"谁翻了一次行"这个问题在 BMP/DIB/PNG 三处都出现过。
    stbi_set_flip_vertically_on_load(0);
    // 请求 4 通道 = 让 stb 替我们把灰度、调色板、16bit 一律归一到 8bit RGBA
    // （灰度复制到 r/g/b、调色板展开、16bit 取高位）。计划里"M4 必须回答"的那条，
    // 答案是用它的 request 参数而不是自己再写一遍位深转换。
    uint8_t* data = static_cast<uint8_t*>(
        stbi_load(path.c_str(), &w, &h, &sourceChannels, 4));
    if (data == nullptr) {
        err = path + ": 无法解码 (" + stbi_failure_reason() + ")";
        return false;
    }
    if (w <= 0 || h <= 0) {
        stbi_image_free(data);
        err = path + ": 尺寸非法";
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
