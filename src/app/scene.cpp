#include "app/scene.h"

#include <cstdio>
#include <utility>

#include "loader/lex.h"   // displayPath（Ruling 62：消息里的路径不用 .string()）
#include "math/transform.h"      // makePerspective
#include "render/builtin_shaders.h"   // planShade 的三个 shader
#include "scene/fit.h"    // fitCamera + kDefaultFovYRadians（P24：fit 与投影同一个角）

// 本 TU 不含任何 Win32 头（cmdline.cpp 先例）：它被 tests 目标直接编译，
// load/refit 的行为由 tests/unit/test_scene.cpp 逐条判定。

namespace app {
namespace {

// 大小写不敏感的 .obj 后缀判定（入口守卫，A4：非 .obj 不静默忽略）。
// 与 model.cpp 的 hasTgaExtension 同形但**职责不同**：那个决定"走哪个解码器"
// （Ruling 63），这个决定"这是不是一个模型文件"。argv 与拖放都经过 load()，
// 所以守卫只写这一处。用 u8string（无损）而不是 .string()（Ruling 62）。
bool hasObjExtension(const std::filesystem::path& p) {
    const std::u8string ext = p.extension().u8string();
    if (ext.size() != 4) return false;
    const char* e = reinterpret_cast<const char*>(ext.data());
    return e[0] == '.' && (e[1] == 'o' || e[1] == 'O') && (e[2] == 'b' || e[2] == 'B') &&
           (e[3] == 'j' || e[3] == 'J');
}

}  // namespace

const sr::Mesh& fallbackCube() {
    static const sr::Mesh cube = [] {
        sr::Mesh m = sr::makeCube(1.6f, true);
        sr::applyPositionColors(m);   // M3：位置编码色，插值不连续就看得见
        return m;
    }();
    return cube;
}

bool SceneState::load(const std::filesystem::path& obj) {
    // 空路径单独成一类错误（终审 M3/P27）：nativePath/utf8FromWide 转换失败时返回 {}，
    // 若落到后缀守卫会报"只支持 .obj"——把**编码**失败误归因成**后缀**错误。空路径不过
    // 后缀守卫、也不进 R 的重试路径（它不是一个可重试的请求）。
    if (obj.empty()) {
        errorLine = "路径为空（编码转换失败或未给出路径）";
        return false;
    }
    if (!hasObjExtension(obj)) {
        errorLine = sr::lex::displayPath(obj) + ": 只支持 .obj 模型";
        return false;
    }
    // 只有**可信的请求**才进 R 的重试路径（终审 M4/P26）：坏后缀的拖放若覆盖 requested，
    // R 会永远重试那个 .txt 而不是最后一个好模型。真正的载入失败（文件缺失/内容坏）仍更新
    // requested —— "修好文件再按 R"对那种失败成立（后缀不会自己变好，内容会）。
    requested = obj;
    // **先建新的再换指针**（绝不先 model.reset()）：失败时 fresh 直接丢弃，
    // 旧模型原样留在画面上 —— "失败保留旧模型"不是靠回滚，是靠根本没动过旧的。
    auto fresh = std::make_unique<sr::LoadedModel>();
    std::string err;
    if (!sr::loadModel(obj, *fresh, err)) {
        errorLine = err;   // loadModel 的 err 已带文件名/行号（displayPath，UTF-8 无损）
        return false;
    }
    // 零面守卫（M5.6 评审 Important 1）。职责分界：**loader 问"我解析对了吗"，app 问
    // "这画面有没有东西可看"**。没有任何面的 OBJ 在解析意义上合法（parseObj/loadModel
    // 返回 true；将来的点云/线框专用几何也会是它），所以 loader 拒绝它是错的；但
    // drawModel 对零 group 什么都不提交 ⇒ 空屏 + DC 0 + 终端"已载入 0 三角形"，
    // 恰好绕过本任务"载入失败绝不空屏"的承诺 —— 所以在换指针**之前**判失败，
    // 兜底立方体/旧模型原样保留。
    if (fresh->model.mesh.indices.empty()) {
        errorLine = sr::lex::displayPath(obj) + ": 0 三角形（没有面），画面上没有东西可看";
        return false;
    }
    model = std::move(fresh);
    errorLine.clear();
    modelLabel = sr::lex::displayPath(obj.filename());
    return true;
}

void SceneState::refit(int width, int height) {
    if (model == nullptr) return;   // 程序生成立方体没有 fit 概念，相机归用户
    // parseObj 成功末尾已 recomputeBounds（obj.h 契约），这里直接消费包围球。
    const sr::BBox& bounds = model->model.mesh.bounds;
    const float r = bounds.radius();
    // 退化 ⇒ 不动作（与 fitCamera 守卫同口径，Ruling 49）：六个字段一个都不碰。
    // 若这里仍按公式写夹钳，max = r*1e3 会低于 min = 1e-6，dolly 会把相机夹死。
    if (bounds.empty() || r < 1e-6f) return;
    // fov 用 kDefaultFovYRadians：renderFrame 的 makePerspective 取的是同一个常量，
    // 两边漂移的症状是"某些模型装不下"（P24）。退化 aspect 由 fitCamera 入口守卫兜住。
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const sr::FittedCamera f = sr::fitCamera(bounds, sr::kDefaultFovYRadians, aspect, camera);
    camera = f.camera;   // fitCamera 只动 distance/target；yaw/pitch 从 base 继承（D15）
    nearPlane = f.nearPlane;
    farPlane = f.farPlane;
    // Ruling 73：fit 是 distance 的第二个写者，夹钳必须知道 fit 的尺度，否则 mm 级 /
    // 千单位级模型第一次滚轮就被旧常数 [0.05, 1000] snap（"某些模型一动就飞走"）。
    // 不变式：fit 的**可见**输出（distance ≤ far = 10r；超出 far 的帧本就全裁，
    // 见 fit.h 的已知限制）恒有 distance ≥ 2r > min 且 ≤ 10r < max ⇒ 落在区间内，
    // 第一下滚轮不跳。test_scene 的夹钳用例直接断言这条不变式，不只断言数值。
    const float lo = r * 1e-3f;
    camera.minDistance = lo > 1e-6f ? lo : 1e-6f;
    camera.maxDistance = r * 1e3f;
}

// ---- M6.6：光照接线 ----------------------------------------------------------

sr::LightRig buildLightRig(const AppOptions& o, const sr::BBox& bounds, const sr::OrbitCamera& cam) {
    sr::LightRig rig;
    // keyDirWorld 是"照向"（lights.h），归一化在这里做一次（AppOptions 存原始值）。
    // 零向量约定（vec.h）：--key-dir 0 0 0 ⇒ normalize 返回零 ⇒ N·L=0 ⇒ 方向光自然关掉。
    rig.keyDirWorld = sr::normalize(sr::Vec3{o.keyDir[0], o.keyDir[1], o.keyDir[2]});
    // 颜色解码**一次**（D5）：AppOptions 是 sRGB 域，这里经 decodeLightColor 进线性。
    rig.keyColor = sr::decodeLightColor(o.keyColor[0], o.keyColor[1], o.keyColor[2]);
    rig.keyIntensity = o.keyIntensity;
    rig.pointColor = sr::decodeLightColor(o.pointColor[0], o.pointColor[1], o.pointColor[2]);
    rig.pointIntensity = o.pointIntensity;
    rig.eyeWorld = cam.eye();
    rig.pointPosWorld = o.pointPosGiven
                            ? sr::Vec3{o.pointPos[0], o.pointPos[1], o.pointPos[2]}
                            : rig.eyeWorld;   // D5：默认跟随相机
    rig.attenuate = o.attenuate;
    // P97：radius 恒 >0。解析层已拒 <=0；这里对程序化塞入的 0/负数/哨兵再夹一道，
    // 绝不让 0 到达 shading.h 的 d/radius 除法。默认 = 场景半径 ×3（空盒/退化 ⇒1.0，Ruling 49）。
    float radius = o.attenuateRadius;
    if (!(radius > 0.0f)) {
        const float br = bounds.radius();
        radius = (bounds.empty() || br < 1e-6f) ? 1.0f : br * 3.0f;
    }
    rig.attenuateRadius = radius;
    // 环境光色 = 解码后的颜色 × 强度（--ambient 是线性乘子，与 key/point 的 color+intensity 同形）。
    const sr::LinearColor amb =
        sr::decodeLightColor(o.ambientColor[0], o.ambientColor[1], o.ambientColor[2]);
    rig.ambient = sr::LinearColor{amb.r * o.ambientIntensity, amb.g * o.ambientIntensity,
                                  amb.b * o.ambientIntensity};
    return rig;
}

void stepIntensity(float& value, float delta) {
    // D24：加法步进 ⇒ 0 可达（乘法永远到不了 0）。夹到 [0,3]。
    value += delta;
    if (value < 0.0f) value = 0.0f;
    if (value > 3.0f) value = 3.0f;
}

ShadePlan planShade(ShadeMode mode) {
    switch (mode) {
        case ShadeMode::Unlit: return {sr::transformVS, sr::texturedFS, "UNLIT"};
        case ShadeMode::Gouraud: return {sr::lambertVS, sr::lambertPerVertexFS, "GOURAUD"};
        case ShadeMode::Phong: return {sr::transformVS, sr::blinnPhongPerPixelFS, "PHONG"};
    }
    return {sr::transformVS, sr::texturedFS, "UNLIT"};
}

ShadeMode cycleShadeMode(ShadeMode mode) {
    switch (mode) {
        case ShadeMode::Phong: return ShadeMode::Gouraud;
        case ShadeMode::Gouraud: return ShadeMode::Unlit;
        case ShadeMode::Unlit: return ShadeMode::Phong;
    }
    return ShadeMode::Phong;
}

ShadeMode parseShadeMode(const std::string& s) {
    if (s == "unlit") return ShadeMode::Unlit;
    if (s == "gouraud") return ShadeMode::Gouraud;
    return ShadeMode::Phong;   // parseCmdline 已校验合法值；未知 defensive 回默认
}

void SceneState::resetLights() {
    // R99：恢复启动基线（命令行构造的 rig），不是内置默认。--key-intensity 2 启动后按 0
    // 仍是 2 —— 撤销的是按键调整，不是命令行意图。只动可步进的三个量。
    rig.keyIntensity = baselineRig.keyIntensity;
    rig.pointIntensity = baselineRig.pointIntensity;
    rig.attenuate = baselineRig.attenuate;
}

FramePlan planFrame(ShadeMode shade, ViewMode view) {
    // 组合着色与视图成**一份**计划（(d)1）：VS 取着色模式，FS 取 planView（视图覆写或
    // 着色默认）。M6.5 的 planView 原样调用，不改其机制。提交点只消费这一份 ⇒ VS/FS 成对。
    const ShadePlan sp = planShade(shade);
    const ViewPlan vp = planView(view, sp.fragmentShader);
    return {sp.vertexShader, vp.fragmentShader, vp.sink, vp.name};
}

void prepareFrameUniforms(SceneState& scene, int width, int height,
                          sr::VertexUniforms& vu, sr::FragmentUniforms& fu) {
    // 视点相关项每帧刷新：eyeWorld 跟随相机；非 --point-pos 时点光也跟随（高光随拖拽移动）。
    scene.rig.eyeWorld = scene.camera.eye();
    if (!scene.pointPosFromCmdline) scene.rig.pointPosWorld = scene.rig.eyeWorld;
    vu.model = scene.modelMatrix;
    vu.view = scene.camera.view();
    vu.proj = sr::makePerspective(sr::kDefaultFovYRadians,
                                  static_cast<float>(width) / static_cast<float>(height),
                                  scene.nearPlane, scene.farPlane);
    // P91 单一实现 + P98 消费 bool：奇异 ⇒ 置位（HUD 显示 NM DEGENERATE），画面继续。
    scene.normalMatrixDegenerate = !sr::setNormalMatrix(vu, vu.model, "");
    // Ruling 97：两个阶段挂**同一个**对象（不是拷贝）。
    vu.lights = &scene.rig;
    fu.lights = &scene.rig;
    vu.mat = &scene.proceduralMat;
    fu.mat = &scene.proceduralMat;
}

std::string shadeHudLine(const AppOptions& o, const SceneState& scene) {
    const sr::LightRig& r = scene.rig;
    // Ka：程序几何用 proceduralMat；导入模型用**第一组**的材质（多材质时显示首组，
    // 逐组值在材质表里 —— HUD 是摘要行）。D9：Blender 常导出 Ka=0，显示出来才知道为什么没环境光。
    float ka = scene.proceduralMat.ambient.r;
    if (scene.model != nullptr && !scene.model->model.groups.empty()) {
        const int32_t mi = scene.model->model.groups[0].material;
        if (mi >= 0 && mi < static_cast<int32_t>(scene.model->model.materials.size())) {
            ka = scene.model->model.materials[static_cast<size_t>(mi)].ambient.r;
        }
    }
    // D10：光色被解码过一次 ⇒ 原始输入（o）与线性值（r）都显示，用 > 分隔。强度取 rig 活值。
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "SHADE %s  KEY %.2f,%.2f,%.2f>%.2f,%.2f,%.2f x%.2f  "
                  "POINT %.2f,%.2f,%.2f>%.2f,%.2f,%.2f x%.2f  Ka %.2f  ATT %s%s",
                  planShade(scene.shade).name,
                  static_cast<double>(o.keyColor[0]), static_cast<double>(o.keyColor[1]),
                  static_cast<double>(o.keyColor[2]),
                  static_cast<double>(r.keyColor.r), static_cast<double>(r.keyColor.g),
                  static_cast<double>(r.keyColor.b), static_cast<double>(r.keyIntensity),
                  static_cast<double>(o.pointColor[0]), static_cast<double>(o.pointColor[1]),
                  static_cast<double>(o.pointColor[2]),
                  static_cast<double>(r.pointColor.r), static_cast<double>(r.pointColor.g),
                  static_cast<double>(r.pointColor.b), static_cast<double>(r.pointIntensity),
                  static_cast<double>(ka), r.attenuate ? "ON" : "OFF",
                  scene.normalMatrixDegenerate ? "  NM DEGENERATE" : "");
    return std::string(buf);
}

}  // namespace app
