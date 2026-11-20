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
#include "render/color.h"
#include "render/overlay_font.h"
#include "render/patterns.h"
#include "render/raster_scalar.h"
#include "render/render_target.h"
#include "version.h"

#ifndef SR_VERSION
#define SR_VERSION "0.0.0-unspecified"
#endif

namespace {

const wchar_t* kWindowTitle = L"soft-raster — 纯 CPU 软件光栅化";

constexpr float kTop = 0.06f;   // 一个"有得看"的渐变，用来确认像素真的在流动
constexpr float kBottom = 0.85f;

// 渲染一整帧。M0 还没有任何 3D：能证明"像素从我们手里走到了屏幕"就够。
// 从这里到 M2 之间唯一的变化是"这一帧的内容由谁产生"。
void renderFrame(sr::RenderTarget& target, float phase, const std::string& statusLine) {
    target.depth.clear();
    const sr::LinearColor a{kTop, kTop * 0.55f, kBottom * 0.35f};
    const sr::LinearColor b{kBottom, kBottom * 0.7f, kTop * 0.4f};
    sr::fillLinearGradient(target.color, a, b, phase);

    // M1 的可见成果：一个屏幕空间三角形。顶点随 phase 绕中心旋转并呼吸，
    // 这样"改顶点会怎样"能直接看出来（设计文档 §8 M1 的验收）。
    // 注意它故意**没有**深度测试与插值：这是 Backend A 的最薄形态。
    {
        const float cx = static_cast<float>(target.color.width) * 0.5f;
        const float cy = static_cast<float>(target.color.height) * 0.62f;
        const float r = static_cast<float>(target.color.height) * 0.30f;
        const float breathe = 1.0f + 0.15f * std::sin(phase * 2.0f);
        sr::Vec2 v[3];
        for (int i = 0; i < 3; ++i) {
            const float ang = phase + static_cast<float>(i) * (2.0f * 3.14159265f / 3.0f);
            v[i] = sr::Vec2{cx + r * breathe * std::cos(ang), cy + r * breathe * std::sin(ang)};
        }
        const sr::FlatTriangle tri{{v[0], v[1], v[2]}, sr::encodeSRGB({0.05f, 0.55f, 0.35f})};
        sr::rasterizeFlatTriangle(target.color, tri);
    }

    // HUD 文字由我们自己画（设计文档 §10.3：屏幕上不该出现一个不是我们算出来的像素）。
    // 离屏路径也画它，这样"字体对不对"能靠一张 BMP 检查，而不必等人盯着窗口看。
    const uint32_t fg = sr::encodeSRGB({1.0f, 1.0f, 1.0f});
    const uint32_t bg = sr::encodeSRGB({0.015f, 0.015f, 0.02f});
    sr::drawText(target.color, 4, 4, statusLine, fg, bg, 2);
    sr::drawText(target.color, 4, 4 + 2 * sr::kGlyphHeightPx + 4, "P=PAUSE S=SHOT V=VIEW ESC=QUIT",
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

int runOffscreen(const app::AppOptions& o) {
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
        renderFrame(target, static_cast<float>(i) / static_cast<float>(o.frames), status);
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

    sr::RenderTarget& target = surface->target();
    sr::Timer clock;
    app::InputState input;

    bool paused = false;
    float phase = 0.0f;
    double lastMs = 0.0;
    double fpsPrintAccum = 0.0;
    int screenshotCounter = 0;
    int renderedFrames = 0;

    // --auto-close 是给冒烟测试用的：不开这个选项，"窗口能跑"就只能靠人盯着看，
    // 而 CI 化的验证需要的是"跑 N 帧、退出码 0、没有断言"。
    while (!surface->shouldClose()) {
        surface->pumpInput(input);

        if (input.togglePause) paused = !paused;
        if (input.screenshot) {
            ++screenshotCounter;
            const std::string path = "screenshots/screenshot-" + std::to_string(screenshotCounter) + ".bmp";
            std::error_code ec;
            std::filesystem::create_directories("screenshots", ec);
            saveScreenshot(target, path, err);
        }

        const bool shouldRender = !paused || input.stepFrame;
        if (shouldRender) {
            sr::Timer frameTimer;
            const double fpsNow = lastMs > 0.0 ? 1000.0 / lastMs : 0.0;
            char buf[128];
            std::snprintf(buf, sizeof(buf), "FPS %.1f  %.2f MS  %dx%d%s", fpsNow, lastMs, o.width,
                          o.height, paused ? "  PAUSED" : "");
            renderFrame(target, phase, buf);
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

        ++renderedFrames;
        if (o.autoClose > 0 && renderedFrames >= o.autoClose) break;

        fpsPrintAccum += clock.lapMs();
        if (fpsPrintAccum >= 500.0) {
            const double fps = lastMs > 0.0 ? 1000.0 / lastMs : 0.0;
            std::printf("[frame] %.2f ms  %.1f fps  phase=%.3f%s\n", lastMs, fps, phase,
                        paused ? "  (暂停)" : "");
            fpsPrintAccum = 0.0;
        }

        // 不给一点喘息的话，这个循环会把一个核占满并且消息队列永远非空，
        // 于是 Windows 会认为窗口无响应。M8 之后由帧率上限取代这一行。
        Sleep(1);
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
    if (o.offscreen) return runOffscreen(o);
    return runWindow(o);
}
