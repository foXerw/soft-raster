#pragma once

#include "render/pipeline.h"

namespace sr {

// 内置 shader。放在 core 而不是 app：它们要能被单元测试，也是 golden 场景的组成部分。

// 最普通的顶点变换：clip = proj * view * model * position。
// varying 在这里就除以 w（见 vertex.h 里 ClipVertex 的推导）—— 这一步做错的后果
// 要到 M3 的插值测试才暴露，所以它必须写在最显眼的位置。
ClipVertex transformVS(const VSIn& in, const VertexUniforms& u);

// 直接输出基础色。M6 会被 lambert/blinnPhong 取代，但 M2 的可见成果靠它。
bool flatColorFS(const FragmentInput& in, const FragmentUniforms& u, FragmentOutput& out);

// 输出插值后的顶点色（M3 起 v 才是真的插值；M2 阶段是 flat）。
// M2 用它做"每个面一个颜色"，M3 用它做平滑渐变。
bool vertexColorFS(const FragmentInput& in, const FragmentUniforms& u, FragmentOutput& out);

}  // namespace sr
