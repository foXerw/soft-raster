#pragma once

namespace sr {

// 一行构建标识：项目名 + 版本 + Debug/Release + 编译器。
// HUD 左下角、`--bench` 输出头、以及每个 golden 的生成说明都要带上它——
// 没有这一行，M8/M9 的性能数字和 golden 差异都无法归因到具体构建配置。
const char* buildInfoString();

}  // namespace sr
