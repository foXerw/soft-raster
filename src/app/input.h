#pragma once

namespace app {

struct InputState;

// 鼠标按键的**持续状态**。单独一个类型的原因是一个产品级 bug：
// InputState 每帧被 pumpInput 清零是对的（位移与"本帧发生过什么"必须每帧重置），
// 但"左键是否还按着"是跨帧状态。把它跟着清零，等于每次拖拽只有按下那一帧生效，
// 之后自动旋转接管 —— 症状正是"拖动一卡一卡"。
//
// 放进类型里而不是靠注释约定，是因为注释已经挡不住过一次（本文件原先的注释
// 写着"持续状态"，实现却每帧清零）。
class MouseButtons {
  public:
    void setLeft(bool down) { m_left = down; }
    void setRight(bool down) { m_right = down; }
    bool left() const { return m_left; }
    bool right() const { return m_right; }

    // 每帧开始时把持续状态播种进快照：没有消息到达的那些帧也必须报告按住。
    // 只碰按键字段，不清零任何东西。
    void seed(InputState& s) const;

  private:
    bool m_left = false;
    bool m_right = false;
};

// 一帧的输入快照。由 win32_window.cpp 从消息里填，渲染代码只看这个结构体，
// 因此渲染路径不需要知道 Win32 消息的存在。
//
// 语义（M7 会依赖）：
//   dx/dy      —— 本帧内的鼠标位移（累计），按住拖拽时非零
//   wheel      —— 本帧内的滚轮格数，正 = 远离用户
//   *Pressed   —— 按键的持续状态，来自 MouseButtons，每帧由 seed 播种
//   其余 bool  —— "本帧发生了这个事件"，用完即清
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

inline void MouseButtons::seed(InputState& s) const {
    s.leftPressed = m_left;
    s.rightPressed = m_right;
}

}  // namespace app
