#pragma once

#include <string>

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
    // M5.6：F = 显式重 fit（只动 distance/target/near/far，yaw/pitch 归用户，D15）。
    bool refit = false;
    // M5.6：本帧拖放进来的模型路径（**UTF-8**）。WM_DROPFILES 给的是 UTF-16，
    // win32_window.cpp 用 utf8FromWide 转成 UTF-8，main 用 nativePath 转原生路径
    // —— 与 --obj 走同一个 SceneState::load 入口（D18）。后缀守卫在 load 里（A4）。
    // 注意：droppedObj 为空**不一定**是没拖放——转换失败时 utf8FromWide 也返回空。
    // "本帧有没有拖放事件"由 dropEvent 单独标记，main 凭它决定走 load（空路径会拿到
    // 自己的错误消息，响亮失败，不静默忽略 —— A4 / 终审 M3）。
    std::string droppedObj;
    bool dropEvent = false;
    // M4 的两个采样旋钮（spec §7.6 的按键表把它们分开列，所以这里是两个事件而不是一个"切过滤"）。
    // `1` = mip 开关，`2` = nearest → bilinear → trilinear。两者正交（Ruling 31）：
    // "关 mip 但用三线性"是个合法组合，它的语义是"永远在 level 0 上做双线性"。
    bool toggleMip = false;
    bool cycleFilter = false;

    // M6.6：着色模式与光参数按键（D24）。`3` 循环着色模式；`[`/`]` 点光强度 ∓/±0.1；
    // `-`/`=` 方向光强度 ∓/±0.1；`A` 衰减开关；`0` 恢复默认。加法步进 ⇒ 0 可达。
    bool cycleShade = false;
    bool toggleAttenuate = false;
    bool resetLights = false;
    bool stepPointUp = false;     // ]
    bool stepPointDown = false;   // [
    bool stepKeyUp = false;       // =
    bool stepKeyDown = false;     // -

    bool windowResized = false;
};

inline void MouseButtons::seed(InputState& s) const {
    s.leftPressed = m_left;
    s.rightPressed = m_right;
}

}  // namespace app
