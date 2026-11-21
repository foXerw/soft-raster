#pragma once

#include <cstdint>

#include "math/mat.h"
#include "render/color.h"
#include "render/render_target.h"
#include "render/stats.h"
#include "render/vertex.h"

// 名字守卫。踩过的坑（M2 实施记录）：WinUser.h 里有 `#define DrawState DrawStateA`
// （GDI 函数 DrawState 的 ANSI/Unicode 分发），所以我们的 DrawState 在 core 里一切正常
// （core 不 include windows.h，设计文档 C4），一进 app 层就被静默改名，
// 症状是链接错误里出现一个源码中根本不存在的类型名 `sr::DrawStateA`。
// 现在类型叫 PipelineState；若哪天又和某个系统宏撞名，下面这几行会把
// "疯狂的链接错误"变成"一行编译错误"。
#ifdef PipelineState
#error "PipelineState 被某个头定义成宏了：本项目里它是类型名，请改名或调整包含顺序"
#endif
#ifdef Viewport
#error "Viewport 被某个头定义成宏了：本项目里它是类型名"
#endif
#ifdef RenderTarget
#error "RenderTarget 被某个头定义成宏了：本项目里它是类型名"
#endif

namespace sr {

struct VertexUniforms {
    Mat4 model;
    Mat4 view;
    Mat4 proj;
    Mat4 normalMatrix;  // transpose(inverse(model)) 的 3x3 部分，M6 的法线变换用
};

// M4/M6 会往这里加 texture 指针与光源；M2 阶段只有基础色。
struct FragmentUniforms {
    LinearColor baseColor{0.8f, 0.8f, 0.8f};
};

struct FragmentInput {
    Vec2 screen;   // 像素坐标（top-down）
    float depth = 0.0f;
    Varyings v;    // M3 之前是"三角形第一个顶点的值"（flat shading）
    // uv 的屏幕梯度，**每个标量场一个向量**：gradU = (∂u/∂x, ∂u/∂y)、gradV = (∂v/∂x, ∂v/∂y)。
    // 名字里刻意不写 X/Y：这两个字段一度叫 uvDerivX / uvDerivY，读起来像"沿 X 的导数、沿 Y 的
    // 导数"，而 mipLevelFromDerivatives 的参数顺序是 (∂u/∂x, ∂v/∂x, ∂u/∂y, ∂v/∂y) —— 两者差一次
    // **转置**。填错的后果不是崩溃也不是黑屏，只是"mip 选错、画面糊或者闪"，最难归因。
    // 现在唯一的入口收 Vec2 gradU, gradV，按位置传错四个 float 的机会没有了。
    Vec2 gradU;
    Vec2 gradV;
};

struct FragmentOutput {
    LinearColor color{0.0f, 0.0f, 0.0f};
};

// 逐顶点 / 逐像素的可调用接口。用函数指针而不是 virtual：
// 逐像素派发如果走虚表，M9 的 SIMD 就废了（设计文档 C5）。
using VertexShaderFn = ClipVertex (*)(const VSIn&, const VertexUniforms&);
using FragmentShaderFn = bool (*)(const FragmentInput&, const FragmentUniforms&, FragmentOutput&);

struct CullState {
    bool backfaceCull = true;
    // top-down 屏幕空间里正面是 CW（NDC 的 CCW 经过 y 翻转）。见 Task 2.4 的测试。
    bool frontFaceIsClockwise = true;
};

struct PipelineState {
    RenderTarget* target = nullptr;
    VertexShaderFn vertexShader = nullptr;
    FragmentShaderFn fragmentShader = nullptr;
    VertexUniforms vu;
    FragmentUniforms fu;
    CullState cull;
    bool depthTest = true;   // M3 起为真实现；与 depthWrite 互相独立
    bool depthWrite = true;
    FrameStats* stats = nullptr;
};

}  // namespace sr
