#pragma once
// 手写 TGA 写出器（M5.3，资产生成工具，不是模型导出 —— 设计 §3 的 OBJ 只读不写不受影响）。
// 与 bmp_writer 同侧进 soft_raster_core：它只认识 ImageRGBA8 的字节，不知道任何第三方解码器。
//
// 格式：未压缩 type 2、32bpp、18 字节头（无 identification field、无 XFILE 脚注）、
// alpha 原样存第 4 字节。**不做 RLE**（YAGNI：资产由本函数生成，读侧支持 RLE 是为了
// 兼容外部导出器，写侧没有这个需求）。
//
// 方向：core 的 ImageRGBA8 是 top-down（C8），这里**故意写成 bottom-up（descriptor
// bit5 = 0，TGA 的历史默认）**。这不是疏忽而是设计 §10 锚 2 的前提：读回这样一个文件
// 必然要走一次行翻转，"TGA 行序翻译"那条路径才有东西可守。若写成 top-down + bit5=1，
// 读回不翻转，锚 2 就退化成一次无翻转的 memcpy。
#include <filesystem>
#include <string>

#include "render/texture.h"   // ImageRGBA8

namespace sr {

// 失败返回 false、err 非空。校验（尺寸为正、u16 范围内、字节数与宽高一致）发生在
// 打开文件**之前**：先打开再校验会把已存在的同名文件截断成 0 字节（bmp_writer 同款纪律）。
//
// path 用 std::filesystem::path（D12 的写侧对称，控制者追加裁决）：std::string 隐式
// 转换，既有调用点源码兼容；MSVC 的 ofstream(fs::path) 走宽字符打开，中文目录能写。
// "读贴图支持中文、写 TGA 不支持"的不对称会在下一轮被复制（M5.6 nativePath），故补齐。
bool writeTga(const std::filesystem::path& path, const ImageRGBA8& img, std::string& err);

}  // namespace sr
