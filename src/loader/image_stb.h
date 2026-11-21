#pragma once
// stb_image 的封装。**全仓库只有本模块允许碰到 third_party/stb_image.h**（设计文档 C2），
// 而且 soft_raster_core 不得 include 本头 —— core 不知道存在一个解码器。
// C2 的"唯一一处第三方"是可以机器检查的：
//   grep -rn "stb_image" src/ 只应命中 src/loader/image_stb.*
#include <string>

#include "render/texture.h"   // ImageRGBA8

namespace sr {

// 解码一个图片文件（PNG / JPG / BMP / TGA / PSD / GIF），输出统一成 RGBA8、**top-down**。
// 失败（文件不存在、格式不认识、尺寸非法）返回 false、err 非空，且 out 保持为空 ——
// 半张图比没有图更糟：它会画出一块看起来像 bug 的垃圾，而真正的错在 IO。
bool loadImageFile(const std::string& path, ImageRGBA8& out, std::string& err);

}  // namespace sr
