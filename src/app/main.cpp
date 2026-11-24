#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
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
#include <shellapi.h>   // CommandLineToArgvW（WIN32_LEAN_AND_MEAN 不带它）

#include "app/cmdline.h"
#include "app/input.h"
#include "app/path.h"
#include "app/scene.h"
#include "app/surface.h"
#include "bench/timer.h"
#include "loader/bmp_writer.h"
#include "loader/lex.h"   // displayPath（Ruling 62：消息里的路径不用 .string()）
#include "loader/model.h"      // loadModel（--golden-write-obj 的装配入口）
#include "loader/obj_scene.h"  // renderObjGoldenScene（M5.7 锚 3 的场景函数）
#include "loader/tga_writer.h"
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
#include "scene/fit.h"   // kDefaultFovYRadians（P24：投影与 fit 同一个角）
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

// HUD 投影成 ASCII（M5.6 评审 Important 3）：位图字体是 8×8 **ASCII** 字形
// （overlay_font.h），>= 0x80 的字节没有字形、glyphFor 画成方块 —— 中文错误原因在
// 屏幕上读不到，而 D18 的中文路径场景恰恰是屏幕最该读到信息的地方（"该看到什么必须
// always 在画面上"，"细节进终端"不满足它）。所以这里把非 ASCII 字节转义成 \xHH：
// 非 ASCII 字节无损保真，终端继续拿完整 UTF-8。注意这**不是严格可逆**的：路径里
// 本来就含字面 \xHH 序列时，转义结果与真的非 ASCII 字节无法区分（不是单射）。
// **不加中文字体**（那是一整块与渲染无关的工作）。
std::string hudAscii(const std::string& utf8) {
    std::string out;
    out.reserve(utf8.size());
    for (const unsigned char c : utf8) {
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
            continue;
        }
        const char* hex = "0123456789ABCDEF";
        out += "\\x";
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 0x0F]);
    }
    return out;
}

