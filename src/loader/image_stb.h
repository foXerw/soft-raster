#pragma once
// stb_image 的封装。**全仓库只有本模块允许碰到 third_party/stb_image.h**（设计文档 C2），
// 而且 soft_raster_core 不得 include 本头 —— core 不知道存在一个解码器。
// C2 的"唯一一处第三方"是可以机器检查的：
//   grep -rn "stb_image" src/ 只应命中 src/loader/image_stb.*
#include <filesystem>
#include <string>

#include "render/texture.h"   // ImageRGBA8 / kMaxTextureSide

namespace sr {

// 解码一个图片文件（PNG / JPG / BMP / TGA / PSD / GIF），输出统一成 RGBA8、**top-down**。
// 失败（文件不存在、格式不认识、尺寸非法、任一边超过 kMaxTextureSide）返回 false、
// err 非空，且 out 保持为空 —— 半张图比没有图更糟：它会画出一块看起来像 bug 的垃圾，
// 而真正的错在 IO。
//
// path 用 std::filesystem::path（D12）：std::string 隐式转换，既有调用点源码兼容。
// 实现自己开 FILE（MSVC 下 _wfopen）再交给解码器 —— 窄字符 fopen 在 MSVC 上是
// ANSI 代码页，中文目录名会被误解码成"打不开文件"（G-M5.5）。
// 尺寸检查发生在解码/分配**之前**（D13，先问后分配，与 tgaPayloadFits 同一条纪律）。
bool loadImageFile(const std::filesystem::path& path, ImageRGBA8& out, std::string& err);

}  // namespace sr
