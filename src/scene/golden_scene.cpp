#include "scene/golden_scene.h"

#include "math/transform.h"
#include "render/builtin_shaders.h"
#include "render/color.h"
#include "render/overlay_font.h"
#include "render/patterns.h"
#include "render/pipeline.h"
#include "render/render_backend.h"
#include "scene/mesh.h"

namespace sr {

void renderGoldenScene(RenderTarget& target, FrameStats* stats) {
    target.depth.clear();
    fillLinearGradient(target.color, LinearColor{0.05f, 0.05f, 0.20f},
                       LinearColor{0.80f, 0.60f, 0.15f}, 0.0f);

    Mesh cube = makeCube(1.6f, true);
    applyPositionColors(cube);

    PipelineState st;
    st.target = &target;
    st.stats = stats;
    st.vertexShader = sr::transformVS;
    st.fragmentShader = sr::vertexColorFS;
    st.depthTest = true;
    st.depthWrite = true;
    st.cull.backfaceCull = true;
    st.vu.model = Mat4::rotationX(0.43f) * Mat4::rotationY(0.61f);
    st.vu.view = makeLookAt({0, 0, 5}, {0, 0, 0}, {0, 1, 0});
    st.vu.proj = makePerspective(1.047f, static_cast<float>(kGoldenWidth) / static_cast<float>(kGoldenHeight),
                                 0.1f, 100.0f);
    st.vu.normalMatrix = Mat4::identity();

    BackendA backend;
    backend.draw(cube, st);

    // HUD 也进 golden：它一次覆盖到字体表、像素写入与打包顺序。
    const uint32_t fg = encodeSRGB({1, 1, 1});
    const uint32_t bg = encodeSRGB({0.02f, 0.02f, 0.03f});
    drawText(target.color, 3, 3, "GOLDEN M3 320X180", fg, bg, 1);
}

}  // namespace sr
