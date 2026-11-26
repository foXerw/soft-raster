#pragma once

#include <cstdint>
#include <cstdio>

#include "math/mat.h"
#include "math/transform.h"
#include "render/color.h"
#include "render/lights.h"
#include "render/render_target.h"
#include "render/sampler.h"
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
    // M6.1（D15）：与 FragmentUniforms 指向**同一份**每帧对象（单一真源，不是两份拷贝）。
    // lights == nullptr ⇒ unlit（lit shader 的早退语义；不发明第四种模式、不加 bool）。
    // mat 由 drawModel 逐组覆写（M6.3 的 lambertVS 要读 diffuse：albedo = 顶点色 × Kd）。
    const LightRig* lights = nullptr;
    const MaterialShade* mat = nullptr;
};

// P91（M6.3）：makeNormalMatrix + 奇异兜底的**唯一一份**实现。此前这段 if/兜底/诊断
// 在四个生产点逐字重复（main.cpp、golden_scene.cpp ×2、obj_scene.cpp）——
// M5 lex.h 判例：逐字重复的逻辑块必须唯一一份，副本会各自悄悄漂移。
// 语义与各点原样一致：成功 ⇒ normalMatrix = 逆转置；奇异 ⇒ 退回单位阵并记一条诊断，
// 画面继续，不退出也不黑屏（M5.6 同原则）。这四个点的 model 每帧恒定（单位阵或固定旋转），
// 所以 last-good 就是单位阵本身；将来 model 逐帧变化时，last-good 状态由调用方持有。
// siteLabel 是诊断里的场景前缀（""/"golden "/"obj golden "）：各点的 stderr 文本逐字保留。
// 返回 makeNormalMatrix 的成败。消费者：app 的 prepareFrameUniforms（scene.cpp）把它
// 取反记进 scene.normalMatrixDegenerate ⇒ HUD 显示 "NM DEGENERATE"（P98，M6.6 接线，
// nm_degenerate_flag_reaches_the_hud 钉住）。三个场景调用点（golden_scene.cpp ×2、
// obj_scene.cpp）忽略返回值：姿态是固定纯旋转，构造上不会奇异；兜底诊断本函数内部已发。
// 兜底先赋给局部 nm 再统一写 vu.normalMatrix（与各点原样同形）：M6.9 的 grep 守卫
// 查的是"把单位阵字面量直接赋给 normalMatrix 字段"的生产代码，零命中要求 ——
// 单位阵字面量只许活在这一个兜底里（经局部变量转手，守卫的模式匹配不到）。
inline bool setNormalMatrix(VertexUniforms& vu, const Mat4& model, const char* siteLabel) {
    Mat4 nm;
    const bool ok = makeNormalMatrix(model, nm);
    if (!ok) {
        nm = Mat4::identity();
        std::fprintf(stderr, "[normal] %smakeNormalMatrix 失败（奇异 model），法线矩阵退回单位阵\n",
                     siteLabel);
    }
    vu.normalMatrix = nm;
    return ok;
}

// M4 加了纹理与采样状态（Ruling A：不放 PipelineState，因为那里剩下的字段全是
// 固定功能阶段读的，采样是 FS 的事）；M6.1 加了光照数据（D15，与 VertexUniforms 同源）。
struct FragmentUniforms {
    LinearColor baseColor{0.8f, 0.8f, 0.8f};
    // nullptr = 这张 draw call 没有贴图，texturedFS 退回 baseColor。
    // 选"退回纯色"而不是"崩掉/画黑"：忘了设纹理时你会看到一块纯色，
    // 于是能立刻知道是纹理没接上；画黑的话，第一个怀疑对象会变成整条管线。
    const Texture* texture = nullptr;
    SamplerState sampler;
    // 与 VertexUniforms 的同名字段指向同一份对象（D15）。lights == nullptr ⇒ unlit。
    const LightRig* lights = nullptr;
    const MaterialShade* mat = nullptr;
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
