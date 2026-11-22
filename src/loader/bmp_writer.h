#pragma once

#include <filesystem>
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
//
// path 用 std::filesystem::path（D12 写侧对称，与 writeTga 同形）：std::string 隐式
// 转换，既有调用点源码兼容；MSVC 的 ofstream(fs::path) 走宽字符打开，中文目录能写。
// M5.6 起是必需而非锦上添花：argv 桥（D18）之后所有路径选项都是 UTF-8 语义，
// 收窄字符串会被按 ANSI 码页误解码 —— 桥修好了 --obj 却弄坏中文 --out 等于没修。
bool writeBmp(const std::filesystem::path& path, const ColorTarget& target, std::string& err);

}  // namespace sr
