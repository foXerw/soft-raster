#include "app/scene.h"

#include <filesystem>
#include <fstream>
#include <string>

#include "harness.h"
#include "math/vec.h"
#include "scene/fit.h"

// M5.6：SceneState 是"画什么 + 从哪看 + 出了什么错"的持有者。它不含 Win32
// （cmdline.cpp 先例），所以能直接列进 tests 目标。这四条行为是 app 接线里唯一
// 可单测的一半；按键/拖放的消息那一半在 win32_window.cpp，由窗口冒烟与肉眼门兜底。

namespace {

namespace fs = std::filesystem;

// 临时目录 RAII（与 test_model.cpp 同形）：建在 sr_tmp_bmp/ 下，析构清掉。
struct TempDir {
    fs::path p;
    explicit TempDir(const char* name) : p(fs::path("sr_tmp_bmp") / name) {
        std::error_code ec;
        fs::create_directories(p, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(p, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

void writeText(const fs::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);   // ofstream(fs::path)：宽字符打开（Ruling 62）
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
}

// 一个三角形（无 mtllib ⇒ 默认材质、无贴图）：载入成败只由文件本身决定。
const char* kOneTriObj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
// 两个三角形：与 kOneTriObj 的 triangleCount 不同 —— "换上了新模型"的断言
// 不能靠新旧模型恰好同形而蒙混（trap (a)）。
const char* kTwoTriObj = "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\nf 1 2 3\nf 2 4 3\n";

// 1.6 立方体、中心 (2,-3,7) —— 与 test_fit 第一条用例同形：r = 0.8√3 ≈ 1.3856406，
// fov = kDefaultFovYRadians（π/3）、aspect 16:9 ⇒ distance = 2r ≈ 2.77128、
// near ≈ 0.001386、far ≈ 13.85641；aspect 9:16 ⇒ distance ≈ 4.48603。
// 中心故意不在原点：refit 若"target 不跟 bounds.center() 走"，默认值重合藏不住它。
// 两个四边形就含全部 8 个角点 ⇒ bounds 是完整盒子（绕向与 bounds 无关）。
const char* kOffsetBoxObj =
    "v 1.2 -3.8 6.2\nv 2.8 -3.8 6.2\nv 2.8 -2.2 6.2\nv 1.2 -2.2 6.2\n"
    "v 1.2 -3.8 7.8\nv 2.8 -3.8 7.8\nv 2.8 -2.2 7.8\nv 1.2 -2.2 7.8\n"
    "f 1 2 3 4\nf 5 6 7 8\n";

}  // namespace

SR_TEST(scene_load_replaces_the_model_on_success) {
    TempDir dir("m56_replace");
    writeText(dir.p / "a.obj", kOneTriObj);
    writeText(dir.p / "b.obj", kTwoTriObj);
    app::SceneState s;
    SR_ASSERT(s.model == nullptr);   // 初始 = 程序生成立方体（M2–M4 的老画面）
    SR_ASSERT(s.modelLabel == "CUBE");
    SR_ASSERT(s.load(dir.p / "a.obj"));
    SR_ASSERT(s.model != nullptr);
    if (s.model) SR_ASSERT_EQ(s.model->model.mesh.triangleCount(), 1);
    SR_ASSERT(s.errorLine.empty());
    SR_ASSERT(s.modelLabel == "a.obj");
    // 第二次成功载入必须**换上**新模型（triangleCount 1 → 2），不是留着旧的。
    SR_ASSERT(s.load(dir.p / "b.obj"));
    SR_ASSERT(s.model != nullptr);
    if (s.model) {
        SR_ASSERT_EQ(s.model->model.mesh.triangleCount(), 2);
        SR_ASSERT(s.model->source == dir.p / "b.obj");
    }
    SR_ASSERT(s.modelLabel == "b.obj");
    SR_ASSERT(s.errorLine.empty());
    SR_ASSERT(s.requested == dir.p / "b.obj");   // R 重载用的"同一个 path"
}

SR_TEST(scene_load_keeps_the_previous_model_and_sets_error_line_on_failure) {
    TempDir dir("m56_keep");
    writeText(dir.p / "a.obj", kOneTriObj);
    // 内容坏（索引越界 ⇒ parseObj D6 失败）：与"文件不存在"是两条不同的失败路径，
    // 都必须保留旧模型。文件存在但坏 ⇒ 失败只能来自解析，不是"打不开"。
    writeText(dir.p / "bad.obj", "v 0 0 0\nf 1 5 9\n");
    app::SceneState s;

    // 前半：初始状态（没有旧模型）失败 ⇒ model 保持 nullptr —— 兜底立方体不许被清空，
    // 错误要写进 errorLine 且**点名肇事文件**（归因，trap (a)）。
    SR_ASSERT_FALSE(s.load(dir.p / "missing.obj"));
    SR_ASSERT(s.model == nullptr);
    SR_ASSERT_FALSE(s.errorLine.empty());
    if (s.errorLine.find("missing.obj") == std::string::npos)
        std::printf("[scene] 缺文件的 errorLine 是：%s\n", s.errorLine.c_str());
    SR_ASSERT(s.errorLine.find("missing.obj") != std::string::npos);
    SR_ASSERT(s.modelLabel == "CUBE");
    SR_ASSERT(s.requested == dir.p / "missing.obj");   // 失败后 R 仍可重试同一个 path

    // 后半：有旧模型时失败 ⇒ 旧模型原样留在画面上（A1/半加载的反面）。
    SR_ASSERT(s.load(dir.p / "a.obj"));
    SR_ASSERT_FALSE(s.load(dir.p / "missing.obj"));
    SR_ASSERT(s.model != nullptr);
    if (s.model) {
        SR_ASSERT_EQ(s.model->model.mesh.triangleCount(), 1);
        SR_ASSERT(s.model->source == dir.p / "a.obj");
    }
    SR_ASSERT(s.modelLabel == "a.obj");   // label 跟着画面上的模型，不跟着失败的请求
    SR_ASSERT(s.errorLine.find("missing.obj") != std::string::npos);

    // 解析失败同样保留旧模型（先建新的再换指针，失败时 fresh 直接丢弃）。
    SR_ASSERT_FALSE(s.load(dir.p / "bad.obj"));
    SR_ASSERT(s.model != nullptr);
    if (s.model) SR_ASSERT_EQ(s.model->model.mesh.triangleCount(), 1);
    SR_ASSERT(s.errorLine.find("bad.obj") != std::string::npos);
}

SR_TEST(scene_successful_load_clears_the_error_line) {
    TempDir dir("m56_clear");
    writeText(dir.p / "a.obj", kOneTriObj);
    app::SceneState s;
    SR_ASSERT_FALSE(s.load(dir.p / "missing.obj"));
    SR_ASSERT_FALSE(s.errorLine.empty());
    SR_ASSERT(s.load(dir.p / "a.obj"));
    SR_ASSERT(s.errorLine.empty());   // 成功 ⇒ 错误行消失，HUD 第二行回到状态文字
}

SR_TEST(scene_refit_does_not_touch_yaw_or_pitch) {
    TempDir dir("m56_refit");
    writeText(dir.p / "box.obj", kOffsetBoxObj);
    app::SceneState s;
    SR_ASSERT(s.load(dir.p / "box.obj"));
    if (!s.model) return;   // 载入失败上面已红，这里不 deref 空指针（trap (b)）
    // base 全非默认且不对称：yaw/pitch 非零、distance 9 ≠ fit 值、target 非盒中心。
    s.camera.yaw = 0.6f;
    s.camera.pitch = -0.23f;
    s.camera.distance = 9.0f;
    s.camera.target = sr::Vec3{9.0f, 9.0f, 9.0f};
    s.nearPlane = 0.1f;
    s.farPlane = 100.0f;

    s.refit(1280, 720);   // 16:9
    SR_ASSERT_NEAR(s.camera.yaw, 0.6f, 1e-6f);
    SR_ASSERT_NEAR(s.camera.pitch, -0.23f, 1e-6f);
    // trap (a)：空操作 refit 也能过上面两行 —— 必须同时断言 fit **真的发生了**：
    // distance 9 → 2r、target (9,9,9) → 盒中心、near/far 默认 → r/1000 与 10r。
    SR_ASSERT_NEAR(s.camera.distance, 2.77128f, 1e-5f);
    SR_ASSERT_NEAR(s.camera.target.x, 2.0f, 1e-5f);
    SR_ASSERT_NEAR(s.camera.target.y, -3.0f, 1e-5f);
    SR_ASSERT_NEAR(s.camera.target.z, 7.0f, 1e-5f);
    SR_ASSERT_NEAR(s.nearPlane, 0.001386f, 1e-5f);
    SR_ASSERT_NEAR(s.farPlane, 13.85641f, 1e-5f);

    // 高瘦窗口：水平 fov 更紧 ⇒ 退得更远。这条同时钉住 refit 真把 width/height
    // 传成了 aspect（"恒用 16:9"的变异在这里红，宽窗口下它与正确实现一致）。
    s.refit(720, 1280);   // 9:16
    SR_ASSERT_NEAR(s.camera.distance, 4.48603f, 1e-5f);
    SR_ASSERT_NEAR(s.camera.yaw, 0.6f, 1e-6f);
    SR_ASSERT_NEAR(s.camera.pitch, -0.23f, 1e-6f);
}

SR_TEST(scene_refit_without_a_model_keeps_the_camera) {
    // model == nullptr（程序生成立方体）：没有包围盒可 fit，相机与 near/far 全归用户。
    // 各字段非默认：refit 若"顺手重置相机"或"退回默认 near/far"都会红。
    app::SceneState s;
    s.camera.yaw = 0.6f;
    s.camera.pitch = -0.23f;
    s.camera.distance = 9.0f;
    s.camera.target = sr::Vec3{1.0f, 2.0f, 3.0f};
    s.refit(1280, 720);
    SR_ASSERT(s.model == nullptr);
    SR_ASSERT_NEAR(s.camera.yaw, 0.6f, 1e-6f);
    SR_ASSERT_NEAR(s.camera.pitch, -0.23f, 1e-6f);
    SR_ASSERT_NEAR(s.camera.distance, 9.0f, 1e-6f);
    SR_ASSERT_NEAR(s.camera.target.x, 1.0f, 1e-6f);
    SR_ASSERT_NEAR(s.camera.target.y, 2.0f, 1e-6f);
    SR_ASSERT_NEAR(s.camera.target.z, 3.0f, 1e-6f);
    SR_ASSERT_NEAR(s.nearPlane, 0.1f, 1e-6f);
    SR_ASSERT_NEAR(s.farPlane, 100.0f, 1e-6f);
}

SR_TEST(scene_load_rejects_a_zero_face_obj_keeping_the_previous_model) {
    // Important 1（M5.6 评审）：没有任何面的 OBJ 在 parseObj/loadModel 眼里是**成功**
    // （空 mesh、零 group）—— 解析意义上合法（将来的点云/线框专用几何也会是它），
    // 但 drawModel 什么都不提交 ⇒ 空屏 + DC 0 + 终端"已载入 0 三角形"，绕过了本任务
    // "载入失败绝不空屏"的承诺。职责分界：loader 问"我解析对了吗"，app 问"这画面有没有
    // 东西可看" ⇒ 守卫在 SceneState::load，不在 loader（loader 的解析语义不许动）。
    TempDir dir("m56_zeroface");
    writeText(dir.p / "verts.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\n");   // 只有顶点，没有 f
    writeText(dir.p / "empty.obj", "");                              // 0 字节下载件
    writeText(dir.p / "a.obj", kOneTriObj);
    app::SceneState s;
    // 初始状态：拒绝 ⇒ 兜底立方体保留（model nullptr），错误点名文件。
    SR_ASSERT_FALSE(s.load(dir.p / "verts.obj"));
    SR_ASSERT(s.model == nullptr);
    if (s.errorLine.find("verts.obj") == std::string::npos)
        std::printf("[scene] 零面 errorLine 是：%s\n", s.errorLine.c_str());
    SR_ASSERT(s.errorLine.find("verts.obj") != std::string::npos);
    SR_ASSERT_FALSE(s.load(dir.p / "empty.obj"));
    SR_ASSERT(s.model == nullptr);
    SR_ASSERT(s.errorLine.find("empty.obj") != std::string::npos);
    // 有旧模型：拒绝 ⇒ 旧模型原样（先建新的再换指针，零面守卫在换指针之前）。
    SR_ASSERT(s.load(dir.p / "a.obj"));
    SR_ASSERT_FALSE(s.load(dir.p / "verts.obj"));
    SR_ASSERT(s.model != nullptr);
    if (s.model) SR_ASSERT_EQ(s.model->model.mesh.triangleCount(), 1);
    SR_ASSERT(s.modelLabel == "a.obj");
    SR_ASSERT(s.errorLine.find("verts.obj") != std::string::npos);
}

SR_TEST(scene_refit_sets_scale_relative_dolly_clamps_and_the_first_wheel_does_not_jump) {
    // Ruling 73 的关键不变式：fit 是 distance 的第二个写者，夹钳必须知道 fit 的尺度 ——
    // refit 之后正/负各 dolly 一次，distance 第一次输入必须按乘法规则走、**不跳**
    // （等价于"fit 的输出永远落在夹钳区间内"）。只断言夹钳数值不够：max 设窄一档数值红
    // 而不变式不红，max 设到 fit 输出以下不变式红 —— 两种断言各挡各的变异（Ruling 67：
    // refit 写 4 个旧字段 + 2 个新边界，六件事各有各的红）。
    TempDir dir("m56_clamps");
    writeText(dir.p / "box.obj", kOffsetBoxObj);
    app::SceneState s;
    SR_ASSERT(s.load(dir.p / "box.obj"));
    if (!s.model) return;   // 载入失败上面已红（trap (b)）
    const float r = s.model->model.mesh.bounds.radius();   // 0.8√3 ≈ 1.3856406
    const float tol = 1e-5f * r;
    s.camera.yaw = 0.6f;
    s.camera.pitch = -0.23f;
    s.refit(1280, 720);
    const float dFit = 2.0f * r;   // 16:9 ⇒ distance = 2r
    SR_ASSERT_NEAR(s.camera.distance, dFit, tol);
    // 两个新边界各自的数值断言（r*1e-3 = 0.001386 > 1e-6 ⇒ min 取 r/1000）。
    SR_ASSERT_NEAR(s.camera.minDistance, r * 1e-3f, tol);
    SR_ASSERT_NEAR(s.camera.maxDistance, r * 1e3f, tol);
    // 不变式：正滚轮一格 = *0.9，不跳。
    s.camera.dolly(1.0f);
    SR_ASSERT_NEAR(s.camera.distance, 0.9f * dFit, tol);
    // 负滚轮一格 = /0.9，不跳（max 若低于 fit 输出，这里被夹 ⇒ 红）。
    s.refit(1280, 720);
    s.camera.dolly(-1.0f);
    SR_ASSERT_NEAR(s.camera.distance, dFit / 0.9f, tol);
    // 高瘦窗口 fit 退得更远（**绝对距离** 4.48603 ≈ 3.24r，r≈1.3856；不是 4.486·r），
    // 仍在区间内，第一下滚轮同样不跳。
    s.refit(720, 1280);
    const float dTall = s.camera.distance;
    SR_ASSERT_NEAR(dTall, 4.48603f, 1e-5f);
    s.camera.dolly(-1.0f);
    SR_ASSERT_NEAR(s.camera.distance, dTall / 0.9f, tol);
    // 加夹钳没弄丢 D15：yaw/pitch 仍归用户。
    SR_ASSERT_NEAR(s.camera.yaw, 0.6f, 1e-6f);
    SR_ASSERT_NEAR(s.camera.pitch, -0.23f, 1e-6f);
}

SR_TEST(scene_refit_on_degenerate_bounds_touches_nothing) {
    // 与 fitCamera 守卫同口径（Ruling 49：退化 ⇒ 不动作）。这里额外钉夹钳：退化盒若仍按
    // 公式写 min/max，max = r*1e3 会低于 min = 1e-6，dolly 会把相机夹死 —— 所以退化时
    // refit 六个字段（distance/target/near/far/min/max）一个都不许碰。
    TempDir dir("m56_degen");
    // 1e-7 尺度三角形：三个**不同**顶点（不是重复引用退化面）⇒ parseObj 保留、indices
    // 非空（过得了零面守卫），但 r = 8.66e-8 < 1e-6 ⇒ fitCamera 的退化分支。
    writeText(dir.p / "tiny.obj", "v 0 0 0\nv 1e-7 0 0\nv 0 1e-7 0\nf 1 2 3\n");
    app::SceneState s;
    SR_ASSERT(s.load(dir.p / "tiny.obj"));
    if (!s.model) return;
    SR_ASSERT(s.model->model.mesh.bounds.radius() < 1e-6f);
    s.camera.yaw = 0.6f;
    s.camera.pitch = -0.23f;
    s.camera.distance = 9.0f;
    s.camera.target = sr::Vec3{1.0f, 2.0f, 3.0f};
    s.nearPlane = 0.2f;    // 非默认："顺手重置成 0.1/100"也红
    s.farPlane = 50.0f;
    s.refit(1280, 720);
    SR_ASSERT_NEAR(s.camera.yaw, 0.6f, 1e-6f);
    SR_ASSERT_NEAR(s.camera.pitch, -0.23f, 1e-6f);
    SR_ASSERT_NEAR(s.camera.distance, 9.0f, 1e-6f);
    SR_ASSERT_NEAR(s.camera.target.x, 1.0f, 1e-6f);
    SR_ASSERT_NEAR(s.camera.target.y, 2.0f, 1e-6f);
    SR_ASSERT_NEAR(s.camera.target.z, 3.0f, 1e-6f);
    SR_ASSERT_NEAR(s.nearPlane, 0.2f, 1e-6f);
    SR_ASSERT_NEAR(s.farPlane, 50.0f, 1e-6f);
    SR_ASSERT_NEAR(s.camera.minDistance, 0.05f, 1e-6f);
    SR_ASSERT_NEAR(s.camera.maxDistance, 1000.0f, 1e-6f);
}

SR_TEST(scene_load_rejects_a_non_obj_extension_with_an_error_line) {
    // A4：非 .obj 后缀不许静默忽略。夹具内容是**合法 OBJ** —— 只有后缀守卫能拒绝它：
    // 守卫缺失时 loadModel 会成功 ⇒ 本条红（而不是因为内容坏"碰巧"失败，trap (a)）。
    TempDir dir("m56_ext");
    writeText(dir.p / "model.txt", kOneTriObj);
    app::SceneState s;
    SR_ASSERT_FALSE(s.load(dir.p / "model.txt"));
    SR_ASSERT(s.model == nullptr);
    SR_ASSERT(s.errorLine.find("model.txt") != std::string::npos);   // 点名文件
    SR_ASSERT(s.errorLine.find(".obj") != std::string::npos);        // 说明只收什么
    // 大小写不敏感：.OBJ 同样收（与 Ruling 63 的 .tga 分派同形）。
    writeText(dir.p / "UP.OBJ", kOneTriObj);
    SR_ASSERT(s.load(dir.p / "UP.OBJ"));
    SR_ASSERT(s.model != nullptr);
    if (s.model) SR_ASSERT_EQ(s.model->model.mesh.triangleCount(), 1);
    SR_ASSERT(s.errorLine.empty());
}