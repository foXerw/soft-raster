#pragma once

#include <vector>

#include "math/rect.h"
#include "math/vec.h"

namespace sr {

// 轴对齐包围盒。初始为"空"（min = +inf, max = -inf），grow 之后才有意义。
// 空盒子的 center/radius 返回零向量/0 而不是垃圾值：相机 fit（M5）会直接消费它们。
struct BBox {
    Vec3 min{1e30f, 1e30f, 1e30f};
    Vec3 max{-1e30f, -1e30f, -1e30f};

    void grow(const Vec3& p) {
        min = Vec3{p.x < min.x ? p.x : min.x, p.y < min.y ? p.y : min.y, p.z < min.z ? p.z : min.z};
        max = Vec3{p.x > max.x ? p.x : max.x, p.y > max.y ? p.y : max.y, p.z > max.z ? p.z : max.z};
    }
    void grow(const BBox& other) {
        if (!other.empty()) {
            grow(other.min);
            grow(other.max);
        }
    }
    bool empty() const { return min.x > max.x || min.y > max.y || min.z > max.z; }
    Vec3 center() const { return empty() ? Vec3{} : (min + max) * 0.5f; }
    float radius() const { return empty() ? 0.0f : length(max - center()); }

    static BBox fromPoints(const std::vector<Vec3>& pts) {
        BBox b;
        for (const Vec3& p : pts) b.grow(p);
        return b;
    }
};

}  // namespace sr
