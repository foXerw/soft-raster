#pragma once

#include <string>

#include "render/render_target.h"

namespace sr {

// 把一块颜色目标写成 24bpp 无压缩 BMP。
//
// 为什么是 BMP 而不是 PNG：PNG 编码要 deflate，那是渲染之外的一整块工作（设计文档 C2/N5）。
// 截图与 golden 回归只需要"字节进、字节出、人眼可看"，BMP 三样都够。
//
// 方向：core 的目标是 top-down（C8），BMP 的行是自底向上（height 取正值），
// 因此这里做**一次**行序转换。窗口那边不做（用负 biHeight），所以"翻转点"总共有两处，
// 各自负责自己的格式，互不叠加 —— 见 docs/notes/M0.md 里那张方向表。
bool writeBmp(const std::string& path, const ColorTarget& target, std::string& err);

}  // namespace sr
