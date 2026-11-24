#include "scene/golden_scene.h"

#include <cstdio>

#include "math/transform.h"
#include "render/builtin_shaders.h"
#include "render/color.h"
#include "render/overlay_font.h"
#include "render/patterns.h"
#include "render/pipeline.h"
#include "render/render_backend.h"
#include "render/sampler.h"
#include "scene/mesh.h"

namespace sr {
namespace {

// 两个场景共用同一个立方体：几何必须逐顶点一致，否则"M3 与 M4 的差别只有贴图"
// 这句话就没有依据，端到端测试里"两帧相差多少像素"的归因也会失去意义。
// applyPositionColors 对 texturedFS 没有作用（它不读 v.color），但留着它，
// 两个场景的 Mesh 就是同一个函数产出的同一份数据。
Mesh goldenCube() {
    Mesh cube = makeCube(kGoldenCubeSize, true);
    applyPositionColors(cube);
    return cube;
}

}  // namespace

// 背景也共用：差别只应该来自"立方体上画的是什么"。
// M5.7 收紧 5：从文件内 static 提出去（声明在 golden_scene.h），OBJ golden 场景复用同一份。
void goldenBackground(ColorTarget& color) {
    fillLinearGradient(color, LinearColor{0.05f, 0.05f, 0.20f}, LinearColor{0.80f, 0.60f, 0.15f},
                       0.0f);
}

// 采样状态。ClampEdge 而不是 Repeat：每面的 uv 正好铺满 [0,1]，
// Repeat 会让 u=1 的边界回绕到第 0 列纹素，面的右边缘于是出现一条
// "从另一侧抄过来的颜色"——那不是 bug，但它会让参考图看起来像接错了。
// M5.7 收紧 5：提出去让 OBJ golden 场景复用 —— 第三份副本就是第三个会悄悄漂的地方。
SamplerState goldenSampler() {
    SamplerState s;
    s.wrapU = WrapMode::ClampEdge;
    s.wrapV = WrapMode::ClampEdge;
    s.filter = FilterMode::Trilinear;
    s.useMip = true;
    return s;
}

Mat4 goldenModel() { return Mat4::rotationX(kGoldenModelRotX) * Mat4::rotationY(kGoldenModelRotY); }

Mat4 goldenView() { return makeLookAt({0, 0, kGoldenCamZ}, {0, 0, 0}, {0, 1, 0}); }

Mat4 goldenProjection(int width, int height) {
    return makePerspective(kGoldenFovYRadians,
                           static_cast<float>(width) / static_cast<float>(height), 0.1f, 100.0f);
}

const Texture& goldenCheckerTexture() {
    // 函数内 static：贴图与它的 8 级 mip 链只算一次。golden 场景是单线程的（Backend A），
    // 而 C++11 起函数内 static 的初始化本身也是线程安全的，所以 app 侧复用同一个单例没问题。
    static const Texture t = makeTexture(
        makeCheckerImage(kGoldenCheckerCells, kGoldenCheckerCellPx, CheckerPalette{}),
        /*colorDataIsSrgb=*/true);
    return t;
}

void renderGoldenScene(RenderTarget& target, FrameStats* stats) {
    target.depth.clear();
    goldenBackground(target.color);

    Mesh cube = goldenCube();

    PipelineState st;
    st.target = &target;
    st.stats = stats;
    st.vertexShader = sr::transformVS;
    st.fragmentShader = sr::vertexColorFS;
    st.depthTest = true;
    st.depthWrite = true;
    st.cull.backfaceCull = true;
    st.vu.model = goldenModel();
    st.vu.view = goldenView();
    st.vu.proj = goldenProjection(kGoldenWidth, kGoldenHeight);
    // M6.0：法线矩阵 = 该旋转本身（纯旋转的逆转置 = 自身），不再是单位阵。
    // 今天没有任何 FS 读 worldNormal，所以参考图逐字节不变 —— 这正是本任务的验收；
    // M6.2 起光照会读它（§8.1 #4 的测试钉住"等于旋转且不等于单位阵"）。
    // 传入的 st.vu.model 两行前刚被赋为 goldenModel()，用同一份值避免两处漂移。
    // 纯旋转构造上不会奇异；万一失败退回单位阵（last-good）并记诊断，不黑屏。
    Mat4 nm;
    if (!makeNormalMatrix(st.vu.model, nm)) {
        nm = Mat4::identity();
        std::fprintf(stderr, "[normal] golden makeNormalMatrix 失败（奇异 model），法线矩阵退回单位阵\n");
    }
    st.vu.normalMatrix = nm;

    BackendA backend;
    backend.draw(cube, st);

    // HUD 也进 golden：它一次覆盖到字体表、像素写入与打包顺序。
    const uint32_t fg = encodeSRGB({1, 1, 1});
    const uint32_t bg = encodeSRGB({0.02f, 0.02f, 0.03f});
    drawText(target.color, 3, 3, "GOLDEN M3 320X180", fg, bg, 1);
}

void renderGoldenSceneTextured(RenderTarget& target, FrameStats* stats) {
    target.depth.clear();
    goldenBackground(target.color);

    Mesh cube = goldenCube();

    PipelineState st;
    st.target = &target;
    st.stats = stats;
    st.vertexShader = sr::transformVS;
    st.fragmentShader = sr::texturedFS;
    st.depthTest = true;
    st.depthWrite = true;
    st.cull.backfaceCull = true;
    st.vu.model = goldenModel();
    st.vu.view = goldenView();
    st.vu.proj = goldenProjection(kGoldenWidth, kGoldenHeight);
    // M6.0：法线矩阵 = 该旋转本身（纯旋转的逆转置 = 自身），不再是单位阵。
    // 今天没有任何 FS 读 worldNormal，所以参考图逐字节不变 —— 这正是本任务的验收；
    // M6.2 起光照会读它（§8.1 #4 的测试钉住"等于旋转且不等于单位阵"）。
    // 传入的 st.vu.model 两行前刚被赋为 goldenModel()，用同一份值避免两处漂移。
    // 纯旋转构造上不会奇异；万一失败退回单位阵（last-good）并记诊断，不黑屏。
    Mat4 nm;
    if (!makeNormalMatrix(st.vu.model, nm)) {
        nm = Mat4::identity();
        std::fprintf(stderr, "[normal] golden makeNormalMatrix 失败（奇异 model），法线矩阵退回单位阵\n");
    }
    st.vu.normalMatrix = nm;
    // baseColor 是**乘上去的材质 tint**（见 texturedFS），不是"没贴图时的兜底色"，
    // 所以这里显式给白：留着默认的 0.8 灰会让参考图整体暗一档，看起来像光照出了问题。
    st.fu.baseColor = LinearColor{1.0f, 1.0f, 1.0f};
    // 纹理指针指向进程内单例：它的生命周期长于任何一帧，而 PipelineState 只是借用它。
    st.fu.texture = &goldenCheckerTexture();
    st.fu.sampler = goldenSampler();

    BackendA backend;
    backend.draw(cube, st);

    // 标题与 M3 那一行**同长度、只差一个字**：这样两张参考图的差别能被数出来，
    // 而"拿错参考图"也不会静默通过（端到端测试盯着 HUD 区的差异像素数）。
    const uint32_t fg = encodeSRGB({1, 1, 1});
    const uint32_t bg = encodeSRGB({0.02f, 0.02f, 0.03f});
    drawText(target.color, 3, 3, "GOLDEN M4 320X180", fg, bg, 1);
}

}  // namespace sr
