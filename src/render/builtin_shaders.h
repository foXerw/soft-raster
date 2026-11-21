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

}  // namespace sr
