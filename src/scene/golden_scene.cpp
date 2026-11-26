#include "scene/golden_scene.h"

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
    // 纯旋转构造上不会奇异；万一失败 setNormalMatrix 退回单位阵（last-good）并记诊断，
    // 不黑屏（P91/M6.3：兜底块各生产点共用一份，M6.8 起五个，见 pipeline.h）。
    setNormalMatrix(st.vu, st.vu.model, "golden ");

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
    // 纯旋转构造上不会奇异；万一失败 setNormalMatrix 退回单位阵（last-good）并记诊断，
    // 不黑屏（P91/M6.3：兜底块各生产点共用一份，M6.8 起五个，见 pipeline.h）。
    setNormalMatrix(st.vu, st.vu.model, "golden ");
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

// ---- M6.8：lit golden --------------------------------------------------------

const LightRig& goldenLitRig() {
    // 函数内 static const（goldenCheckerTexture 的先例）：只算一次，地址进程内稳定。
    // 值镜像 D19 的 app 默认（见 golden_scene.h 的注释与漂移守卫测试）。
    static const LightRig rig = [] {
        LightRig r;
        r.keyDirWorld = normalize(Vec3{-0.3f, -0.8f, -0.6f});   // "照向"（lights.h）
        r.keyColor = decodeLightColor(1.0f, 1.0f, 1.0f);
        r.keyIntensity = 0.35f;      // D19：不饱和默认（推导见 test_scene 的 #7a 前半）
        r.pointColor = decodeLightColor(1.0f, 1.0f, 1.0f);
        r.pointIntensity = 0.4f;     // D19：头灯几何下 1.0 必撞 255，0.4 留余量
        r.eyeWorld = Vec3{0.0f, 0.0f, kGoldenCamZ};   // goldenView 的眼
        r.pointPosWorld = r.eyeWorld;                 // 点光跟随眼（D5 默认）
        r.attenuate = false;                          // D7 默认关
        // 半径默认 = **包围盒**半径 ×3（buildLightRig/Ruling 49 同式；单位球的 AABB
        // 半径是 √3 不是 1）。衰减关时不参与着色，但漂移守卫逐字段对账，必须同式。
        const Mesh probe = makeUVSphere(1.0f, kGoldenLitStacks, kGoldenLitSlices);
        r.attenuateRadius = probe.bounds.radius() * 3.0f;
        r.ambient = LinearColor{1.0f, 1.0f, 1.0f};    // decode(1,1,1) × ambientIntensity 1.0
        return r;
    }();
    return rig;
}

void renderGoldenSceneLit(RenderTarget& target, FrameStats* stats) {
    target.depth.clear();
    goldenBackground(target.color);   // 与 M3/M4/M5 同一份背景（收紧 5）

    // 无贴图球（§8.2 夹具约束 1/2）：slices 奇数 ⇒ 没有顶点正对眼/点光，
    // Gouraud 的高光峰不落在顶点上（理由详见 golden_scene.h 的常量注释）。
    Mesh sphere = makeUVSphere(1.0f, kGoldenLitStacks, kGoldenLitSlices);

    PipelineState st;
    st.target = &target;
    st.stats = stats;
    st.vertexShader = sr::transformVS;
    st.fragmentShader = sr::blinnPhongPerPixelFS;
    st.depthTest = true;
    st.depthWrite = true;
    st.cull.backfaceCull = true;
    // D15：固定姿态，不用 fit 相机（fit 的 correctness 由数值断言守，M5 Ruling 69）。
    st.vu.model = goldenModel();
    st.vu.view = goldenView();
    st.vu.proj = goldenProjection(kGoldenWidth, kGoldenHeight);
    // 法线矩阵 = 该旋转本身（纯旋转的逆转置 = 自身）。**lit golden 真的消费它**：
    // blinnPhongPerPixelFS 读插值后的 worldNormal —— §8.3 #13（改回 identity 必红）
    // 是本章的地板。兜底走 setNormalMatrix 单一实现（P91）。
    // 能力边界（notes ②/补记）：姿态纯旋转 ⇒ normalMatrix == mat3(model) 数值相等，
    // #13 证"法线被消费、旋转有意义"，不证 inverse-transpose 机器本身（那由
    // test_transform 的非均匀缩放测试钉住）。
    setNormalMatrix(st.vu, st.vu.model, "golden lit ");
    // C5：rig 与材质都是函数内 static const 单例，两个阶段挂**同一个**对象
    //（Ruling 97 同形）；量化断言的夹具也取同一个 goldenLitRig() ⇒ 不会各自漂。
    const LightRig& rig = goldenLitRig();
    st.vu.lights = &rig;
    st.fu.lights = &rig;
    st.vu.mat = &litDefaultMaterialShade();
    st.fu.mat = &litDefaultMaterialShade();
    // 无贴图：fu.texture 保持 nullptr ⇒ blinnPhongPerPixelFS 的 albedo = 白（D26）。

    BackendA backend;
    backend.draw(sphere, st);

    // HUD 标签（D22）：比 M5 的 17 字符长 —— 实际 21 字符（简报写 20，数错了：
    // GOLDEN=6 + 空格 + M6=2 + 空格 + LIT=3 + 空格 + 320X180=7 = 21）。
    // 墨迹：末字形起点 x = 3 + 20×kGlyphAdvancePx(6) = 123，5 列宽 ⇒ 到 x=127。
    // 本场景与**自己的**参考图比对，两边是同一条标签 ⇒ 比对**不需要**排除矩形
    //（M5 的 x<110,y<12 是为"OBJ 场景 vs M4 参考"那条跨场景等式服务的，抄过来
    // 反而会静默忽略标签条上的真实差异）。
    const uint32_t fg = encodeSRGB({1, 1, 1});
    const uint32_t bg = encodeSRGB({0.02f, 0.02f, 0.03f});
    drawText(target.color, 3, 3, "GOLDEN M6 LIT 320X180", fg, bg, 1);
}

}  // namespace sr
