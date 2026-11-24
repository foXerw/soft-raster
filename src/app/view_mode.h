#pragma once

#include <cstdint>

#include "render/pipeline.h"   // sr::FragmentShaderFn
#include "render/sink.h"       // sr::DebugViewSink::View

namespace app {

// M6.5（D23）：app 的"视图"状态，以及它到"FS 选择 + 帧末 sink"的映射。
// 本文件**不含任何 Win32**（cmdline.cpp / scene.cpp 的先例，C4）：它被直接编进
// tests 目标（CMakeLists 的 tests 源列表显式列出），路由表由 test_view_mode.cpp 逐条断言。
//
// 两种机制并存、各归其位（D23，分界注释在 sink.h 的 DebugViewSink 上方）：
// - 能逐像素判定的视图（法线；以后的 overdraw 计数）走**普通 FS** ——
//   spec §7.3 的"debug_* 走正式 shader 同一条路"只适用于这一类：法线视图读的是
//   本像素插值出来的 worldNormal，只有 FS 拿得到。
// - 需要整张缓冲的视图（深度读整张深度缓冲、灰度读已写好的颜色）只能是帧末
//   后处理 sink —— 硬改成 FS 等于伪造一个不可能的接口。
enum class ViewMode : uint8_t { Color, Depth, Gray, Normal };

// useFragmentShader = true ⇒ 本次提交用 fragmentShader **覆写**默认 FS；
// false ⇒ fragmentShader 就是传进来的 defaultFS 原样（视图不干预着色，只干预帧末 sink）。
// Normal ⇒ {true, debugNormalFS, View::Color, "NORMAL"}：sink 保持恒等，
// 法线视图绝不能再被后处理改写（D23）。
struct ViewPlan {
    bool useFragmentShader;
    sr::FragmentShaderFn fragmentShader;
    sr::DebugViewSink::View sink;
    const char* name;   // HUD / 终端显示（ASCII：位图字体没有非 ASCII 字形）
};

ViewPlan planView(ViewMode mode, sr::FragmentShaderFn defaultFS);

}  // namespace app
