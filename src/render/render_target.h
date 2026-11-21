#pragma once

#include <cstdint>

namespace sr {

// 一块颜色像素的**非拥有视图**。
//
// 两个约定来自设计文档，不是本地偏好：
//   C4：这里没有 <windows.h>，没有 GDI 类型。DIB 的内存由 app 层分配后交进来。
//   C8：行序一律 top-down —— 第 0 行是图像的上边缘。Win32 DIB 想按 bottom-up 排，
//        那个翻转只允许发生在 src/app/dib.cpp（我们用负 biHeight 让它根本不发生）。
// 之所以带 pitch 而不是假定 width*4：DIB 的 stride 有 4 字节对齐要求，
// 而测试里的目标故意用不连续的 pitch，好让"行跨度写错"在第一行之后立刻暴露。
struct ColorTarget {
    uint32_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    int pitch = 0;  // 单位：字节，必须 >= width*4

    void clear(uint32_t packed);
    void writePacked(int x, int y, uint32_t packed);  // 越界：静默丢弃
    uint32_t readPacked(int x, int y) const;         // 越界：返回 0（有定义，不读垃圾）
    bool contains(int x, int y) const;
};

// 一块深度缓冲的非拥有视图，支持**子矩形 view**。
//
// 这个能力不是锦上添花：设计文档 §7.2 的并行安全完全建立在"每个 tile 拿到自己的
// 深度子矩形，而所有子矩形指向同一份全局缓冲"之上。tile 之间不重叠 ⇒ 无锁、无原子，
// 同时 debug 深度视图仍然能读到整帧。所以 view() 必须是偏移指针而不是拷贝。
struct DepthTarget {
    float* values = nullptr;
    int width = 0;
    int height = 0;
    // 行跨度（元素个数），**必须与 width 分开**：子矩形 view 的行跨度是父缓冲的宽度。
    // 复审 I1：原先用 width 当跨度，于是 view 里 y>0 的读写落到错误的父像素上，
    // 而 M8 的"tile 不重叠所以无锁"前提正是建立在子矩形寻址正确之上。
    int stride = 0;

    DepthTarget() = default;
    DepthTarget(float* v, int w, int h) : values(v), width(w), height(h), stride(w) {}

    void clear(float v = 1.0f);  // 约定：默认最远 = 1.0（非 reverse-Z，§7.1）
    void write(int x, int y, float d);
    float read(int x, int y) const;  // 越界：返回 +Infinity
    bool contains(int x, int y) const;

    DepthTarget view(int x0, int y0, int w, int h) const;
};

struct RenderTarget {
    ColorTarget color;
    DepthTarget depth;
};

}  // namespace sr
