#pragma once

#include "render/render_target.h"
#include "render/vertex.h"

namespace sr {

// clip → 屏幕。两件事在这里发生：透视除法与 NDC→像素映射。
//
// y 翻转（C8）就发生在这里，且只发生在这里：NDC 的 +y 是"上"，而 ColorTarget 的第 0 行
// 是上边缘，所以 pixel_y = (1 - (ndcY*0.5 + 0.5)) * height。
// 这条翻转同时让屏幕空间的绕向与 NDC 相反（CCW → CW），Task 2.4 的背面剔除按这个约定写。
struct Viewport {
    int x = 0;
    int y = 0;
    int width = 1;
    int height = 1;

    ScreenVertex project(const ClipVertex& cv) const;

    static Viewport fromColorTarget(const ColorTarget& t) {
        Viewport v;
        v.width = t.width;
        v.height = t.height;
        return v;
    }
};

}  // namespace sr
