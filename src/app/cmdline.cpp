#include "app/cmdline.h"

#include <charconv>
#include <cstdlib>   // strtof
#include <string>
#include <vector>

// 这个 .cpp 里出现 <windows.h> 就是设计文档 C4 被破坏。它之所以能被测试目标直接链接，
// 靠的正是这一点 —— 见计划 Task 0.6 Step 1 与 CMakeLists 里的 tests 源文件列表。

namespace app {
namespace {

constexpr int kMinDimension = 16;
constexpr int kMaxDimension = 16384;

bool parseSignedInt(const char* text, int& value) {
    const char* first = text;
    const char* last = text + std::string(text).size();
    int out = 0;
    const std::from_chars_result r = std::from_chars(first, last, out);
    if (r.ec != std::errc{} || r.ptr != last) return false;  // 尾部有多余字符也算错
    value = out;
    return true;
}

struct Parser {
    int argc;
    char** argv;
    std::string& err;

    // nextInt/nextString 是所有带值选项的入口：缺值、非数字、越界三类错误都带上 flag 名，
    // 因为"哪个 flag 写坏了"是用户唯一能立刻行动的信息。

    bool fail(const std::string& message) {
        if (err.empty()) err = message;
        return false;
    }

    // 两个入口都先 ++index 跳过 flag 本身，再取下一个参数当值。
    // （第一版忘了这一步，症状是 `--width 640` 报"需要一个整数，收到 '--width'"，
    //  而 `--obj foo.obj` 静默把 obj 设成了 "--obj" —— 后者更危险，因为它不报错。）
    bool nextFloat(const char* flag, float& dst) {
        ++index;
        if (index >= argc) return fail(std::string(flag) + " 缺少值");
        char* end = nullptr;
        const float v = std::strtof(argv[index], &end);
        if (end == argv[index] || *end != 0) {
            return fail(std::string(flag) + " 需要一个数，收到 '" + argv[index] + "'");
        }
        dst = v;
        return true;
    }

    bool nextInt(const char* flag, int& dst) {
        ++index;
        if (index >= argc) return fail(std::string(flag) + " 缺少值");
        int parsed = 0;
        if (!parseSignedInt(argv[index], parsed)) {
            return fail(std::string(flag) + " 需要一个整数，收到 '" + argv[index] + "'");
        }
        dst = parsed;
        return true;
    }

    bool nextString(const char* flag, std::string& dst) {
        ++index;
        if (index >= argc) return fail(std::string(flag) + " 缺少值");
        dst = argv[index];
        return true;
    }

