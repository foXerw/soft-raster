#pragma once

#include "render/pipeline.h"
#include "scene/mesh.h"

namespace sr {

// 一个光栅后端。虚调用**每个 draw call 一次**，内层不再有（C5）。
class RenderBackend {
  public:
    virtual ~RenderBackend() = default;
    virtual const char* name() const = 0;
    virtual void draw(const Mesh& mesh, const PipelineState& state) = 0;
};

// Backend A：标量、逐像素、单线程、整帧共享一个深度目标。
// 它是"正确性"的参照物：M8/M9 的所有优化都必须与它逐像素一致（±1/255，设计文档 §7.8）。
class BackendA final : public RenderBackend {
  public:
    const char* name() const override { return "scalar"; }
    void draw(const Mesh& mesh, const PipelineState& state) override;
};

}  // namespace sr
