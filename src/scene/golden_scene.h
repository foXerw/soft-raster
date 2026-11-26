#pragma once

#include "math/mat.h"
#include "render/lights.h"
#include "render/render_target.h"
#include "render/sampler.h"
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

// ---- M6.8：lit golden（设计 §8.2/§10）--------------------------------------
// 无贴图球的夹具常量。**slices 必须是奇数**（§8.2 夹具约束第 2 条）：顶点经度
// θ_j = 2πj/slices，正对相机/点光的 +z 方向在 θ=π/2，j = slices/4 只有在 slices
// 被 4 整除时才是整数 ⇒ 奇数 slices 保证没有顶点正对眼，Gouraud 的高光峰不会恰好
// 落在顶点上，"摊开 vs 收紧"（#7b）才不退化成空转对照。15/4 = 3.75 ⇒ 最近的顶点
// 经度差 0.25 格 = 6°。密度是**实测选出来的**（M6.8）：24/31 的细网格上三角形
//（7.5°×11.6°）比 Ns=64 的高光带（±8.4°）还小，Gouraud 的"摊开"与逐像素的带几乎
// 同宽（实测半高宽 10 vs 10），#7b 退化成空转；12/15 的三角形（15°×24°）显著大于
// 高光带，摊开才可见（实测见 test_golden_lit 的注释）。golden 的作用是抓数值回归，
// 不是展示画质（本文件开头）——轮廓略多边形化是有意取舍。
inline constexpr int kGoldenLitStacks = 12;
inline constexpr int kGoldenLitSlices = 15;   // 奇数（见上）
// 编译时钉住"没有顶点正对眼/点光"前提（终审修复波 D11）：θ=π/2 落在顶点 j=slices/4
// 上要求 4|slices —— 偶数且被 4 整除的 slices（如 24）让一个顶点恰好正对眼，Gouraud
// 在顶点直接采到 N·H=1 的峰，凸组合上界就是峰本身，#7b 的"摊开 vs 收紧"退化成空转。
// 奇数是比 slices%4!=0 更强、更简单的不变量，取它。注意**奇偶不是唯一前提**：M6.8
// 实测 24/31（slices=31 是奇数）也塌过 #7b（半高宽 10 vs 10）——那是密度效应（三角形
// 比 Ns=64 的高光带还小），由上面的实测密度区间与 test_golden_lit 的阈值钉住；
// 本 static_assert 只钉奇偶这一半。改密度必须重测分离度（见上）。
static_assert(kGoldenLitSlices % 2 == 1,
              "kGoldenLitSlices must be odd: slices divisible by 4 put a vertex facing the "
              "eye/point light, collapsing #7b's smear-vs-tight comparison (M6.8 measured "
              "24/31 degenerating it via the separate density premise)");

// lit golden 的 rig：函数内 static const，**进程内单例**（C5；M4"窗口与 golden
// 共用同一张贴图"的原则对 rig 同样适用）。vu.lights/fu.lights 与量化断言的夹具
// 都指向这一个对象 —— 参考图与断言夹具不可能各自漂。值 = D19 的 app 默认
//（AppOptions 的 keyIntensity 0.35 / pointIntensity 0.4 / keyDir {-0.3,-0.8,-0.6}），
// eye/point 在 golden 姿态的眼 (0,0,kGoldenCamZ)。core 不能 include app/cmdline.h
//（分层），所以这份镜像由测试 golden_lit_rig_matches_the_app_default_rig 钉住不漂。
const LightRig& goldenLitRig();

// M6.8 的参考场景：同一个固定姿态（D15：不用 fit 相机），但画无贴图球 +
// blinnPhongPerPixelFS + litDefaultMaterialShade()。**新函数，不改既有两个**：
// M3/M4 的参考图与回归锚点原样保留。签名与它们相同（void(*)(RenderTarget&,
// FrameStats*)）⇒ writeGolden 不需要 std::function 加宽。
void renderGoldenSceneLit(RenderTarget& target, FrameStats* stats);

// 背景与采样状态（M5.7 收紧 5：从 golden_scene.cpp 的文件内 static 提出来）。
// OBJ golden 场景（src/loader/obj_scene.cpp）必须**复用这两个函数**而不是再写一份：
// M4 的教训是两份副本会被一次改一个，而没被改的那份一直绿着。提出去的证明就是
// 两份老 golden 仍然匹配（M4 逐字节 md5 不变；M3 走 ctest 的 ±1 容差，Ruling 50）。
void goldenBackground(ColorTarget& color);
SamplerState goldenSampler();

}  // namespace sr