// 渲染一整帧。M5.6 起画面内容由 SceneState 决定：model == nullptr ⇒ 程序生成的
// 立方体（M2–M4 的老画面，逐字节不变是这条分支的回归证据）；非空 ⇒ drawModel 逐组提交。
void renderFrame(sr::RenderTarget& target, float phase, const std::string& statusLine,
                 const app::SceneState& scene) {
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

    sr::FrameStats stats;
    static sr::BackendA backend;
    sr::VertexUniforms vu;
    // 物体不自转：旋转交给轨道相机，鼠标拖拽直接改变 yaw/pitch。
    // "转相机"与"转物体"在画面上等价，但只有前者能让用户觉得自己抓住了物体。
    vu.model = sr::Mat4::identity();
    vu.view = scene.camera.view();
    // near/far 来自 SceneState（fit 写入），不再硬编码 0.1/100；fov 与 SceneState::refit
    // 取**同一个常量**（P24：fit 用的角与投影用的角漂移，症状是"某些模型装不下"）。
    vu.proj = sr::makePerspective(sr::kDefaultFovYRadians,
                                  static_cast<float>(target.color.width) /
                                      static_cast<float>(target.color.height),
                                  scene.nearPlane, scene.farPlane);
    // M6.0：法线矩阵一律走 makeNormalMatrix（M6.9 的 grep 守卫禁止生产侧写单位阵字面量）。
    // 这里的 model 恒为单位阵（旋转在轨道相机），构造上不会奇异；万一失败，
    // last-good 就是单位阵本身 —— 记一条诊断，画面继续，不退出也不黑屏（M5.6 同原则）。
    // P91（M6.3）：if/兜底/诊断块收进 setNormalMatrix（pipeline.h），四个生产点共用一份。
    sr::setNormalMatrix(vu, vu.model, "");

    if (scene.model == nullptr) {
        // 老路径（M2–M4）：立方体 + 程序棋盘。这一分支的像素必须逐字节不变 ——
        // 无 --obj 的离屏图与 M4 golden 是它的回归证据。立方体本体在 scene.cpp
        // （fallbackCube 单例）：renderFrame 只负责画，不持有场景内容。
        sr::PipelineState st;
        st.target = &target;
        st.vertexShader = sr::transformVS;
        // M4 的可见成果：贴一张程序生成的 checker。
        // 顶点色路径（vertexColorFS）在这里退出画面，但 Mesh 里的 colors 仍然保留 ——
        // M6 的光照 shader 还要它（M6.3 的 lambertVS 已消费：有效 albedo = 顶点色 × Kd，
        // D17，承诺兑现），而且 W 键看背面时"哪一面是哪个颜色"仍然读得出来。
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
        // ClampEdge 是"每面一整张贴图"的特例语义（Ruling 37），**只属于这个立方体**；
        // 导入模型走下面的 Repeat。理由见 golden_scene.cpp 的 goldenSampler()。
        st.fu.sampler.wrapU = sr::WrapMode::ClampEdge;
        st.fu.sampler.wrapV = sr::WrapMode::ClampEdge;
        st.fu.sampler.filter = g_filter;
        st.fu.sampler.useMip = g_useMip;
        st.vu = vu;
        st.stats = &stats;
        backend.draw(app::fallbackCube(), st);
    } else {
        // 导入模型（M5.4 drawModel）：逐组一次 drawRange，Kd/贴图按组覆写。
        sr::FragmentUniforms base;
        base.baseColor = sr::LinearColor{1.0f, 1.0f, 1.0f};   // drawModel 按组覆写成线性 Kd
        // 导入模型用 **Repeat**：外部模型的 uv 常越界（平铺语义）。ClampEdge 是
        // "每面一整张贴图"的特例语义（Ruling 37），只属于上面的 golden 立方体 ——
        // 两条路径的 wrap 不同是故意的，不是漏改。
        base.sampler.wrapU = sr::WrapMode::Repeat;
        base.sampler.wrapV = sr::WrapMode::Repeat;
        // `1`/`2` 两个旋钮对导入模型同样生效：传活的应用状态，别在导入路径上
        // 重新发明第二条采样状态路径（M4 旋钮的自然延伸）。
        base.sampler.filter = g_filter;
        base.sampler.useMip = g_useMip;
        sr::drawModel(backend, *scene.model, target, vu, base, &stats);
    }

    // HUD 文字由我们自己画（设计文档 §10.3：屏幕上不该出现一个不是我们算出来的像素）。
    // 离屏路径也画它，这样"字体对不对"能靠一张 BMP 检查，而不必等人盯着窗口看。
    const uint32_t fg = sr::encodeSRGB({1.0f, 1.0f, 1.0f});
    const uint32_t bg = sr::encodeSRGB({0.015f, 0.015f, 0.02f});
    // 调试视图在文字之前应用：HUD 必须永远可读，否则切到深度视图时文字被一起冲掉。
    g_sink.present(target);

    sr::drawText(target.color, 4, 4, statusLine, fg, bg, 2);
    // 第二行：errorLine 非空 ⇒ **整行给错误**，持续到下次成功载入（"该看到什么必须
    // always 在画面上"）。否则 = 模型标签 + DC/UNRECOGNIZED 计数 + 按键提示。
    // drawText 越界静默丢弃（render_target.h），长错误行在右缘截断，不会写坏内存。
    // 错误行与标签都经 hudAscii：屏幕上是 \xHH 转义串而不是中文原文（字体只有 ASCII
    // 字形，理由见 hudAscii 注释）；终端拿完整 UTF-8。
    const int line2Y = 4 + 2 * sr::kGlyphHeightPx + 4;
    if (!scene.errorLine.empty()) {
        sr::drawText(target.color, 4, line2Y, hudAscii(scene.errorLine),
                     sr::encodeSRGB({1.0f, 0.4f, 0.35f}), bg, 1);
    } else {
        char line2[256];
        const int unrecognized = scene.model ? scene.model->stats.unrecognized : 0;
        std::snprintf(line2, sizeof(line2),
                      "%s  DC %lld  UNRECOGNIZED %d  P=PAUSE S=SHOT V=VIEW 1=MIP 2=FILT R=RELOAD F=FIT ESC=QUIT",
                      hudAscii(scene.modelLabel).c_str(), static_cast<long long>(stats.drawCalls),
                      unrecognized);
        sr::drawText(target.color, 4, line2Y, line2,
                     sr::encodeSRGB({0.9f, 0.85f, 0.6f}), bg, 1);
    }
}

