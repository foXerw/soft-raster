#pragma once

#include "render/pipeline.h"

namespace sr {

// 内置 shader。放在 core 而不是 app：它们要能被单元测试，也是 golden 场景的组成部分。

// 最普通的顶点变换：clip = proj * view * model * position。
// **varying 在这里不除 w**：ClipVertex::v 存的是未除的属性，因为 clip 空间里线性的是
// attr 与 w **各自**，而 attr/w 是"线性/线性"，对它插值在跨近平面的边上是错的
// （推导与数值证据见 vertex.h:56-67 —— 复审 C2 纠正的正是这句话原先写反的版本，
// 那时症状是"贴近或穿过模型时颜色与 UV 在裁剪边界上扭曲"）。
// 除法发生在两处：视口之后的平面方程插值（屏幕空间里 attr/w 与 1/w 才仿射），
// 以及裁剪插值时按 t 对 attr 与 w 分别插。
ClipVertex transformVS(const VSIn& in, const VertexUniforms& u);

// 直接输出基础色。M6 会被 lambert/blinnPhong 取代，但 M2 的可见成果靠它。
bool flatColorFS(const FragmentInput& in, const FragmentUniforms& u, FragmentOutput& out);

// 输出插值后的顶点色（M3 起 v 才是真的插值；M2 阶段是 flat）。
// M2 用它做"每个面一个颜色"，M3 用它做平滑渐变。
bool vertexColorFS(const FragmentInput& in, const FragmentUniforms& u, FragmentOutput& out);

// 纹理采样：用插值后的 uv 取纹素，再乘 baseColor 当材质 tint。
// 乘法发生在线性空间（TexelF 已在采样时解码，baseColor 本来就是线性的），
// 所以这一步不会引入 gamma 错误 —— 这是 Review Focus 5 的第二处翻转点之后的第一处运算。
// u.texture == nullptr 时退回 baseColor（见 pipeline.h 的理由）。
bool texturedFS(const FragmentInput& in, const FragmentUniforms& u, FragmentOutput& out);

// Gouraud（spec §7.3 的 lambert_pervertex）：在**顶点**算完整光照，把结果插值到像素。
// - 完整光照（Ruling 91/D25）：调 shadeBlinnPhong 而不是 shadeDiffuseOnly ——
//   保留这个模式的唯一理由是观察高光带与 Mach band，没有高光的 Gouraud 没有带可摊开。
// - 有效 albedo = 顶点色 × mat->diffuse（D17，线性空间；Kd 的乘法在 shadeBlinnPhong
//   内部完成，VS 的实参只传顶点色）。这兑现 main.cpp 老路径里"M6 的光照 shader 还要它"
//   那句承诺：顶点色从此是真消费者，不再休眠。
// - lights == nullptr ⇒ unlit 早退：v.color = in.color（mat == nullptr 同样早退，
//   没有材质可读，不发明默认材质）。不发明第四种模式、不加 bool。
// - **不采样贴图**（VS 阶段没有屏幕导数，也绝不为它加采样）——见下面 FS 的注释。
// MVP 与其余 varying 的填法和 transformVS 共用同一份实现（builtin_shaders.cpp 的
// clipFromPosition），光照结果写进已有的 v.color 槽（D6：kVaryingFloatCount 仍是 12）。
ClipVertex lambertVS(const VSIn& in, const VertexUniforms& u);

// Gouraud 的 FS：直读插值后的 v.color（顶点光照结果），**一行**。
// **它不读 u.texture / u.baseColor / u.lights**：带贴图的模型切到这个模式时贴图会
// "消失"，只剩顶点光照的平滑渐变 —— 这是 D17 的设计（VS 不采样），不是贴图坏了。
// 用户可见的解释必须在两处：这段 shader 选择注释，以及 M6.6 的 HUD 着色模式行。
// 行为由 gouraud_ignores_the_texture_and_says_so_in_the_shader_choice 钉住。
bool lambertPerVertexFS(const FragmentInput& in, const FragmentUniforms& u, FragmentOutput& out);

}  // namespace sr