    int index = 1;
};

}  // namespace

bool parseCmdline(int argc, char** argv, AppOptions& out, std::string& err) {
    err.clear();
    out = AppOptions{};
    Parser p{argc, argv, err};

    for (p.index = 1; p.index < argc; ++p.index) {
        const char* arg = argv[p.index];
        const std::string a(arg);

        if (a == "--help" || a == "-h") {
            out.help = true;
            continue;
        }
        if (a == "--offscreen") {
            out.offscreen = true;
            continue;
        }
        if (a == "--no-fit") {
            out.noFit = true;
            continue;
        }
        if (a == "--gradient") {
            out.gradient = true;
            continue;
        }

        if (a == "--obj") {
            if (!p.nextString("--obj", out.obj)) return false;
            continue;
        }
        if (a == "--golden-write") {
            if (!p.nextString("--golden-write", out.goldenWrite)) return false;
            out.offscreen = true;   // 写 golden 不需要窗口
            continue;
        }
        // M4 的参考图有自己的入口，理由见 cmdline.h 的字段注释（Ruling 38）。
        if (a == "--golden-write-textured") {
            if (!p.nextString("--golden-write-textured", out.goldenWriteTextured)) return false;
            out.offscreen = true;
            continue;
        }
        if (a == "--out") {
            if (!p.nextString("--out", out.out)) return false;
            continue;
        }
        if (a == "--backend") {
            std::string b;
            if (!p.nextString("--backend", b)) return false;
            if (b != "a" && b != "b") return p.fail("--backend 只接受 a 或 b，收到 '" + b + "'");
            out.backend = b;
            continue;
        }
        if (a == "--width" || a == "--height") {
            int v = 0;
            if (!p.nextInt(a.c_str(), v)) return false;
            if (v < kMinDimension || v > kMaxDimension) {
                return p.fail(std::string(a) + " 必须在 [" + std::to_string(kMinDimension) + ", " +
                              std::to_string(kMaxDimension) + "] 之间，收到 " + std::to_string(v));
            }
            if (a == "--width") out.width = v; else out.height = v;
            continue;
        }
        if (a == "--frames") {
            int v = 0;
            if (!p.nextInt("--frames", v)) return false;
            if (v < 1) return p.fail("--frames 必须 >= 1，收到 " + std::to_string(v));
            out.frames = v;
            continue;
        }
        if (a == "--yaw" || a == "--pitch" || a == "--distance") {
            float v = 0.0f;
            if (!p.nextFloat(a.c_str(), v)) return false;
            if (a == "--yaw") out.yaw = v;
            else if (a == "--pitch") out.pitch = v;
            else {
                if (!(v > 0.0f)) return p.fail("--distance 必须为正，收到 " + std::to_string(v));
                out.distance = v;
            }
            continue;
        }
        if (a == "--auto-close") {
            int v = 0;
            if (!p.nextInt("--auto-close", v)) return false;
            if (v < 1) return p.fail("--auto-close 必须 >= 1，收到 " + std::to_string(v));
            out.autoClose = v;
            continue;
        }
        if (a == "--threads") {
            int v = 0;
            if (!p.nextInt("--threads", v)) return false;
            if (v < 0) return p.fail("--threads 不能为负，收到 " + std::to_string(v));
            out.threads = v;
            continue;
        }
        if (a == "--bench") {
            int v = 0;
            if (!p.nextInt("--bench", v)) return false;
            if (v < 1) return p.fail("--bench 必须 >= 1，收到 " + std::to_string(v));
            out.bench = v;
            out.offscreen = true;   // bench 是离屏测量入口，不需要再写 --offscreen
            continue;
        }

        if (!a.empty() && a[0] == '-') {
            return p.fail("未知选项 '" + a + "'（--help 看可用选项）");
        }
        return p.fail("多余参数 '" + a + "'（这个程序只接受选项，不接受位置参数）");
    }

    // 两个 golden 入口同时给 ⇒ 拒绝。放在循环**之后**而不是分支里，是因为这里要判的是
    // "两个都非空"这个整体形状：写在分支里就得依赖参数顺序（先看到谁就赢），
    // 而那正是我们要消灭的东西 —— 静默忽略第二个入口的症状是"我以为两张都刷新了"。
    if (!out.goldenWrite.empty() && !out.goldenWriteTextured.empty()) {
        return p.fail("--golden-write 与 --golden-write-textured 不能同时给"
                      "（一次只刷一张参考图；两张都要就分两次跑）");
    }

    // 离屏模式没给 --out 时给一个默认名：这样 `--bench 60` 这类纯测量的调用
    // 不会因为没有输出文件而报错，也不会有人误以为"没写 --out 就会弹窗"。
    if (out.offscreen && out.out.empty()) out.out = "out.bmp";
    return true;
}

std::string usageString() {
    return "soft-raster — 纯 CPU 软件光栅化渲染器\n"
           "用法: soft-raster [选项]\n"
           "  --obj <path>       载入 OBJ 模型（默认用程序生成的模型）\n"
           "  --width <N>        窗口/离屏宽度，默认 1280\n"
           "  --height <N>       窗口/离屏高度，默认 720\n"
           "  --offscreen        不开窗口，渲染 --frames 帧后写出 --out\n"
           "  --frames <N>       离屏帧数，默认 1\n"
           "  --out <path.bmp>   离屏输出文件，默认 out.bmp\n"
           "  --bench <N>        离屏跑 N 帧并打印分阶段耗时（隐含 --offscreen）\n"
           "  --backend <a|b>    a=标量逐像素，b=tile+多线程（M8 起生效）\n"
           "  --threads <N>      Backend B 的线程数，0=用满逻辑核\n"
           "  --gradient         背景用全屏渐变（默认纯色；渐变每帧要吃 width*height 次编码）"
           "\n"
           "  --no-fit           载入模型后不自动调整相机\n"
           "  --yaw <rad>        轨道相机初始偏航（离屏渲染要确定性，所以必须能从命令行给）\n"
           "  --pitch <rad>      轨道相机初始俯仰\n"
           "  --distance <N>     轨道相机初始距离，必须为正\n"
           "  --auto-close <N>   窗口模式渲染 N 帧后自动关闭（冒烟测试用，不改渲染内容）\n"
           "  --golden-write <path.bmp>\n"
           "                     渲染 M3 golden 场景（顶点色立方体）并写出，隐含 --offscreen\n"
           "  --golden-write-textured <path.bmp>\n"
           "                     渲染 M4 golden 场景（贴图立方体 + 三线性）并写出。\n"
           "                     与上面那个是**两个入口**而不是一个带默认值的场景选择：\n"
           "                     后者会把 M4 的图画进 M3 的路径，而症状是\"重新生成后一切正常\"\n"
           "  --help, -h         显示本帮助\n";
}

}  // namespace app
