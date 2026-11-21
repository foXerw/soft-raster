#pragma once
// 手写 TGA 加载器。保留它的两个理由（spec §7.4）：它是"图像就是字节数组"的最低成本样本，
// 也是 stb 缺席时的回退路径。
//
// 支持：type 2（未压缩 truecolor 24/32bpp）、type 3（8bit 灰度）、type 10（RLE truecolor）。
// 拒绝：调色板型（1 与 9 —— 只有这两种是调色板）、type 11（RLE 灰度，未实现，不是调色板）、
//       以及 16bpp —— 返回 false 并说清原因，比画出一张错色的图诚实。
// （type 11 原先被写在调色板那一串里，是标签写错：拿一个合法的 type-11 文件来的人会
//  收到一句关于调色板的解释，然后去改一个本来没错的文件头。）
//
// 解析函数**不做 IO**（与 M5 计划里 parseObj 同一条理由）：给定字节缓冲就能单测，
// 包括"文件被截断"这种用真文件很难稳定构造的输入。
#include <cstddef>
#include <cstdint>
#include <string>

#include "render/texture.h"   // ImageRGBA8

namespace sr {

// 像素数据在**理论上最少**需要多少字节，以及 available 里放不放得下。
// 抽成纯函数的唯一理由：**分配必须发生在检查之后**。RLE 的编码长度与内容无关
// （最省的编码是每包 128 个像素、每包 1 + bytesPP 字节），所以在读任何一个包之前
// 就能算出下界；拿这个下界先问一次，一个 18 字节的头就换不走一次 16 GiB 的分配。
// 未压缩分支的下界是精确值 w*h*bytesPP。
// w/h/bytesPP 任一 <= 0 ⇒ false（非法输入不是"随便给个数"）。
bool tgaPayloadFits(int w, int h, int bytesPP, bool rle, size_t available);

// 从内存解析。失败返回 false、err 非空，out 保持为空。绝不读越过 size 的字节。
bool parseTga(const uint8_t* data, size_t size, ImageRGBA8& out, std::string& err);

// 读文件再解析。文件打不开时 err 里带上路径。
bool loadTgaFile(const std::string& path, ImageRGBA8& out, std::string& err);

}  // namespace sr
