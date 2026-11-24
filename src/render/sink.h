#pragma once

#include "render/render_target.h"

namespace sr {

// 输出去向。渲染器写进 RenderTarget，sink 决定"这一帧变成什么"：
// 直接显示、深度图、灰度挂在这里（哪些视图能走 FS、哪些必须走 sink，见下方分界）。
// 抽象止步于此 —— sink 内部不许再藏渲染逻辑（设计文档 A1）。
class OutputSink {
  public:
    virtual ~OutputSink() = default;
    // 就地改写颜色目标：RenderTarget 是本帧的，覆盖它是安全的。
    virtual void present(RenderTarget& target) = 0;
    virtual const char* name() const = 0;
};

// debug 视图两种机制的分界（D23，M6.5 —— 写死在这里，免得有人花一天去"统一"它们）：
// **能逐像素判定的视图走普通 FS**（法线视图 debugNormalFS；以后的 overdraw 计数），
// **需要整张缓冲的视图走 sink**（深度读整张深度缓冲，灰度读已经写好的颜色）。
// spec §7.3 的"debug_* 走正式 shader 同一条路"只适用于前者：法线视图读的是
// 本像素插值出来的法线，只有 FS 拿得到；深度/灰度天生是后处理 —— 硬改成 FS
// 等于伪造一个不可能的接口（FS 看不见别的像素的深度/颜色）。
// app 侧把"视图"映射成"FS 选择 + 帧末 sink"的表在 app/view_mode.h 的 planView。
// 两种机制并存、各归其位是设计，不是历史包袱。
class DebugViewSink final : public OutputSink {
  public:
    // 只有 Color/Depth/Gray：Normal 不是 sink 视图（它是 FS，见上方分界），
    // 所以这个枚举**不加** Normal —— 视图状态在 app::ViewMode（4 个）。
    enum class View : uint8_t { Color, Depth, Gray };

    void present(RenderTarget& target) override;
    const char* name() const override { return "debug"; }

    View view = View::Color;
    float depthNear = 0.0f;   // 深度视图的归一化区间
    float depthFar = 1.0f;
};

}  // namespace sr
