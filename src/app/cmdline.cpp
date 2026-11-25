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
        // M5.7 的 OBJ golden 入口（Ruling 38：独立 flag、不带默认场景）。
        // 与 --obj 的配对检查放在循环之后：这里只收值，整体形状最后判。
        if (a == "--golden-write-obj") {
            if (!p.nextString("--golden-write-obj", out.goldenWriteObj)) return false;
            out.offscreen = true;
            continue;
        }
        // M5 的 checker.tga 资产入口（Ruling 38：独立 flag、不带默认值）。
        // 它不渲染，所以**不**隐含 --offscreen —— main 里写完即返回。
        if (a == "--emit-checker-tga") {
            if (!p.nextString("--emit-checker-tga", out.emitCheckerTga)) return false;
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

        // ---- M6.6 光照 flag（G-M6.4）。值一律原始 sRGB 域，解码归 buildLightRig ----
        // 三元组复用 nextFloat 三次：缺第 2/3 个分量同样点名 flag（不静默补 0）。
        if (a == "--key-color" || a == "--point-color" || a == "--ambient-color" ||
            a == "--key-dir" || a == "--point-pos") {
            float* dst = a == "--key-color" ? out.keyColor
                       : a == "--point-color" ? out.pointColor
                       : a == "--ambient-color" ? out.ambientColor
                       : a == "--key-dir" ? out.keyDir
                       : out.pointPos;
            for (int c = 0; c < 3; ++c) {
                if (!p.nextFloat(a.c_str(), dst[c])) return false;
            }
            if (a == "--point-pos") out.pointPosGiven = true;
            continue;
        }
        if (a == "--key-intensity" || a == "--point-intensity" || a == "--ambient") {
            float v = 0.0f;
            if (!p.nextFloat(a.c_str(), v)) return false;
            if (!(v >= 0.0f)) return p.fail(std::string(a) + " 不能为负，收到 " + std::to_string(v));
            // R100（P103）：两个可按键步进的强度在**解析时**拒绝 >3。运行时夹钳 [0,3]
            // 是按键步进的边界，不是静默改写命令行的理由（与 R99 同构：命令行是明确
            // 表达的意图）。--ambient 没有按键步进 ⇒ 不设这个上限。
            if ((a == "--key-intensity" || a == "--point-intensity") && v > 3.0f) {
                return p.fail(std::string(a) + " 必须在 [0, 3] 之间（按键步进的夹钳范围），收到 " +
                              std::to_string(v));
            }
            if (a == "--key-intensity") out.keyIntensity = v;
            else if (a == "--point-intensity") out.pointIntensity = v;
            else out.ambientIntensity = v;
            continue;
        }
        if (a == "--attenuate") {
            out.attenuate = true;   // 布尔开关，无值（D7）
            continue;
        }
        if (a == "--attenuate-radius") {
            float v = 0.0f;
            if (!p.nextFloat("--attenuate-radius", v)) return false;
            // P97：radius>0 是 shading.h 除法的前提，必须在边界拒绝，不许 0/负数到达逐像素代码。
            if (!(v > 0.0f)) return p.fail("--attenuate-radius 必须为正，收到 " + std::to_string(v));
            out.attenuateRadius = v;
            continue;
        }
        if (a == "--shade") {
            std::string s;
            if (!p.nextString("--shade", s)) return false;
            if (s != "unlit" && s != "gouraud" && s != "phong") {
                return p.fail("--shade 只接受 unlit|gouraud|phong，收到 '" + s + "'");
            }
            out.shade = s;
            continue;
        }

        if (!a.empty() && a[0] == '-') {
            return p.fail("未知选项 '" + a + "'（--help 看可用选项）");
        }
        return p.fail("多余参数 '" + a + "'（这个程序只接受选项，不接受位置参数）");
    }

    // 三个 golden 入口同时给 ⇒ 拒绝。放在循环**之后**而不是分支里，是因为这里要判的是
    // "多个都非空"这个整体形状：写在分支里就得依赖参数顺序（先看到谁就赢），
    // 而那正是我们要消灭的东西 —— 静默忽略第二个入口的症状是"我以为两张都刷新了"。
    const int goldenEntries = (!out.goldenWrite.empty() ? 1 : 0) +
                              (!out.goldenWriteTextured.empty() ? 1 : 0) +
                              (!out.goldenWriteObj.empty() ? 1 : 0);
    if (goldenEntries > 1) {
        return p.fail("--golden-write / --golden-write-textured / --golden-write-obj 不能同时给"
                      "（一次只刷一张图；都要就分多次跑）");
    }

    // OBJ golden 必须与 --obj 配对（M5.7）：缺 --obj ⇒ 明确报错，**没有默认场景**。
    // 同样放在循环之后，与参数顺序无关。
    if (!out.goldenWriteObj.empty() && out.obj.empty()) {
        return p.fail("--golden-write-obj 需要 --obj 指定要载入的模型（没有默认场景）");
    }

    // 资产入口与三个 golden 入口互斥，理由与上面那条相同：main 里 emit-checker-tga
    // 分支先返回，同时给 golden 入口会被静默忽略 ——"我以为参考图也刷新了"没有别的信号。
    if (!out.emitCheckerTga.empty() && goldenEntries > 0) {
        return p.fail("--emit-checker-tga 与 --golden-write/--golden-write-textured/"
                      "--golden-write-obj 不能同时给（一次只做一件事；都要就分多次跑）");
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
           "  --golden-write-obj <path.bmp>\n"
           "                     载入 --obj 指定的模型，按 M4 golden 的固定姿态渲染并写出\n"
           "                     （M5.7 锚 3 的人眼查看入口；比对在测试里进程内做，不新增\n"
           "                     参考图）。必须与 --obj 同时给，缺 --obj 报错而不是默认场景\n"
           "  --emit-checker-tga <path.tga>\n"
           "                     把程序生成的 checker（makeCheckerImage，128x128）写成\n"
           "                     未压缩 32bpp TGA（bottom-up）。不渲染；assets/tga/checker.tga\n"
           "                     的 provenance 入口（M5 锚 2）\n"
           "  --shade <mode>     着色模式 unlit|gouraud|phong，默认 phong（3 键运行时循环）\n"
           "  --key-color <r g b>      方向光颜色（sRGB 0..1），默认 1 1 1\n"
           "  --key-dir <x y z>        方向光\"照向\"的方向（世界空间，自动归一化）\n"
           "  --key-intensity <f>      方向光强度（线性乘子），默认 0.35，范围 [0,3]（-/= 键 ±0.1）\n"
           "  --point-color <r g b>    点光颜色（sRGB 0..1），默认 1 1 1\n"
           "  --point-pos <x y z>      点光世界坐标；不给则跟随相机 eye（[/] 键调强度）\n"
           "  --point-intensity <f>    点光强度（线性乘子），默认 0.4，范围 [0,3]（[/] 键 ±0.1）\n"
           "  --ambient <f>            环境光强度（线性乘子），默认 1.0\n"
           "  --ambient-color <r g b>  环境光颜色（sRGB 0..1），默认 1 1 1\n"
           "  --attenuate              打开点光距离衰减（A 键切换；默认关）\n"
           "  --attenuate-radius <f>   衰减半径（必须为正）；默认 = 场景半径 ×3\n"
           "  --help, -h         显示本帮助\n";
}

}  // namespace app
