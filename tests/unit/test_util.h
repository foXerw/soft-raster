#pragma once
// 测试共用的极小工具。只给测试用，不进生产代码。
#include <vector>

#include "harness.h"
#include "math/vec.h"
#include "render/render_target.h"

inline void expectVec3Near(const sr::Vec3& a, float x, float y, float z, float tol) {
    SR_ASSERT_NEAR(a.x, x, tol);
    SR_ASSERT_NEAR(a.y, y, tol);
    SR_ASSERT_NEAR(a.z, z, tol);
}

// 就地构造（绝不按值返回）：RenderTarget 里存的是指向自己成员的裸指针。
struct Canvas {
    std::vector<uint8_t> colorMem;
    std::vector<float> depthMem;
    sr::RenderTarget rt;

    explicit Canvas(int w, int h)
        : colorMem(static_cast<size_t>(w) * h * 4u, 0), depthMem(static_cast<size_t>(w) * h, 1.0f) {
        rt.color = sr::ColorTarget{reinterpret_cast<uint32_t*>(colorMem.data()), w, h, w * 4};
        rt.depth = sr::DepthTarget{depthMem.data(), w, h};
    }
};
