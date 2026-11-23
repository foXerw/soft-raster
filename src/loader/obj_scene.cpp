#include "loader/obj_scene.h"

#include "math/mat.h"
#include "render/color.h"
#include "render/overlay_font.h"
#include "render/pipeline.h"
#include "render/render_backend.h"
#include "scene/golden_scene.h"

namespace sr {

void renderObjGoldenScene(const LoadedModel& m, RenderTarget& target, FrameStats* stats) {
    target.depth.clear();
    goldenBackground(target.color);   // 与 M3/M4 同一份背景（收紧 5）

    // Ruling D15：锚 3 用 M4 的固定 pose，不用 fit 相机（spec §7.5 的那条对本锚豁免）。
    // normalMatrix = identity 与 renderGoldenSceneTextured 一致（model 是旋转，
    // 法线矩阵本该是它的逆转置，但 M4 参考图就是在 identity 下生成的 —— 锚 3 要的是
    // 逐字节相同，不是"物理上更对"）。
    VertexUniforms vu;
    vu.model = goldenModel();
    vu.view = goldenView();
    vu.proj = goldenProjection(kGoldenWidth, kGoldenHeight);
    vu.normalMatrix = Mat4::identity();

    FragmentUniforms base;
    // baseColor 是乘性 tint：显式给白。drawModel 会按组覆写成材质的线性 Kd，
    // cube.mtl 的 Kd 1 1 1 解码后仍是精确的 1（srgbToLinear(1)==1）⇒ tint 可证中性。
    base.baseColor = LinearColor{1.0f, 1.0f, 1.0f};
    base.sampler = goldenSampler();   // 收紧 5：ClampEdge/Trilinear/mip 全来自同一份

    BackendA backend;
    drawModel(backend, m, target, vu, base, stats);

    // 标题与 M4 那一行同长度、只差一个字（M3→M4→M5 的既有做法）：
    // 拿错参考图不可能静默通过，而锚 3 的比对恰好跳过这条 HUD 矩形。
    const uint32_t fg = encodeSRGB({1, 1, 1});
    const uint32_t bg = encodeSRGB({0.02f, 0.02f, 0.03f});
    drawText(target.color, 3, 3, "GOLDEN M5 320X180", fg, bg, 1);
}

}  // namespace sr
