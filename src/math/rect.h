#pragma once

#include <algorithm>

namespace sr {

// 像素闭区间矩形 [x0, x1] × [y0, y1]。空矩形用 x1 < x0 表示（而不是额外的 bool，
// 那样会出现"empty() 为假但区间反了"的第三种状态）。
struct Rect {
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;

    int width() const { return x1 - x0 + 1; }
    int height() const { return y1 - y0 + 1; }
    bool empty() const { return x1 < x0 || y1 < y0; }

    // 夹到 [0, w-1] × [0, h-1]。夹完可能变空，调用方必须检查 empty()。
    Rect clampedTo(int w, int h) const {
        Rect r;
        r.x0 = std::max(x0, 0);
        r.y0 = std::max(y0, 0);
        r.x1 = std::min(x1, w - 1);
        r.y1 = std::min(y1, h - 1);
        if (r.x1 < r.x0 || r.y1 < r.y0) { r.x1 = r.x0 - 1; r.y1 = r.y0 - 1; }   // 归一成"空"
        return r;
    }
};

}  // namespace sr
