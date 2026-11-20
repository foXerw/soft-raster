#include "render/viewport.h"

namespace sr {

ScreenVertex Viewport::project(const ClipVertex& cv) const {
    ScreenVertex s;
    const float invW = 1.0f / cv.clip.w;
    const float ndcX = cv.clip.x * invW;
    const float ndcY = cv.clip.y * invW;
    const float ndcZ = cv.clip.z * invW;

    s.pos.x = static_cast<float>(x) + (ndcX * 0.5f + 0.5f) * static_cast<float>(width);
    // y 翻转只发生在这里（C8）：NDC 的 +y 是"上"，而 ColorTarget 的第 0 行是上边缘。
    s.pos.y = static_cast<float>(y) + (1.0f - (ndcY * 0.5f + 0.5f)) * static_cast<float>(height);
    s.depth = ndcZ * 0.5f + 0.5f;
    s.w = cv.clip.w;
    s.invW = invW;
    // vOverW 存的是 v/w，乘回 w 得到未除的 varying。
    s.v = cv.vOverW * cv.clip.w;
    return s;
}

}  // namespace sr
