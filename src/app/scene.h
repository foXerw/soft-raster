#pragma once
// 场景持有者（M5.6）：app 的"画什么 + 从哪看 + 出了什么错"状态。
//
// **本文件不含任何 Win32 头**（cmdline.cpp 先例，C4 的例外处理同形）：它只认识
// loader/scene 的概念，因此能被 tests 目标直接链接（CMakeLists 的 tests 源列表里
// 有 scene.cpp），load/refit 的行为可以逐条单测。Win32 那一半（按键、拖放消息）
// 留在 win32_window.cpp，由窗口冒烟与产品方肉眼门兜底。
//
// 与 loader 的分工：loadModel/drawModel（M5.4）管"一个模型怎么读进来、怎么画"；
// 这里管"哪个模型在画面上、失败了画面不许空"。失败保留旧模型是 A1（半加载比
// 没有更糟）在 app 层的对应物：看不见模型比看见错误更糟，但看见**空白**比两者都糟。
#include <filesystem>
#include <memory>
#include <string>

#include "loader/model.h"
#include "scene/camera.h"
#include "scene/mesh.h"

namespace app {

// 程序生成的兜底立方体（model == nullptr 时的 M2–M4 老画面）。进程内单例，与
// goldenCheckerTexture 同一条纪律：窗口里看到的兜底模型与测试里断言的是同一个对象。
// makeCube/applyPositionColors 都是 core，本 TU 仍不含 Win32。
const sr::Mesh& fallbackCube();

struct SceneState {
    std::unique_ptr<sr::LoadedModel> model;   // nullptr = 画程序生成的立方体（M2–M4 的老画面）
    sr::OrbitCamera camera;
    float nearPlane = 0.1f;
    float farPlane = 100.0f;
    // fitEnabled 只门控"载入成功后的**自动** refit"（由调用方检查）；F 键是显式手动
    // 请求，总是 refit。--no-fit 的语义是"载入后不自动调整相机"（usage 原文），
    // 不是"禁止 fit"—— 按 F 的人在抱怨模型太大/出屏，不是在请求被拒绝。
    bool fitEnabled = true;
    std::string errorLine;   // 空 = 无错误；成功载入清空；失败写新值（HUD 第二行整行给它）
    std::string modelLabel = "CUBE";   // HUD 用：文件名（UTF-8）或 "CUBE"，跟着画面上的模型走
    // 最近一次请求载入的路径（R 重载"同一个 path"用；失败后也可重试）。empty = 从未请求。
    std::filesystem::path requested;

    // 成功：换上**新建**的模型、清 errorLine、更新 label，返回 true。
    // 失败：旧 model 原样保留（先建新的再换指针，绝不先 reset()），只写 errorLine。
    // 入口守 .obj 后缀（大小写不敏感）：argv 与拖放共用这一道，非 .obj ⇒ 错误行，
    // 不静默忽略（A4）。自动 refit 不在这里做 —— load 不知道窗口尺寸，由调用方
    // 在成功后按 fitEnabled 决定 refit(w,h)。
    bool load(const std::filesystem::path& obj);

    // 按当前窗口尺寸重 fit：只动 distance/target/near/far，yaw/pitch 从当前相机继承
    // （fitCamera 的保证，D15：按 F 的人不是为了被重置视角）。model == nullptr ⇒ 不动作。
    void refit(int width, int height);
};

}  // namespace app