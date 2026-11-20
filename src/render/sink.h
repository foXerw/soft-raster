#pragma once

#include "render/render_target.h"

namespace sr {

// 输出去向。渲染器写进 RenderTarget，sink 决定"这一帧变成什么"：
// 直接显示、深度图、灰度、以后的 overdraw 热图与分箱图都挂在这里。
// 抽象止步于此 —— sink 内部不许再藏渲染逻辑（设计文档 A1）。
class OutputSink {
  public:
    virtual ~OutputSink() = default;
    // 就地改写颜色目标：RenderTarget 是本帧的，覆盖它是安全的。
    virtual void present(RenderTarget& target) = 0;
    virtual const char* name() const = 0;
};

class DebugViewSink final : public OutputSink {
  public:
    enum class View : uint8_t { Color, Depth, Gray };

    void present(RenderTarget& target) override;
    const char* name() const override { return "debug"; }

    View view = View::Color;
    float depthNear = 0.0f;   // 深度视图的归一化区间
    float depthFar = 1.0f;
};

}  // namespace sr
