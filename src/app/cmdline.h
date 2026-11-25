#pragma once

#include <string>

namespace app {

// 命令行选项。这个文件**不允许**包含任何 Win32 头（C4 的例外处理见计划 Task 0.6 Step 1）：
// 它是纯字符串解析，因此可以被测试目标直接链接并逐个 flag 断言。
// **所有路径字段都是 UTF-8 语义**（D18）：main 开头用 GetCommandLineW + CommandLineToArgvW
// 把 argv 统一成 UTF-8 再喂进来（Git Bash/MSYS2 传 UTF-8，而 MSVC CRT 的窄 argv 按 ANSI
// 码页转，中文会 mojibake）。本文件与它的测试一行不改 —— 它们喂的一直是 ASCII。
// 消费侧用 app::nativePath（全仓库唯一的 UTF-8→path 转换点）转成原生路径。
struct AppOptions {
    std::string obj;      // OBJ 路径（UTF-8，见上）；空 = 用程序生成的模型
    std::string out;      // 离屏模式下写出的 BMP 路径
    std::string backend = "a";   // "a" 标量 / "b" tile+多线程
    int width = 1280;
    int height = 720;
    int frames = 1;       // 离屏模式渲染多少帧
    int threads = 0;      // 0 = 用满逻辑核，由 Backend B 解释
    int bench = 0;        // >0 = 跑 N 帧并打印分阶段耗时（隐含 offscreen）
    int autoClose = 0;    // >0 = 窗口模式渲染这么多帧后自动关闭（冒烟测试用，见 M0 notes）
    bool offscreen = false;
    float yaw = 0.0f;
    float pitch = 0.35f;
    float distance = 5.0f;   // 轨道相机的初值（离屏渲染要确定性，所以必须能从命令行给）
    bool gradient = false;   // 背景用 M0 的全屏渐变（默认纯色 clear，因为渐变要吃掉一半帧预算）
    bool noFit = false;
    std::string goldenWrite;   // 非空 = 渲染 golden 场景（M3，无纹理）并写到这个路径
    // M4 的参考图有**自己的入口**，而不是给 --golden-write 加一个"场景选择"：
    // 带默认值的场景选择意味着"忘了指定场景"会把 M4 的图画进 M3 的路径（或反之），
    // 而它的症状是"重新生成基准图之后一切正常"。两个显式入口 = 没有默认值 = 没有这个坑。
    std::string goldenWriteTextured;
    // M5.7 的 OBJ golden 同样是**自己的入口**（Ruling 38）：它必须与 --obj 配对
    // （缺 --obj ⇒ 明确报错，没有默认场景），输出只是给人看的图 —— 锚 3 的比对在
    // 进程内对着既有 M4 参考图做，**不新增参考图**（收紧 2）。
    std::string goldenWriteObj;
    // M5 的 checker.tga 资产同样是**自己的入口**（Ruling 38 的理由原样适用）：
    // 不带默认值、不与任何场景/格式选择器合并。它不渲染，只把 makeCheckerImage
    // 的字节写成 TGA（锚 2 的 provenance 入口，设计 §10）。
    std::string emitCheckerTga;
    bool help = false;

    // ---- M6.6：光照参数（G-M6.4）。**一律存原始输入值（sRGB 域）** ----
    // 解码只发生在 buildLightRig 一处（scene.cpp），避免"解两次"（D5/Review Focus 5）。
    // 强度是线性乘子，不走解码；颜色三元组是 display-referred sRGB 浮点（与 MTL Kd 同源）。
    std::string shade = "phong";        // unlit | gouraud | phong（默认 phong，D19）
    float keyColor[3] = {1.0f, 1.0f, 1.0f};   // --key-color（sRGB）
    // --key-dir：方向光"照向"的方向（原始值，buildLightRig 归一化）。默认与默认视线约 35°
    // （§6.1 缓解头灯式点光把凹凸照平）：朝光源 = normalize(0.3,0.8,0.6)，与 eye 方向
    // (0,sin0.35,cos0.35) 夹角 ≈ 36.6°。世界空间固定 ⇒ 拖拽时高光随视点移动。
    float keyDir[3] = {-0.3f, -0.8f, -0.6f};
    float keyIntensity = 0.35f;         // --key-intensity（D19 默认）
    float pointColor[3] = {1.0f, 1.0f, 1.0f};   // --point-color（sRGB）
    float pointPos[3] = {0.0f, 0.0f, 0.0f};     // --point-pos（世界坐标）
    bool pointPosGiven = false;         // 未给 ⇒ 点光跟随相机 eye（D5）
    // D19：默认参数必须让高光**不饱和**。简报初稿 pointIntensity=1.0，但无衰减时点光在
    // 正对相机的球面点 N·L=1 ⇒ 漫反射 alone ≈1.0 再叠高光必撞 255。按 D19"改默认不改断言"
    // 下调到 0.4（峰值 ≈0.84 线性 ⇒ ~238 字节，留余量；推导见 test_scene 与本报告）。
    float pointIntensity = 0.4f;        // --point-intensity
    float ambientIntensity = 1.0f;      // --ambient：环境光强度（线性乘子，乘到解码后的环境色）
    float ambientColor[3] = {1.0f, 1.0f, 1.0f};   // --ambient-color（sRGB）
    bool attenuate = false;             // --attenuate / A 键（D7 默认关）
    // --attenuate-radius：<0 = 哨兵"用默认 = bounds.radius()*3"（空盒 ⇒1.0，Ruling 49）。
    // 解析层拒绝 <=0（P97：不许 0 到达 shading.h 的除法）；buildLightRig 再夹一道防御。
    float attenuateRadius = -1.0f;
};

// 解析失败时返回 false，并把可读的原因写进 err（包含出问题的 flag 名）。
bool parseCmdline(int argc, char** argv, AppOptions& out, std::string& err);

std::string usageString();

}  // namespace app
