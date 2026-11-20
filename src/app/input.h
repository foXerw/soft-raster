#pragma once

namespace app {

// 一帧的输入快照。由 win32_window.cpp 从消息里填，渲染代码只看这个结构体，
// 因此渲染路径不需要知道 Win32 消息的存在。
//
// 语义说明（M7 会依赖）：
//   dx/dy      —— 本帧内的鼠标位移（累计），按下拖拽时非零
//   wheel      —— 本帧内的滚轮格数，正 = 远离用户
//   *Pressed   —— 本帧结束时按键状态（持续状态，不是事件）
//   其余 bool  —— "本帧发生了这个事件"，**用完即清**，由 pump 在每帧开始时归零
struct InputState {
    float dx = 0.0f;
    float dy = 0.0f;
    float wheel = 0.0f;
    int cursorX = 0;
    int cursorY = 0;

    bool leftPressed = false;
    bool rightPressed = false;

    bool togglePause = false;
    bool stepFrame = false;
    bool screenshot = false;
    bool cycleView = false;
    bool toggleWireframe = false;
    bool reloadModel = false;

    bool windowResized = false;
};

}  // namespace app
