#pragma once

#include "render/render_target.h"
#include "render/stats.h"

namespace sr {

// 完全确定的 golden 场景：Backend A、单线程、无随机、无时间相关项。
// 320x180 而不是 640x360：golden 的作用是**抓数值回归**，不是展示画质，
// 而基准图要提交进仓库，尺寸直接决定仓库体积。
inline constexpr int kGoldenWidth = 320;
inline constexpr int kGoldenHeight = 180;

// 生产代码里也有它（src/scene/），因为参考图必须由**同一个函数**生成：
// 若测试里有一份、app 里有另一份，"参考图是怎么来的"就变成口头承诺。
void renderGoldenScene(RenderTarget& target, FrameStats* stats);

}  // namespace sr
