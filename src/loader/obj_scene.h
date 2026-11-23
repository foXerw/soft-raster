#pragma once
// M5.7 锚 3 的场景函数：把**已载入**的 OBJ 模型画成与 M4 golden 同一套姿态的画面。
// 不读文件、不要路径 —— 装配（loadModel）与渲染（本函数）分开，锚 3 才能在进程内
// 渲染 + 比对而不落一张新参考图（收紧 2）。
//
// 分层（C4）：本文件进 **soft_raster_loader** 而不是 core —— 它要 LoadedModel
// （loader/model.h），而 core 不知道装配层的存在。姿态与采样状态全部来自 core 的
// scene/golden_scene.h（goldenModel/goldenView/goldenProjection/goldenBackground/
// goldenSampler，收紧 5：不许在这里写第二份）。
#include "loader/model.h"
#include "render/render_target.h"
#include "render/stats.h"

namespace sr {

// HUD 那行写 "GOLDEN M5 320X180" —— 与 M4 同长度、只差一个字
// （golden_scene.cpp 的既有做法：让"拿错参考图"不可能静默通过）。
void renderObjGoldenScene(const LoadedModel& m, RenderTarget& target, FrameStats* stats);

}  // namespace sr
