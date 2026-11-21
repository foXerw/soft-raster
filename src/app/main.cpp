#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// 顺序与宏都有作用：WIN32_LEAN_AND_MEAN 少拉一坨头，
// NOMINMAX 保住 std::min/std::max —— windows.h 的 min/max 宏会把
// `std::min(best, v)` 解析成成员访问，报错信息还特别难看懂（C2589）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app/cmdline.h"
#include "app/input.h"
#include "app/surface.h"
#include "bench/timer.h"
#include "loader/bmp_writer.h"
#include "math/transform.h"
#include "render/builtin_shaders.h"
#include "render/color.h"
#include "render/render_backend.h"
#include "render/sampler.h"
#include "render/sink.h"
#include "render/overlay_font.h"
#include "render/patterns.h"
#include "scene/golden_scene.h"
#include "scene/camera.h"
#include "scene/mesh.h"
#include "render/raster_scalar.h"
#include "render/render_target.h"
#include "version.h"

#ifndef SR_VERSION
#define SR_VERSION "0.0.0-unspecified"
#endif

namespace {

const wchar_t* kWindowTitle = L"soft-raster — 纯 CPU 软件光栅化";

[[maybe_unused]] constexpr float kTop = 0.06f;   // 一个"有得看"的渐变，用来确认像素真的在流动
[[maybe_unused]] constexpr float kBottom = 0.85f;

// 渲染一整帧。M0 还没有任何 3D：能证明"像素从我们手里走到了屏幕"就够。
// 从这里到 M2 之间唯一的变化是"这一帧的内容由谁产生"。
bool g_gradientBackground = false;   // 由 --gradient 打开
bool g_showBackfaces = false;   // 临时开关：M7 会做成正式的视图选项
sr::DebugViewSink g_sink;       // 'V' 循环：color → depth → gray

// M4 的两个采样旋钮（`1` 与 `2`）。它们是**应用状态**而不是管线状态：
// PipelineState 每帧重建，而这两个变量的生命期是整个程序 —— 暂停时反复按也保持得住。
// 两个旋钮正交（Ruling 31）：`1` 关 mip 时 `2` 依然在三线性/双线性/最近邻之间循环，
// "关 mip + 三线性"的语义是"永远在 level 0 上做双线性"，不是"退回最近邻"。
bool g_useMip = true;
sr::FilterMode g_filter = sr::FilterMode::Trilinear;

const char* filterName(sr::FilterMode f) {
    switch (f) {
        case sr::FilterMode::Nearest: return "NEAREST";
        case sr::FilterMode::Bilinear: return "BILINEAR";
        case sr::FilterMode::Trilinear: return "TRILINEAR";
    }
    return "?";
}

// 循环顺序固定 NEAREST → BILINEAR → TRILINEAR → NEAREST：
// 与"从最便宜到最贵"一致，按两下就能从"看锯齿"走到"看 mip"，不用绕远路。
sr::FilterMode nextFilter(sr::FilterMode f) {
    return (f == sr::FilterMode::Nearest)
               ? sr::FilterMode::Bilinear
               : (f == sr::FilterMode::Bilinear) ? sr::FilterMode::Trilinear
                                                 : sr::FilterMode::Nearest;
}

// 转台速度（弧度/秒）。0.6 ≈ 每秒 34°，一圈约 10 秒：慢到能看清形状，
// 又快到让人觉得画面是活的。
constexpr float kTurntableRadiansPerSecond = 0.6f;
constexpr int kTargetFps = 60;

void renderFrame(sr::RenderTarget& target, float phase, const std::string& statusLine,
                 const sr::OrbitCamera& camera) {
    target.depth.clear();
    // 背景从"全屏渐变填充"换成纯色 clear。
    // 渐变是 M0 的验收物（当时除了它没有别的东西可画），但它每帧要做 width*height 次
    // encodeSRGB + writePacked：实测 640x360 下整帧 29ms，其中大半是背景，
    // 于是自动旋转只有 35fps —— 用户说的"不丝滑"其实主要是这件事，不是节流的锅。
    // 现在画面本身在动，背景只需要把立方体衬出来。渐变仍可用 --gradient 打开。
    if (g_gradientBackground) {
        const sr::LinearColor a{kTop, kTop * 0.55f, kBottom * 0.35f};
        const sr::LinearColor b{kBottom, kBottom * 0.7f, kTop * 0.4f};
        sr::fillLinearGradient(target.color, a, b, phase);
    } else {
        target.color.clear(sr::encodeSRGB({0.045f, 0.05f, 0.07f}));
    }

    // M2 的可见成果：一个 MVP 变换的旋转立方体，面纯色，**故意不启用深度**。
    // 剔除开着时它是干净的（凸体 + 剔除已经解决了遮挡）；把 cull 关掉就能看到
    // 背面透出来盖住正面 —— 那是 painter's 算法的缺陷，也是 M3 的动机。
    {
        static sr::BackendA backend;
        static sr::Mesh cube = [] {
            sr::Mesh m = sr::makeCube(1.6f, true);
            sr::applyPositionColors(m);   // M3：位置编码色，插值不连续就看得见
            return m;
        }();

        sr::PipelineState st;
        st.target = &target;
        st.vertexShader = sr::transformVS;
        // M4 的可见成果：贴一张程序生成的 checker。
        // 顶点色路径（vertexColorFS）在这里退出画面，但 Mesh 里的 colors 仍然保留 ——
        // M6 的光照 shader 还要它，而且 W 键看背面时"哪一面是哪个颜色"仍然读得出来。
        st.fragmentShader = sr::texturedFS;
        st.depthTest = true;
        st.depthWrite = true;
        st.cull.backfaceCull = !g_showBackfaces;
        // baseColor 是乘上去的材质 tint，不是兜底色 ⇒ 给白，否则参考图与窗口都会暗一档。
        st.fu.baseColor = sr::LinearColor{1.0f, 1.0f, 1.0f};
        // 贴图与 golden 用**同一个单例**：窗口里看到的那张贴图就是参考图里那一张。
        // 这不是偷懒，是让"我在窗口里验收过的画面"与"golden 守住的画面"是同一个东西 ——
        // 两者各自建一张图时，改了一边的参数不会让另一边变红。
        st.fu.texture = &sr::goldenCheckerTexture();
        st.fu.sampler.wrapU = sr::WrapMode::ClampEdge;
        st.fu.sampler.wrapV = sr::WrapMode::ClampEdge;   // 理由见 golden_scene.cpp 的 goldenSampler()
        st.fu.sampler.filter = g_filter;
        st.fu.sampler.useMip = g_useMip;


        // 物体不再自转：旋转交给轨道相机，鼠标拖拽直接改变 yaw/pitch。
        // "转相机"与"转物体"在画面上等价，但只有前者能让用户觉得自己抓住了物体。
        st.vu.model = sr::Mat4::identity();
        st.vu.view = camera.view();
        st.vu.proj = sr::makePerspective(60.0f * 3.14159265f / 180.0f,
                                         static_cast<float>(target.color.width) /
                                             static_cast<float>(target.color.height),
                                         0.1f, 100.0f);
        st.vu.normalMatrix = sr::Mat4::identity();   // model 是单位阵，法线矩阵也就是单位阵
        backend.draw(cube, st);
    }

    // HUD 文字由我们自己画（设计文档 §10.3：屏幕上不该出现一个不是我们算出来的像素）。
    // 离屏路径也画它，这样"字体对不对"能靠一张 BMP 检查，而不必等人盯着窗口看。
    const uint32_t fg = sr::encodeSRGB({1.0f, 1.0f, 1.0f});
    const uint32_t bg = sr::encodeSRGB({0.015f, 0.015f, 0.02f});
    // 调试视图在文字之前应用：HUD 必须永远可读，否则切到深度视图时文字被一起冲掉。
    g_sink.present(target);

    sr::drawText(target.color, 4, 4, statusLine, fg, bg, 2);
    sr::drawText(target.color, 4, 4 + 2 * sr::kGlyphHeightPx + 4,
                 "P=PAUSE S=SHOT V=VIEW 1=MIP 2=FILT ESC=QUIT",
                 sr::encodeSRGB({0.9f, 0.85f, 0.6f}), bg, 1);
}

int saveScreenshot(const sr::RenderTarget& target, const std::string& path, std::string& err) {
    // writeBmp 需要一个非常量的 ColorTarget（视图对象本身是值语义，不修改它），
    // 所以这里拷贝一份视图而不是 const_cast。
    sr::ColorTarget copy = target.color;
    if (!sr::writeBmp(path, copy, err)) {
        std::printf("[screenshot] 失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("[screenshot] 已写出 %s (%dx%d)\n", path.c_str(), copy.width, copy.height);
    return 0;
}

// 两个 golden 入口共用"建离屏面 → 渲一帧 → 存图"这一段，唯一区别是**调用哪个场景函数**。
// 场景函数以指针传入而不是复制两份：参考图必须由生产代码里的那一个函数生成，
// 而"哪个函数生成了它"要能从打印出来的标签里读出来（标签与函数搞错的代价是一整帧白比对）。
int writeGolden(const std::string& label, const std::string& path,
                void (*scene)(sr::RenderTarget&, sr::FrameStats*)) {
    auto surface = app::createOffscreenSurface(sr::kGoldenWidth, sr::kGoldenHeight);
    sr::FrameStats stats;
    scene(surface->target(), &stats);
    std::string err;
    if (saveScreenshot(surface->target(), path, err) != 0) return 1;
    const std::string line = "[golden] " + label + " → " + path + " 由 " + sr::buildInfoString();
    std::puts(line.c_str());
    return 0;
}

int runOffscreen(const app::AppOptions& o) {
    g_gradientBackground = o.gradient;   // 离屏默认也走纯色，golden 场景自己决定背景
    auto surface = app::createOffscreenSurface(o.width, o.height);
    sr::RenderTarget& target = surface->target();

    sr::Timer total;
    std::vector<double> frameMs;
    frameMs.reserve(static_cast<size_t>(o.frames));

    for (int i = 0; i < o.frames; ++i) {
        sr::Timer frameTimer;
        const std::string status = "OFFSCREEN FRAME " + std::to_string(i + 1) + "/" +
                                   std::to_string(o.frames) + "  " + std::to_string(o.width) + "X" +
                                   std::to_string(o.height);
        sr::OrbitCamera camera;
        camera.yaw = o.yaw;
        camera.pitch = o.pitch;
        camera.distance = o.distance;
        renderFrame(target, static_cast<float>(i) / static_cast<float>(o.frames), status, camera);
        frameMs.push_back(frameTimer.elapsedMs());
    }
    const double ms = total.elapsedMs();

    const std::string outPath = o.out.empty() ? "out.bmp" : o.out;
    std::string err;
    if (saveScreenshot(target, outPath, err) != 0) return 1;

    double sum = 0.0;
    double best = 1e30, worst = 0.0;
    for (const double v : frameMs) {
        sum += v;
        best = std::min(best, v);
        worst = std::max(worst, v);
    }
    std::printf("[offscreen] %s\n", sr::buildInfoString());
    std::printf("[offscreen] %dx%d, %d 帧, 平均 %.3f ms/帧 (最快 %.3f / 最慢 %.3f), 总 %.1f ms\n",
                o.width, o.height, o.frames, sum / static_cast<double>(frameMs.size()), best, worst,
                ms);
    if (o.bench > 0) {
        std::printf("[bench] 分阶段计时要到 M2 才有内容可测；现在只有整帧时间。\n");
    }
    if (!o.obj.empty()) {
        std::printf("[提示] --obj 从 M5 开始生效，现在会忽略它。\n");
    }
    return 0;
}

int runWindow(const app::AppOptions& o) {
    std::string err;
    auto surface = app::createWindowSurface(o.width, o.height, kWindowTitle, err);
    if (surface == nullptr) {
        std::printf("[错误] 无法创建窗口: %s\n", err.c_str());
        return 1;
    }
    if (o.backend == "b") {
        std::printf("[提示] --backend b 要到 M8 才存在，现在总是用标量后端 a。\n");
    }
    if (!o.obj.empty()) {
        std::printf("[提示] --obj 从 M5 开始生效，现在会忽略它。\n");
    }
    std::printf("[window] %s — %dx%d\n", sr::buildInfoString(), o.width, o.height);
    std::printf("[window] P=暂停 O=单步 S=截图 V=视图 Esc=退出\n");
    std::printf("[window] 1=开关 mip（关掉后斜面和远处会闪） 2=采样模式 nearest/bilinear/trilinear\n");

    sr::RenderTarget& target = surface->target();
    sr::Timer clock;
    app::InputState input;

    bool paused = false;
    float phase = 0.0f;
    g_gradientBackground = o.gradient;
    sr::OrbitCamera camera;
    camera.yaw = o.yaw;
    camera.pitch = o.pitch;
    camera.distance = o.distance;
    // 自动旋转在**没有拖拽**时进行；拖拽一次之后从新朝向接着转（不是回到原角度），
    // 否则用户刚摆好的视角会被下一帧覆盖掉，那种"抢镜头"的观感非常差。
    bool dragging = false;
    double lastFrameSeconds = 1.0 / 60.0;
    double frameBudgetMs = 1000.0 / static_cast<double>(kTargetFps);
    double lastMs = 0.0;
    double fpsPrintAccum = 0.0;
    int screenshotCounter = 0;
    int renderedFrames = 0;

    // --auto-close 是给冒烟测试用的：不开这个选项，"窗口能跑"就只能靠人盯着看，
    // 而 CI 化的验证需要的是"跑 N 帧、退出码 0、没有断言"。
    while (!surface->shouldClose()) {
        if (!surface->pumpInput(input) || surface->shouldClose()) break;   // 复审 I3：
        // WM_CLOSE 会同步 DestroyWindow，句柄当场失效；继续渲染并 present 就是在
        // 已释放的 DC 上做 BitBlt（未定义行为 + 一句误导性的"上屏失败" + 退出码 1）。

        if (input.togglePause) paused = !paused;
        if (input.toggleWireframe) g_showBackfaces = !g_showBackfaces;   // 借 W 键做 M2 的穿帮演示
        if (input.cycleView) {
            const int next = (static_cast<int>(g_sink.view) + 1) % 3;
            g_sink.view = static_cast<sr::DebugViewSink::View>(next);
            static const char* kNames[] = {"COLOR", "DEPTH", "GRAY"};
            // puts 自带换行，所以这里不需要转义序列（本会话里 "\\n" 被工具层吃成真实换行两次了）。
            const std::string line = std::string("[view] ") + kNames[next];
            std::puts(line.c_str());
        }
        if (input.toggleMip) {
            g_useMip = !g_useMip;
            // 关掉之后斜面和远处会开始闪烁 —— 那正是 spec §8 的 M4 验收，
            // 所以状态必须打在终端与 HUD 两处：只改画面不显示状态，没人知道现在该看什么。
            std::printf("[mip] %s\n", g_useMip ? "开（按屏幕导数选级）" : "关（永远 level 0，会闪）");
        }
        if (input.cycleFilter) {
            g_filter = nextFilter(g_filter);
            std::printf("[filter] %s\n", filterName(g_filter));
        }
        if (input.screenshot) {
            ++screenshotCounter;
            const std::string path = "screenshots/screenshot-" + std::to_string(screenshotCounter) + ".bmp";
            std::error_code ec;
            std::filesystem::create_directories("screenshots", ec);
            saveScreenshot(target, path, err);
        }

        const bool shouldRender = !paused || input.stepFrame;
        if (shouldRender) {
            // 鼠标：左键拖拽 = 轨道旋转，滚轮 = 推拉。暂停时不响应，
            // 这样"冻结当前帧再看细节"才成立（设计文档 C7）。
            dragging = input.leftPressed;
            if (dragging) {
                camera.orbit(input.dx, input.dy);
            } else {
                // 与帧时间无关的角速度：掉帧时转速不变（否则慢帧会让物体看起来忽快忽慢）。
                camera.yaw += kTurntableRadiansPerSecond * static_cast<float>(lastFrameSeconds * 1.0);
            }
            if (input.wheel != 0.0f) camera.dolly(input.wheel);

            sr::Timer frameTimer;
            const double fpsNow = lastMs > 0.0 ? 1000.0 / lastMs : 0.0;
            (void)camera;
            // 采样状态与相机姿态都要上 HUD：前者是"我现在该看到什么"（关 mip 该闪），
            // 后者是"这张截图怎么用命令行复现"（--yaw/--pitch/--distance 同名字段）。
            char buf[200];
            std::snprintf(buf, sizeof(buf),
                          "FPS %.1f  %.2f MS  Y %.2f P %.2f D %.1f  MIP %s  %s%s",
                          fpsNow, lastMs, static_cast<double>(camera.yaw),
                          static_cast<double>(camera.pitch), static_cast<double>(camera.distance),
                          g_useMip ? "ON" : "OFF", filterName(g_filter),
                          paused ? (dragging ? "  PAUSED DRAG" : "  PAUSED")
                                 : (dragging ? "  DRAG" : ""));
            renderFrame(target, phase, buf, camera);
            // 与帧时间相关：掉帧时动画速度不变（float 累加，所以乘的是 float）。
            phase += static_cast<float>(0.35 * lastMs * 0.001);
            lastMs = frameTimer.elapsedMs();
            if (!surface->present(err)) {
                std::printf("[错误] 上屏失败: %s\n", err.c_str());
                return 1;
            }
        } else {
            // 暂停时也要泵一下窗口，否则系统会认为它没响应。内容不动，画面就不会闪。
            surface->present(err);
        }

        // 一帧的总耗时在这里结算一次，节流与 FPS 打印都用它（分两次读会把时间吃掉一段）。
        const double elapsed = clock.lapMs();
        lastFrameSeconds = elapsed * 0.001;

        ++renderedFrames;
        if (o.autoClose > 0 && renderedFrames >= o.autoClose) break;

        fpsPrintAccum += elapsed;
        if (fpsPrintAccum >= 500.0) {
            const double fps = lastMs > 0.0 ? 1000.0 / lastMs : 0.0;
            std::printf("[frame] %.2f ms  %.1f fps  phase=%.3f%s\n", lastMs, fps, phase,
                        paused ? "  (暂停)" : "");
            fpsPrintAccum = 0.0;
        }

        // 帧率节流。原先是固定的 Sleep(1)：Windows 的 Sleep 粒度约 1~15ms，
        // 于是每一帧的耗时被随机拉长，动画看起来就是"不丝滑"——不是慢，是抖。
        // 现在按剩余预算睡到下一帧；睡不够就不睡（宁可掉一帧也不要累积延迟）。
        const double remaining = frameBudgetMs - elapsed;
        if (remaining > 1.0) {
            Sleep(static_cast<DWORD>(remaining));
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    app::AppOptions o;
    std::string err;
    // argv 在 Windows 下是本地编码（这里假设 UTF-8：--obj 路径含中文时由调用方决定）。
    if (!app::parseCmdline(argc, argv, o, err)) {
        std::printf("[参数错误] %s\n\n%s", err.c_str(), app::usageString().c_str());
        return 2;
    }
    if (o.help) {
        std::printf("%s", app::usageString().c_str());
        return 0;
    }
    if (!o.goldenWrite.empty()) {
        return writeGolden("m3_cube", o.goldenWrite, sr::renderGoldenScene);
    }
    if (!o.goldenWriteTextured.empty()) {
        return writeGolden("m4_textured_cube", o.goldenWriteTextured,
                           sr::renderGoldenSceneTextured);
    }
    if (o.offscreen) return runOffscreen(o);
    return runWindow(o);
}
