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

#include "app/cmdline.h"      // AppOptions（buildLightRig / resetLights / shadeHudLine 的原始输入）
#include "loader/model.h"
#include "math/mat.h"
#include "render/lights.h"    // LightRig / MaterialShade / litDefaultMaterialShade
#include "render/pipeline.h"  // VertexUniforms / FragmentUniforms / setNormalMatrix / shader fn 类型
#include "scene/camera.h"
#include "scene/mesh.h"

namespace app {

// M6.6：着色模式（与 M6.5 的 ViewMode 正交 —— 视图管"看哪个缓冲/法线"，着色管"怎么算颜色"）。
// 默认 Phong（D19）。Unlit 必须逐字节复现 M4/M5 的老画面（app 通路的回归锚）。
enum class ShadeMode : uint8_t { Unlit, Gouraud, Phong };

// 着色模式 → "用哪个 VS + 哪个默认 FS + HUD 名字"。P101：默认 FS 只在这里写一份，
// renderFrame 把它喂给 planView 当 defaultFS，视图不覆写时原样透传 —— 不再在提交点
// 用三元值重述 planView 已经保证的事。
struct ShadePlan {
    sr::VertexShaderFn vertexShader;
    sr::FragmentShaderFn fragmentShader;   // 该模式的默认 FS（视图覆写前）
    const char* name;                      // "UNLIT"/"GOURAUD"/"PHONG"（ASCII，位图字体）
};
ShadePlan planShade(ShadeMode mode);
// 3 键循环：Phong → Gouraud → Unlit → Phong（从默认按一下就到 Gouraud，肉眼门要用）。
ShadeMode cycleShadeMode(ShadeMode mode);
ShadeMode parseShadeMode(const std::string& s);   // parseCmdline 已校验；未知 defensive 回 Phong

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
    // 最近一次**可信**请求载入的路径（过了 .obj 后缀守卫；R 重载"同一个 path"用，内容级
    // 失败后也可重试）。坏后缀/空路径不覆盖它（P26/P27：R 不该重试一个后缀永远不会变好的
    // 请求）。empty = 从未有过可信请求。
    std::filesystem::path requested;

    // ---- M6.6：光照的单一真源（Ruling 97）----
    // rig 是 app 层**唯一**的 LightRig：renderFrame 的两个阶段都从它取指针，绝不在别处
    // 再建第二份。于是 test_scene 能断言 vu.lights == fu.lights == &scene.rig —— 把
    // "调用方把同一个对象挂到两个阶段"从约定变成可测的不变式（M6.1 的 RecordingBackend
    // 只证明了 drawModel 拷贝一致，证明不了调用方）。
    sr::LightRig rig;
    // 程序几何（model == nullptr）的逐帧材质：litDefaultMaterialShade 的副本（D15）。
    // 导入模型由 drawModel 逐组覆写，不用这一份。
    sr::MaterialShade proceduralMat = sr::litDefaultMaterialShade();
    ShadeMode shade = ShadeMode::Phong;
    // P98：setNormalMatrix 的 bool 返回从此有消费者 —— 奇异 ⇒ 置位，HUD 第三行显示
    // "NM DEGENERATE"（"该看到什么必须在画面上"；helper 的 stderr 在窗口会话里会被藏住）。
    bool normalMatrixDegenerate = false;
    // 点光位置是否来自 --point-pos（true ⇒ 世界固定；false ⇒ 每帧跟随 camera.eye()，D5）。
    bool pointPosFromCmdline = false;
    // 物体模型矩阵。今天恒为单位阵（旋转在轨道相机），但持有在这里让"奇异 model ⇒
    // normalMatrixDegenerate"这条路径可被单测触达，也给将来逐帧变化的 model 一个家。
    sr::Mat4 modelMatrix = sr::Mat4::identity();

    // 成功：换上**新建**的模型、清 errorLine、更新 label，返回 true。
    // 失败：旧 model 原样保留（先建新的再换指针，绝不先 reset()），只写 errorLine。
    // 两道守卫：入口守 .obj 后缀（大小写不敏感，argv 与拖放共用，非 .obj ⇒ 错误行，
    // 不静默忽略，A4）；loadModel 成功后守**零面**（评审 Important 1：loader 问"解析
    // 对了吗"——零面文件解析意义上合法；app 问"画面有没有东西可看"——零三角形 ⇒
    // 空屏，所以在这里判失败）。自动 refit 不在这里做 —— load 不知道窗口尺寸，
    // 由调用方在成功后按 fitEnabled 决定 refit(w,h)。
    bool load(const std::filesystem::path& obj);

    // 按当前窗口尺寸重 fit：动 distance/target/near/far 与 dolly 的夹钳区间
    // （min/maxDistance，Ruling 73：fit 是 distance 的第二个写者，夹钳必须知道 fit 的
    // 尺度）；yaw/pitch 从当前相机继承（fitCamera 的保证，D15：按 F 的人不是为了被
    // 重置视角）。model == nullptr 或退化包围盒 ⇒ 不动作（六个字段一个都不碰）。
    void refit(int width, int height);

    // 0 键：两盏灯强度 + 衰减开关恢复**内置默认**（AppOptions{} 的字段初值，单一真源）。
    // 不是恢复 CLI 覆盖值 —— "0 = 回到出厂画面"语义确定、可预期（D24）。颜色/半径不动。
    void resetLights();
};

// 从命令行原始值（sRGB 域）组装 LightRig：颜色经 decodeLightColor 解码**一次**（D5），
// keyDir 归一化，pointPos/eyeWorld 取 cam.eye()（--point-pos 覆盖 pointPos），
// attenuateRadius 默认 = bounds.radius()*3（空盒/退化 ⇒ 1.0，Ruling 49），
// 且 P97：radius 恒 >0（解析层已拒 <=0，这里再夹一道，绝不让 0 到达 shading.h 的除法）。
sr::LightRig buildLightRig(const AppOptions& o, const sr::BBox& bounds, const sr::OrbitCamera& cam);

// D24：强度**加法**步进 ±delta，夹到 [0,3] ⇒ 0 可达（"只留一盏灯"真能按出来）。
// 乘法步进永远到不了 0，且上/下不对称 —— 那是第一版的错误。
void stepIntensity(float& value, float delta);

// 组装一帧的 uniforms（Ruling 97 的可测接缝）：先刷新 rig 的视点相关项（eyeWorld，
// 以及非 --point-pos 时的 pointPosWorld），再填 model/view/proj/normalMatrix，并把
// vu.lights/fu.lights 都指向 &scene.rig、vu.mat/fu.mat 都指向 &scene.proceduralMat。
// normalMatrix 走 setNormalMatrix（P91 单一实现），其成败记进 scene.normalMatrixDegenerate（P98）。
// 采样/贴图/baseColor 由各提交分支自己填（那是 app 全局旋钮，不属于场景状态）。
void prepareFrameUniforms(SceneState& scene, int width, int height,
                          sr::VertexUniforms& vu, sr::FragmentUniforms& fu);

// D10：HUD 第三行。光色被 sRGB 解码过一次 ⇒ **输入值与线性值都显示**（只显示一个会让人
// 算错期望）。强度取 rig 的当前值（按键步进后的活值），颜色原始值取 AppOptions。
// normalMatrixDegenerate ⇒ 追加 "NM DEGENERATE"。纯字符串组装，可单测。
std::string shadeHudLine(const AppOptions& o, const SceneState& scene);

}  // namespace app
