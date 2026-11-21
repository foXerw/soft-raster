#pragma once

#include "math/mat.h"
#include "render/render_target.h"
#include "render/stats.h"
#include "render/texture.h"

namespace sr {

// 完全确定的 golden 场景：Backend A、单线程、无随机、无时间相关项。
// 320x180 而不是 640x360：golden 的作用是**抓数值回归**，不是展示画质，
// 而基准图要提交进仓库，尺寸直接决定仓库体积。
inline constexpr int kGoldenWidth = 320;
inline constexpr int kGoldenHeight = 180;

// ---------------------------------------------------------------------------
// 姿态。两个 golden 场景共用同一套相机与模型，端到端测试也要用它自己算
// "某个 uv 角点落在哪个像素"——姿态若在生产与测试里各写一份，两边就会各自漂，
// 而漂的症状是"测试期望的像素位置在别处"，看起来像渲染错了。
// 这些常量原来就是 renderGoldenScene 里的字面量（0.43f / 0.61f / 1.6f / 1.047f），
// 提出来时**一个数字都没改**：M3 的 golden 逐像素比对就是"重构没有改变任何东西"的证明。
// ---------------------------------------------------------------------------
inline constexpr float kGoldenCubeSize = 1.6f;
inline constexpr float kGoldenModelRotX = 0.43f;   // 弧度
inline constexpr float kGoldenModelRotY = 0.61f;
inline constexpr float kGoldenCamZ = 5.0f;         // 相机在 +Z 上看原点
inline constexpr float kGoldenFovYRadians = 1.047f;   // ≈60°

Mat4 goldenModel();                              // rotationX(kGoldenModelRotX) * rotationY(kGoldenModelRotY)
Mat4 goldenView();                               // lookAt((0,0,kGoldenCamZ), 原点, +Y)
Mat4 goldenProjection(int width, int height);

// M4 的贴图：4x4 的程序棋盘，每格 32 像素 ⇒ 128x128 ⇒ 8 级 mip 链。
// 为什么不是 8x8：屏幕上正面只有约 70 像素宽，8 格的话每格不到 9 像素，
// 而三线性在 rho≈1.8 时会重度混合 level 1 —— 标记色会被邻居的灰冲淡。
// 4 格时每格约 17 像素、中心离格边足够远，混合两级仍是纯标记色。
inline constexpr int kGoldenCheckerCells = 4;
inline constexpr int kGoldenCheckerCellPx = 32;

// 贴图与其 mip 链只算一次（进程内单例）。golden 场景每帧重建贴图的话，
// "参考图由哪个函数生成"这件事会多出一个时间维度。
const Texture& goldenCheckerTexture();

// 生产代码里也有它（src/scene/），因为参考图必须由**同一个函数**生成：
// 若测试里有一份、app 里有另一份，"参考图是怎么来的"就变成口头承诺。
void renderGoldenScene(RenderTarget& target, FrameStats* stats);

// M4 的参考场景：同一个立方体、同一套姿态，但走 texturedFS + 程序棋盘贴图。
// **它是一个新函数，不是 renderGoldenScene 的改动**：后者被 main.cpp 与
// `assets/golden/m3_cube.bmp` 用着，把纹理塞进去会让 M3 的回归锚点当场失效。
void renderGoldenSceneTextured(RenderTarget& target, FrameStats* stats);

}  // namespace sr
