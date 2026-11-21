#pragma once

#include "render/color.h"
#include "render/render_target.h"

namespace sr {

// 测试图案。放在 core 而不是 app，因为 M0 的验收要能在离屏目标上断言，
// 而 app 层（Win32）是不可单测的那一层，必须保持薄。
//
// 关于 phase 为什么用三角波而不是 fract：平移一条渐变时不能产生接缝。
// 接缝会在画面上留下一条竖线，而这条竖线看起来 exactly 像一个渲染 bug ——
// 测试图案的职责是排除歧义，不是制造歧义。

// 水平渐变：x=0 处为 a，x=width-1 处为 b（phase=0 时）。phase 把图案沿 x 平移，
// 周期为 2 个宽度（镜像折返），因此对任意 phase 都连续。
void fillLinearGradient(ColorTarget& target, const LinearColor& a, const LinearColor& b,
                        float phase);

// 棋盘格，格子边长 cellPx 像素（cellPx < 1 按 1 处理）。
void fillChecker(ColorTarget& target, const LinearColor& a, const LinearColor& b, int cellPx);

}  // namespace sr