int saveScreenshot(const sr::RenderTarget& target, const std::filesystem::path& path,
                   std::string& err) {
    // writeBmp 需要一个非常量的 ColorTarget（视图对象本身是值语义，不修改它），
    // 所以这里拷贝一份视图而不是 const_cast。
    sr::ColorTarget copy = target.color;
    if (!sr::writeBmp(path, copy, err)) {
        std::printf("[screenshot] 失败: %s\n", err.c_str());
        return 1;
    }
    std::printf("[screenshot] 已写出 %s (%dx%d)\n", sr::lex::displayPath(path).c_str(),
                copy.width, copy.height);
    return 0;
}

// 三个 golden 入口共用"建离屏面 → 渲一帧 → 存图"这一段，唯一区别是**调用哪个场景函数**。
// 场景以可调用对象传入而不是复制两份：图必须由生产代码里的那一个函数生成，
// 而"哪个函数生成了它"要能从打印出来的标签里读出来（标签与函数搞错的代价是一整帧白比对）。
// M5.7：从裸函数指针放宽到 std::function，因为 OBJ 场景要携带一个已载入的模型
// （renderObjGoldenScene(m, ...)）—— 但**没有引入带默认值的场景选择**：每个入口
// 仍是自己的 flag（Ruling 38），缺 --obj 在 parseCmdline 就报错。
int writeGolden(const std::string& label, const std::filesystem::path& path,
                const std::function<void(sr::RenderTarget&, sr::FrameStats*)>& scene) {
    auto surface = app::createOffscreenSurface(sr::kGoldenWidth, sr::kGoldenHeight);
    sr::FrameStats stats;
    scene(surface->target(), &stats);
    std::string err;
    if (saveScreenshot(surface->target(), path, err) != 0) return 1;
    const std::string line =
        "[golden] " + label + " → " + sr::lex::displayPath(path) + " 由 " + sr::buildInfoString();
    std::puts(line.c_str());
    return 0;
}

// --obj / R / 拖放共用的载入入口（M5.6）。失败 ⇒ 打印 + HUD 错误行，旧画面（首载时
// 是兜底立方体）原样保留，**程序继续跑** —— 不是退出：看不见模型比看见错误更糟。
// 成功 ⇒ 打印计数，按 fitEnabled 自动 refit（--no-fit 时不自动；F 键仍可手动）。
void loadObjIntoScene(app::SceneState& scene, const std::filesystem::path& objPath, int width,
                      int height) {
    if (scene.load(objPath)) {
        const sr::LoadedModel& m = *scene.model;
        std::printf("[obj] 已载入 %s: %d 三角形 / %zu 组 / %zu 贴图\n",
                    sr::lex::displayPath(objPath).c_str(), m.stats.triangles,
                    m.model.groups.size(), m.textures.size());
        if (scene.fitEnabled) scene.refit(width, height);
    } else {
        std::printf("[obj] 载入失败: %s\n", scene.errorLine.c_str());
    }
}

