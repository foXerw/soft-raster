#include "app/input.h"

#include "harness.h"

using app::InputState;
using app::MouseButtons;

// 测的是"事件 vs 持续状态"的分离。产品反馈"拖动一卡一卡"的根因就在这里：
// InputState 每帧清零是对的（位移必须每帧重置），但按住不放是跨帧状态，
// 跟着一起清零就只有按下那一帧算拖拽，之后自动旋转接管，两边打架。

SR_TEST(button_state_survives_frames_with_no_messages) {
    MouseButtons b;
    b.setLeft(true);

    InputState frame1;
    b.seed(frame1);
    SR_ASSERT(frame1.leftPressed);

    // 关键：接下来几帧一条消息都没有（按住但不动，或系统没派发），
    // 快照仍然必须报告"按着"。旧实现里这几帧会变成"没按"。
    for (int i = 0; i < 3; ++i) {
        InputState f;
        b.seed(f);
        SR_ASSERT(f.leftPressed);
    }

    b.setLeft(false);
    InputState after;
    b.seed(after);
    SR_ASSERT_FALSE(after.leftPressed);
}

SR_TEST(right_button_is_independent_of_left) {
    MouseButtons b;
    b.setRight(true);
    InputState f;
    b.seed(f);
    SR_ASSERT(f.rightPressed);
    SR_ASSERT_FALSE(f.leftPressed);
}

SR_TEST(seed_does_not_clobber_the_per_frame_deltas) {
    // seed 只碰按键字段。若它顺手清零整个结构体，位移就丢了 —— 那会是"拖不动"。
    MouseButtons b;
    b.setLeft(true);
    InputState f;
    f.dx = 12.0f;
    f.dy = -7.0f;
    f.wheel = 2.0f;
    b.seed(f);
    SR_ASSERT(f.leftPressed);
    SR_ASSERT_NEAR(f.dx, 12.0f, 1e-6f);
    SR_ASSERT_NEAR(f.dy, -7.0f, 1e-6f);
    SR_ASSERT_NEAR(f.wheel, 2.0f, 1e-6f);
}
