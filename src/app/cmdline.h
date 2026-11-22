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
    // M5 的 checker.tga 资产同样是**自己的入口**（Ruling 38 的理由原样适用）：
    // 不带默认值、不与任何场景/格式选择器合并。它不渲染，只把 makeCheckerImage
    // 的字节写成 TGA（锚 2 的 provenance 入口，设计 §10）。
    std::string emitCheckerTga;
    bool help = false;
};

// 解析失败时返回 false，并把可读的原因写进 err（包含出问题的 flag 名）。
bool parseCmdline(int argc, char** argv, AppOptions& out, std::string& err);

std::string usageString();

}  // namespace app