int runOffscreen(const app::AppOptions& o) {
    g_gradientBackground = o.gradient;   // 离屏默认也走纯色，golden 场景自己决定背景
    auto surface = app::createOffscreenSurface(o.width, o.height);
    sr::RenderTarget& target = surface->target();

    app::SceneState scene;
    scene.camera.yaw = o.yaw;
    scene.camera.pitch = o.pitch;
    scene.camera.distance = o.distance;
    scene.fitEnabled = !o.noFit;   // --no-fit：载入后不自动调整相机（F 仍可手动）
    if (!o.obj.empty()) {
        // nativePath：全仓库唯一的 UTF-8→path 转换点（D18）。argv 已在 main 开头统一成 UTF-8。
        loadObjIntoScene(scene, app::nativePath(o.obj), o.width, o.height);
    }

    sr::Timer total;
    std::vector<double> frameMs;
    frameMs.reserve(static_cast<size_t>(o.frames));

    for (int i = 0; i < o.frames; ++i) {
        sr::Timer frameTimer;
        const std::string status = "OFFSCREEN FRAME " + std::to_string(i + 1) + "/" +
                                   std::to_string(o.frames) + "  " + std::to_string(o.width) + "X" +
                                   std::to_string(o.height);
        renderFrame(target, static_cast<float>(i) / static_cast<float>(o.frames), status, scene);
        frameMs.push_back(frameTimer.elapsedMs());
    }
    const double ms = total.elapsedMs();

    const std::filesystem::path outPath = app::nativePath(o.out.empty() ? "out.bmp" : o.out);
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
    std::printf("[window] %s — %dx%d\n", sr::buildInfoString(), o.width, o.height);
    std::printf("[window] P=暂停 O=单步 S=截图 V=视图 Esc=退出\n");
    std::printf("[window] 1=开关 mip（关掉后斜面和远处会闪） 2=采样模式 nearest/bilinear/trilinear\n");
    std::printf("[window] R=重载模型 F=重新 fit（不动视角） 拖放 .obj 进窗口即可载入\n");

    sr::RenderTarget& target = surface->target();
    sr::Timer clock;
    app::InputState input;

    bool paused = false;
    float phase = 0.0f;
    g_gradientBackground = o.gradient;
    app::SceneState scene;
    scene.camera.yaw = o.yaw;
    scene.camera.pitch = o.pitch;
    scene.camera.distance = o.distance;
    scene.fitEnabled = !o.noFit;   // --no-fit：载入后不自动调整相机（F 仍可手动）
    if (!o.obj.empty()) {
        // 首载失败不退出：兜底立方体留在画面上，错误进终端 + HUD 第二行（M5.6）。
        // nativePath：全仓库唯一的 UTF-8→path 转换点（D18）。
        loadObjIntoScene(scene, app::nativePath(o.obj), o.width, o.height);
    }
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
        // F = 显式重 fit：只动 distance/target/near/far，yaw/pitch 归用户（D15）。
        // 不受 --no-fit 门控：fitEnabled 只管"载入后的自动 fit"，按 F 是手动请求。
        if (input.refit) scene.refit(target.color.width, target.color.height);
        // R = 重载同一个 path（失败保留旧模型，错误行持续到下次成功）。
        // 从未请求过模型（无 --obj 无拖放）时无事可做。
        if (input.reloadModel && !scene.requested.empty()) {
            loadObjIntoScene(scene, scene.requested, target.color.width, target.color.height);
        }
        // 拖放（M5.6）：shell 给的路径已在 win32_window.cpp 转成 UTF-8，这里经
        // nativePath 走与 --obj 完全相同的入口（后缀守卫在 SceneState::load，A4）。
        // 凭 dropEvent 而非 droppedObj 非空（终审 M3）：转换失败时 droppedObj 为空，
        // 但事件发生过 —— 仍走 load，空路径拿到自己的错误消息，不静默忽略（A4）。
        if (input.dropEvent) {
            loadObjIntoScene(scene, app::nativePath(input.droppedObj), target.color.width,
                             target.color.height);
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
                scene.camera.orbit(input.dx, input.dy);
            } else {
                // 与帧时间无关的角速度：掉帧时转速不变（否则慢帧会让物体看起来忽快忽慢）。
                scene.camera.yaw += kTurntableRadiansPerSecond * static_cast<float>(lastFrameSeconds * 1.0);
            }
            if (input.wheel != 0.0f) scene.camera.dolly(input.wheel);

            sr::Timer frameTimer;
            const double fpsNow = lastMs > 0.0 ? 1000.0 / lastMs : 0.0;
            // 采样状态与相机姿态都要上 HUD：前者是"我现在该看到什么"（关 mip 该闪），
            // 后者是"这张截图怎么用命令行复现"（--yaw/--pitch/--distance 同名字段）。
            char buf[200];
            std::snprintf(buf, sizeof(buf),
                          "FPS %.1f  %.2f MS  Y %.2f P %.2f D %.1f  MIP %s  %s%s",
                          fpsNow, lastMs, static_cast<double>(scene.camera.yaw),
                          static_cast<double>(scene.camera.pitch),
                          static_cast<double>(scene.camera.distance),
                          g_useMip ? "ON" : "OFF", filterName(g_filter),
                          paused ? (dragging ? "  PAUSED DRAG" : "  PAUSED")
                                 : (dragging ? "  DRAG" : ""));
            renderFrame(target, phase, buf, scene);
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
    // UTF-8 argv 桥（D18）：产品方的 shell 是 Git Bash/MSYS2，传的是 UTF-8；而 MSVC CRT
    // 的窄 argv 按本地 ANSI 码页（本机 936）从 UTF-16 命令行转出来，中文路径会 mojibake。
    // 所以这里用 GetCommandLineW + CommandLineToArgvW 取 UTF-16 argv，统一转成 UTF-8
    // 再喂**现有的** parseCmdline（cmdline.cpp 与它的测试一行不改，它们喂的一直是 ASCII）。
    // AppOptions 的路径字段从此是 UTF-8 语义；nativePath 是全仓库唯一的 UTF-8→path 转换点。
    std::vector<std::string> args;
    std::vector<char*> argp;
    int argcW = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argcW);
    if (argvW != nullptr) {
        args.reserve(static_cast<size_t>(argcW));
        for (int i = 0; i < argcW; ++i) args.push_back(app::utf8FromWide(argvW[i]));
        LocalFree(argvW);
        for (std::string& s : args) argp.push_back(s.data());
        argc = static_cast<int>(argp.size());
        argv = argp.data();
    }
    // CommandLineToArgvW 失败（极端 OOM）时退回 CRT argv：ASCII 路径仍可用，比不启动好。

    app::AppOptions o;
    std::string err;
    if (!app::parseCmdline(argc, argv, o, err)) {
        std::printf("[参数错误] %s\n\n%s", err.c_str(), app::usageString().c_str());
        return 2;
    }
    if (o.help) {
        std::printf("%s", app::usageString().c_str());
        return 0;
    }
    // 资产生成入口（M5 锚 2 的 provenance）：不渲染、不建 surface，写完即返回。
    // 与 golden 入口互斥已在 parseCmdline 里拒绝，这里不会静默忽略谁。
    if (!o.emitCheckerTga.empty()) {
        const std::filesystem::path tgaPath = app::nativePath(o.emitCheckerTga);
        const sr::ImageRGBA8 checker =
            sr::makeCheckerImage(sr::kGoldenCheckerCells, sr::kGoldenCheckerCellPx, {});
        std::string werr;
        if (!sr::writeTga(tgaPath, checker, werr)) {
            std::printf("[checker-tga] 失败: %s\n", werr.c_str());
            return 1;
        }
        std::printf("[checker-tga] %dx%d → %s 由 makeCheckerImage(%d,%d) + writeTga，%s\n",
                    checker.width, checker.height, sr::lex::displayPath(tgaPath).c_str(),
                    sr::kGoldenCheckerCells, sr::kGoldenCheckerCellPx, sr::buildInfoString());
        return 0;
    }
    if (!o.goldenWrite.empty()) {
        return writeGolden("m3_cube", app::nativePath(o.goldenWrite), sr::renderGoldenScene);
    }
    if (!o.goldenWriteTextured.empty()) {
        return writeGolden("m4_textured_cube", app::nativePath(o.goldenWriteTextured),
                           sr::renderGoldenSceneTextured);
    }
    if (!o.goldenWriteObj.empty()) {
        // parseCmdline 已保证 --obj 非空（缺它 ⇒ 参数错误），这里没有默认场景。
        // 输出只是给人看的图（notes 用）：锚 3 的比对在测试里进程内对着既有 M4
        // 参考图做，**不新增参考图**（收紧 2）。载入失败 ⇒ 响亮退出，不画兜底立方体。
        sr::LoadedModel model;
        std::string loadErr;
        if (!sr::loadModel(app::nativePath(o.obj), model, loadErr)) {
            std::printf("[golden-obj] 载入失败: %s\n", loadErr.c_str());
            return 1;
        }
        return writeGolden("m5_obj_cube", app::nativePath(o.goldenWriteObj),
                           [&model](sr::RenderTarget& t, sr::FrameStats* s) {
                               sr::renderObjGoldenScene(model, t, s);
                           });
    }
    if (o.offscreen) return runOffscreen(o);
    return runWindow(o);
}
