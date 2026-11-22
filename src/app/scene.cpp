#include "app/scene.h"

#include <utility>

#include "loader/lex.h"   // displayPath（Ruling 62：消息里的路径不用 .string()）
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
    requested = obj;   // R 重载"同一个 path"；失败后也能重试（修好文件再按 R）
    if (!hasObjExtension(obj)) {
        errorLine = sr::lex::displayPath(obj) + ": 只支持 .obj 模型";
        return false;
    }
    // **先建新的再换指针**（绝不先 model.reset()）：失败时 fresh 直接丢弃，
    // 旧模型原样留在画面上 —— "失败保留旧模型"不是靠回滚，是靠根本没动过旧的。
    auto fresh = std::make_unique<sr::LoadedModel>();
    std::string err;
    if (!sr::loadModel(obj, *fresh, err)) {
        errorLine = err;   // loadModel 的 err 已带文件名/行号（displayPath，UTF-8 无损）
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
    // fov 用 kDefaultFovYRadians：renderFrame 的 makePerspective 取的是同一个常量，
    // 两边漂移的症状是"某些模型装不下"（P24）。退化 aspect 由 fitCamera 入口守卫兜住。
    const float aspect = static_cast<float>(width) / static_cast<float>(height);
    const sr::FittedCamera f =
        sr::fitCamera(model->model.mesh.bounds, sr::kDefaultFovYRadians, aspect, camera);
    camera = f.camera;   // fitCamera 只动 distance/target；yaw/pitch 从 base 继承（D15）
    nearPlane = f.nearPlane;
    farPlane = f.farPlane;
}

}  // namespace app