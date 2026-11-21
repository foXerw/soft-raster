#pragma once

#include <string>

namespace app {

// 命令行选项。这个文件**不允许**包含任何 Win32 头（C4 的例外处理见计划 Task 0.6 Step 1）：
// 它是纯字符串解析，因此可以被测试目标直接链接并逐个 flag 断言。
struct AppOptions {
    std::string obj;      // OBJ 路径；空 = 用程序生成的模型
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
    std::string goldenWrite;   // 非空 = 渲染 golden 场景并写到这个路径
    bool help = false;
};

// 解析失败时返回 false，并把可读的原因写进 err（包含出问题的 flag 名）。
bool parseCmdline(int argc, char** argv, AppOptions& out, std::string& err);

std::string usageString();

}  // namespace app
