#pragma once
// 手写 TGA 加载器。保留它的两个理由（spec §7.4）：它是"图像就是字节数组"的最低成本样本，
// 也是 stb 缺席时的回退路径。
//
// 支持：type 2（未压缩 truecolor 24/32bpp）、type 3（8bit 灰度）、type 10（RLE truecolor）。
// 拒绝：调色板型（1/9/11）与 16bpp —— 返回 false 并说清原因，比画出一张错色的图诚实。
//
// 解析函数**不做 IO**（与 M5 计划里 parseObj 同一条理由）：给定字节缓冲就能单测，
// 包括"文件被截断"这种用真文件很难稳定构造的输入。
#include <cstddef>
#include <cstdint>
#include <string>

#include "render/texture.h"   // ImageRGBA8

namespace sr {

// 从内存解析。失败返回 false、err 非空，out 保持为空。绝不读越过 size 的字节。
bool parseTga(const uint8_t* data, size_t size, ImageRGBA8& out, std::string& err);

// 读文件再解析。文件打不开时 err 里带上路径。
bool loadTgaFile(const std::string& path, ImageRGBA8& out, std::string& err);

}  // namespace sr
