#pragma once

#include <cstdint>

#include "render/pipeline.h"
#include "scene/mesh.h"

namespace sr {

// 一个光栅后端。虚调用**每个 draw call 一次**，内层不再有（C5）。
class RenderBackend {
  public:
    virtual ~RenderBackend() = default;
    virtual const char* name() const = 0;
    // 索引区间入口（M5.0）：firstIndex 是 **mesh.indices 的下标**，不是顶点下标；
    // indexCount 是索引个数。M5 的 OBJ group 按同一单位喂偏移。
    virtual void drawRange(const Mesh& mesh, const PipelineState& state,
                           uint32_t firstIndex, uint32_t indexCount) = 0;
    // 整网格 draw = 全索引区间。**非虚**包装：现有调用点源码不动，
    // 且每个 draw call 仍只有一次虚派发（进 drawRange），不违反 C5。
    void draw(const Mesh& mesh, const PipelineState& state) {
        drawRange(mesh, state, 0, static_cast<uint32_t>(mesh.indices.size()));
    }
};

// Backend A：标量、逐像素、单线程、整帧共享一个深度目标。
// 它是"正确性"的参照物：M8/M9 的所有优化都必须与它逐像素一致（±1/255，设计文档 §7.8）。
class BackendA final : public RenderBackend {
  public:
    const char* name() const override { return "scalar"; }
    void drawRange(const Mesh& mesh, const PipelineState& state,
                   uint32_t firstIndex, uint32_t indexCount) override;
};

}  // namespace sr
