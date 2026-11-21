#pragma once

#include <cstdint>
#include <string>

#include "render/render_target.h"

namespace sr {

// 8x8 位图字体：HUD 的文字由我们自己画，不用 GDI 的 TextOut。
// 理由有两条：(1) 设计文档 §10.3 —— 屏幕上不该出现一个不是我们算出来的像素；
// (2) 实用 —— 文字要能在离屏渲染与 golden 里出现，而 GDI 字体只在有 DC 时可用。
//
// 这个文件在 core 而不是 app（计划把它放在 src/app/，实施时更正，见 ledger Ruling 14）：
// 它只依赖 ColorTarget，因此可以被单元测试直接断言，而 app 层是不可单测的那一层。
//
// 字形表用可读的字符画（'#' 是前景）而不是十六进制位表：字形画错时必须能一眼看出来并改掉。
// 行序自上而下，列序自左而右，最后一行留空作行距。

inline constexpr int kGlyphWidthPx = 5;
inline constexpr int kGlyphHeightPx = 8;
inline constexpr int kGlyphAdvancePx = 6;   // 5 列 + 1 列间距

struct Glyph {
    const char* rows[kGlyphHeightPx];       // 每条都是 kGlyphWidthPx 个字符
};

// 可打印字符永远返回一个字形：小写映射到大写；表里没有的字符返回"方块"，
// 因为缺字必须看得见 —— 静默画成空白的话，HUD 少了一整段你也不会发现。
const Glyph& glyphFor(char c);

// bgPacked == 0 表示"背景透明"。用 0 作哨兵是安全的：0x00000000 是"A=0 且全黑"，
// 我们的颜色出口从不产生它（encodeSRGB 的 alpha 默认 1.0）。
void drawText(ColorTarget& target, int x, int y, const std::string& text, uint32_t fgPacked,
              uint32_t bgPacked = 0u, int scale = 1);

int textWidthPx(const std::string& text, int scale = 1);

}  // namespace sr
