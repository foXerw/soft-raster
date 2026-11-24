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
    // M6.0：法线矩阵从此等于 goldenModel() 那个旋转（纯旋转的逆转置 = 自身），
    // 与 renderGoldenSceneTextured 一致地走 makeNormalMatrix。锚 3 的逐字节相同仍然成立：
    // 今天没有任何 FS 读 worldNormal —— 画面不变不是"物理上更对"的让步，而是"没人读"的证据。
    VertexUniforms vu;
    vu.model = goldenModel();
    vu.view = goldenView();
    vu.proj = goldenProjection(kGoldenWidth, kGoldenHeight);
    // 纯旋转构造上不会奇异；万一失败 setNormalMatrix 退回单位阵（last-good）并记诊断，
    // 不黑屏（P91/M6.3：兜底块四个生产点共用一份，见 pipeline.h）。
    setNormalMatrix(vu, vu.model, "obj golden ");

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
