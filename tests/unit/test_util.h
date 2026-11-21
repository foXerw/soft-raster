#pragma once
// 测试共用的极小工具。只给测试用，不进生产代码。
#include <cstddef>
#include <cstdint>
#include <vector>

#include "harness.h"
#include "math/vec.h"
#include "render/render_target.h"
#include "render/texture.h"

// 一张灰度 RGBA8 图：每个纹素 rgb = fn(x,y)、a = 255。
// 纹理相关的测试都需要"按坐标给值"的图，而**值就是坐标的函数**是唯一能让人一眼看出
// 采样取到了哪个纹素的写法（取错一个纹素，读回来的数字自己就会说出来）。
// 放在这里而不是每个测试文件各写一份：test_texture 与 test_sampler 用的是同一个构造器，
// 副本的用法是先各自悄悄改样，然后两条测试对同一个函数给出不同的结论。
inline sr::ImageRGBA8 makeGrayImage(int w, int h, uint8_t (*fn)(int x, int y)) {
    sr::ImageRGBA8 img;
    img.width = w;
    img.height = h;
    img.rgba.assign(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint8_t v = fn(x, y);
            const size_t at = (static_cast<size_t>(y) * static_cast<size_t>(w) +
                               static_cast<size_t>(x)) * 4u;
            img.rgba[at] = v;
            img.rgba[at + 1] = v;
            img.rgba[at + 2] = v;
            img.rgba[at + 3] = 255;
        }
    }
    return img;
}

// 常用值函数。命名比 lambda 好在：断言失败时能报出"用的是哪张图"。
inline uint8_t grayConstant200(int, int) { return 200; }
inline uint8_t grayXPlus1(int x, int y) { return static_cast<uint8_t>(x + y * 16 + 1); }

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
