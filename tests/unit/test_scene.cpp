#include "app/scene.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "harness.h"
#include "math/transform.h"
#include "math/vec.h"
#include "render/builtin_shaders.h"
#include "render/color.h"
#include "render/render_backend.h"
#include "scene/fit.h"
#include "scene/golden_scene.h"   // goldenCheckerTexture（unlit 回归锚用的同一张单例贴图）
#include "test_util.h"            // Canvas

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
    // P26：坏后缀的拖放不许覆盖 R 的重试路径 —— requested 仍指最后一个好模型。
    // 变异：把 requested = obj 挪回 load 入口（后缀守卫之前）⇒ 这里变成 model.txt ⇒ 红。
    SR_ASSERT_FALSE(s.load(dir.p / "model.txt"));
    SR_ASSERT(s.requested == dir.p / "UP.OBJ");
}

SR_TEST(scene_load_rejects_an_empty_path_with_its_own_message) {
    // P27：nativePath/utf8FromWide 转换失败返回 {}；若报"只支持 .obj"会把**编码**失败
    // 误归因成**后缀**错误。空路径有自己的消息，且不进 R 的重试路径、不丢旧模型。
    // 变异：删空路径分支 ⇒ load({}) 落到后缀守卫 ⇒ errorLine 含 ".obj"、不含"空" ⇒ 红。
    TempDir dir("m5fix_empty");
    writeText(dir.p / "a.obj", kOneTriObj);
    app::SceneState s;
    SR_ASSERT(s.load(dir.p / "a.obj"));
    SR_ASSERT_FALSE(s.load(fs::path{}));
    SR_ASSERT_FALSE(s.errorLine.empty());
    SR_ASSERT(s.errorLine.find("空") != std::string::npos);   // 自己的消息
    SR_ASSERT(s.errorLine.find(".obj") == std::string::npos); // 不是后缀错误
    SR_ASSERT(s.model != nullptr);   // 旧模型保留
    if (s.model) SR_ASSERT_EQ(s.model->model.mesh.triangleCount(), 1);
    SR_ASSERT(s.requested == dir.p / "a.obj");   // 重试路径不被空请求覆盖
}

// ---- M6.6：光照接线 ----------------------------------------------------------

namespace {

// 渲一帧内容进已就地构造的 Canvas（与 test_lit_shaders 的 drawQuad 同形，剔除开）。
void renderContent(Canvas& c, const sr::Mesh& mesh, sr::VertexShaderFn vs, sr::FragmentShaderFn fs,
                   const sr::VertexUniforms& vu, const sr::FragmentUniforms& fu, sr::FrameStats& s) {
    c.rt.color.clear(0xFF000000u);
    c.rt.depth.clear();
    sr::PipelineState st;
    st.target = &c.rt;
    st.vertexShader = vs;
    st.fragmentShader = fs;
    st.vu = vu;
    st.fu = fu;
    st.cull.backfaceCull = true;
    st.stats = &s;
    sr::BackendA backend;
    backend.draw(mesh, st);
}

// 默认相机 + 单位 model 的 uniforms（球夹具用）。
sr::VertexUniforms sphereVu(const sr::OrbitCamera& cam, float aspect) {
    sr::VertexUniforms vu;
    vu.model = sr::Mat4::identity();
    vu.view = cam.view();
    vu.proj = sr::makePerspective(sr::kDefaultFovYRadians, aspect, 0.1f, 100.0f);
    sr::setNormalMatrix(vu, vu.model, "");
    return vu;
}

sr::FragmentUniforms litFu() {
    sr::FragmentUniforms fu;
    fu.baseColor = sr::LinearColor{1.0f, 1.0f, 1.0f};
    fu.sampler.wrapU = sr::WrapMode::ClampEdge;
    fu.sampler.wrapV = sr::WrapMode::ClampEdge;
    fu.sampler.filter = sr::FilterMode::Trilinear;
    fu.sampler.useMip = true;
    return fu;
}

}  // namespace

SR_TEST(buildLightRig_decodes_colors_and_defaults_radius_to_three_times_the_scene) {
    app::AppOptions o;
    o.keyColor[0] = o.keyColor[1] = o.keyColor[2] = 0.5f;
    o.pointColor[0] = 0.2f; o.pointColor[1] = 0.4f; o.pointColor[2] = 0.6f;
    o.ambientColor[0] = o.ambientColor[1] = o.ambientColor[2] = 0.5f;
    o.ambientIntensity = 0.5f;
    sr::BBox bounds;
    bounds.grow(sr::Vec3{-2.0f, -2.0f, -2.0f});
    bounds.grow(sr::Vec3{2.0f, 2.0f, 2.0f});   // radius = 2√3 ≈ 3.4641
    sr::OrbitCamera cam;
    const sr::LightRig rig = app::buildLightRig(o, bounds, cam);

    // 解码一次（D5）：0.5 ⇒ ≈0.2140，不是 0.5（没解）也不是 0.25（gamma²）。
    SR_ASSERT_NEAR(rig.keyColor.r, sr::srgbToLinear(0.5f), 1e-4f);
    SR_ASSERT_NEAR(rig.keyColor.g, sr::srgbToLinear(0.5f), 1e-4f);
    SR_ASSERT_NEAR(rig.pointColor.r, sr::srgbToLinear(0.2f), 1e-4f);
    SR_ASSERT_NEAR(rig.pointColor.g, sr::srgbToLinear(0.4f), 1e-4f);
    SR_ASSERT_NEAR(rig.pointColor.b, sr::srgbToLinear(0.6f), 1e-4f);
    // 环境色 = 解码 × 强度（--ambient 是线性乘子）。
    SR_ASSERT_NEAR(rig.ambient.r, sr::srgbToLinear(0.5f) * 0.5f, 1e-4f);
    // keyDir 归一化（原始 {-0.3,-0.8,-0.6}）。
    SR_ASSERT_NEAR(sr::length(rig.keyDirWorld), 1.0f, 1e-5f);
    // 半径默认 = 场景半径 ×3。
    SR_ASSERT_NEAR(rig.attenuateRadius, bounds.radius() * 3.0f, 1e-4f);
    // eye/point 默认 = cam.eye()（D5）。
    const sr::Vec3 eye = cam.eye();
    expectVec3Near(rig.eyeWorld, eye.x, eye.y, eye.z, 1e-5f);
    expectVec3Near(rig.pointPosWorld, eye.x, eye.y, eye.z, 1e-5f);

    // 空盒 ⇒ 半径 1.0（Ruling 49 退化哲学）。
    const sr::LightRig rigEmpty = app::buildLightRig(o, sr::BBox{}, cam);
    SR_ASSERT_NEAR(rigEmpty.attenuateRadius, 1.0f, 1e-6f);

    // --point-pos 覆盖点光位置，但 eyeWorld 仍跟随相机。
    o.pointPosGiven = true;
    o.pointPos[0] = 9.0f; o.pointPos[1] = 8.0f; o.pointPos[2] = 7.0f;
    const sr::LightRig rigOverride = app::buildLightRig(o, bounds, cam);
    expectVec3Near(rigOverride.pointPosWorld, 9.0f, 8.0f, 7.0f, 1e-5f);
    expectVec3Near(rigOverride.eyeWorld, eye.x, eye.y, eye.z, 1e-5f);

    // 衰减开关原样透传（A 键/命令行 → rig.attenuate）。默认关、给了开。
    SR_ASSERT_FALSE(rig.attenuate);   // 上面 o.attenuate 未设 ⇒ 默认 false
    o.attenuate = true;
    const sr::LightRig rigAtt = app::buildLightRig(o, bounds, cam);
    SR_ASSERT(rigAtt.attenuate);
    // 强度原样透传（线性乘子，不解码）。
    SR_ASSERT_NEAR(rigAtt.keyIntensity, o.keyIntensity, 1e-6f);
    SR_ASSERT_NEAR(rigAtt.pointIntensity, o.pointIntensity, 1e-6f);
}

SR_TEST(stepIntensity_reaches_zero_by_repeated_presses) {
    // D24 的牙齿：加法步进能从 1.0 按到**精确 0**（夹钳）。乘法步进 ×0.9^11 ≈ 0.31，
    // 永远到不了 0 ⇒ 这条在乘法实现下必红。
    float v = 1.0f;
    for (int i = 0; i < 11; ++i) app::stepIntensity(v, -0.1f);
    SR_ASSERT_NEAR(v, 0.0f, 1e-6f);
    // 上夹钳 [0,3]：2.95 + 0.1 = 3.05 ⇒ 夹到 3.0，再按仍 3.0。
    float u = 2.95f;
    app::stepIntensity(u, 0.1f);
    SR_ASSERT_NEAR(u, 3.0f, 1e-6f);
    app::stepIntensity(u, 0.1f);
    SR_ASSERT_NEAR(u, 3.0f, 1e-6f);
    // 下夹钳：0.05 - 0.1 = -0.05 ⇒ 夹到 0.0（不是负数）。
    float w = 0.05f;
    app::stepIntensity(w, -0.1f);
    SR_ASSERT_NEAR(w, 0.0f, 1e-6f);
}

SR_TEST(reset_key_restores_defaults) {
    app::SceneState s;
    s.rig.keyIntensity = 2.5f;
    s.rig.pointIntensity = 0.0f;
    s.rig.attenuate = true;
    s.resetLights();
    const app::AppOptions d{};   // 内置默认是唯一真源
    SR_ASSERT_NEAR(s.rig.keyIntensity, d.keyIntensity, 1e-6f);
    SR_ASSERT_NEAR(s.rig.pointIntensity, d.pointIntensity, 1e-6f);
    SR_ASSERT_FALSE(s.rig.attenuate);
}

SR_TEST(scene_hangs_one_rig_on_both_stages) {
    // Ruling 97：两个阶段的 lights/mat 指针都指向 scene 持有的那一份，不是拷贝。
    app::SceneState s;
    s.rig.keyIntensity = 1.23f;   // 标记值：证明挂的就是这个对象
    sr::VertexUniforms vu;
    sr::FragmentUniforms fu;
    app::prepareFrameUniforms(s, 1280, 720, vu, fu);
    SR_ASSERT(vu.lights == &s.rig);
    SR_ASSERT(fu.lights == &s.rig);
    SR_ASSERT(vu.lights == fu.lights);
    SR_ASSERT(vu.mat == &s.proceduralMat);
    SR_ASSERT(fu.mat == &s.proceduralMat);
    SR_ASSERT(vu.mat == fu.mat);
    SR_ASSERT_NEAR(vu.lights->keyIntensity, 1.23f, 1e-6f);
    // 矩阵真的填了（view 非单位阵：相机有 pitch）。
    SR_ASSERT_FALSE(s.normalMatrixDegenerate);
    // 视点相关项刷新：eyeWorld = cam.eye()；非 --point-pos ⇒ pointPos 跟随。
    const sr::Vec3 eye = s.camera.eye();
    expectVec3Near(s.rig.eyeWorld, eye.x, eye.y, eye.z, 1e-5f);
    expectVec3Near(s.rig.pointPosWorld, eye.x, eye.y, eye.z, 1e-5f);
    // 相机转动后点光跟随（高光随拖拽移动的前提）。
    s.camera.yaw += 1.0f;
    app::prepareFrameUniforms(s, 1280, 720, vu, fu);
    const sr::Vec3 eye2 = s.camera.eye();
    expectVec3Near(s.rig.pointPosWorld, eye2.x, eye2.y, eye2.z, 1e-5f);
    // --point-pos 固定时不跟随。
    s.pointPosFromCmdline = true;
    s.rig.pointPosWorld = sr::Vec3{9.0f, 8.0f, 7.0f};
    s.camera.yaw += 1.0f;
    app::prepareFrameUniforms(s, 1280, 720, vu, fu);
    expectVec3Near(s.rig.pointPosWorld, 9.0f, 8.0f, 7.0f, 1e-5f);
    expectVec3Near(s.rig.eyeWorld, s.camera.eye().x, s.camera.eye().y, s.camera.eye().z, 1e-5f);
}

SR_TEST(default_rig_on_the_lit_sphere_does_not_saturate) {
    // D19 / §8.2 #7a 的前半：默认 rig 渲球，全图最大通道 < 255 **且**高光像素数 > 0。
    // 高光像素 = 比"关掉高光（Ks=0）"的同像素亮 > 0.01 线性 ⇒ 高光项真的可见，不靠魔法阈值。
    app::AppOptions o;   // 全默认
    sr::OrbitCamera cam;
    const sr::Mesh sphere = sr::makeUVSphere(1.0f, 24, 32);
    const sr::LightRig rig = app::buildLightRig(o, sphere.bounds, cam);
    sr::MaterialShade mat = sr::litDefaultMaterialShade();

    sr::VertexUniforms vu = sphereVu(cam, 1.0f);
    vu.lights = &rig;
    vu.mat = &mat;
    sr::FragmentUniforms fu = litFu();
    fu.lights = &rig;
    fu.mat = &mat;

    Canvas full{128, 128};
    sr::FrameStats sf;
    renderContent(full, sphere, sr::transformVS, sr::blinnPhongPerPixelFS, vu, fu, sf);
    SR_ASSERT(sf.fragmentsWritten > 500);

    int maxByte = 0;
    for (int y = 0; y < 128; ++y) {
        for (int x = 0; x < 128; ++x) {
            const uint32_t p = full.rt.color.readPacked(x, y);
            const int r = static_cast<int>((p >> 16) & 0xFFu);
            const int g = static_cast<int>((p >> 8) & 0xFFu);
            const int b = static_cast<int>(p & 0xFFu);
            if (r > maxByte) maxByte = r;
            if (g > maxByte) maxByte = g;
            if (b > maxByte) maxByte = b;
        }
    }
    SR_ASSERT(maxByte < 255);   // 不饱和（若默认太亮，改默认而不是改这条）

    // 关高光对照（Ks=0），数被高光提亮的像素。
    sr::MaterialShade noSpec = mat;
    noSpec.specular = sr::LinearColor{0.0f, 0.0f, 0.0f};
    sr::VertexUniforms vu2 = vu;
    vu2.mat = &noSpec;
    sr::FragmentUniforms fu2 = fu;
    fu2.mat = &noSpec;
    Canvas base{128, 128};
    sr::FrameStats sb;
    renderContent(base, sphere, sr::transformVS, sr::blinnPhongPerPixelFS, vu2, fu2, sb);
    int highlight = 0;
    for (int y = 0; y < 128; ++y) {
        for (int x = 0; x < 128; ++x) {
            const float f = sr::decodeSRGB(full.rt.color.readPacked(x, y)).r;
            const float b0 = sr::decodeSRGB(base.rt.color.readPacked(x, y)).r;
            if (f > b0 + 0.01f) ++highlight;
        }
    }
    SR_ASSERT(highlight > 0);   // 高光看得见
}

SR_TEST(attenuate_radius_zero_is_rejected_or_clamped_before_shading) {
    // P97：--attenuate-radius 0 在解析层被拒（点名 flag），且 buildLightRig 对程序化 0/负数
    // 夹到正值 —— 双保险，绝不让 0 到达 shading.h 的 d/radius 除法。
    app::AppOptions o;
    std::string err;
    char arg0[] = "soft-raster";
    char arg1[] = "--attenuate-radius";
    char arg2[] = "0";
    char* argv[] = {arg0, arg1, arg2};
    SR_ASSERT_FALSE(app::parseCmdline(3, argv, o, err));
    SR_ASSERT(err.find("--attenuate-radius") != std::string::npos);

    sr::BBox b;
    b.grow(sr::Vec3{-1.0f, -1.0f, -1.0f});
    b.grow(sr::Vec3{1.0f, 1.0f, 1.0f});
    sr::OrbitCamera cam;
    o = app::AppOptions{};
    o.attenuateRadius = 0.0f;   // 程序化塞 0（绕过解析层）
    const sr::LightRig rig = app::buildLightRig(o, b, cam);
    SR_ASSERT(rig.attenuateRadius > 0.0f);
    o.attenuateRadius = -5.0f;
    const sr::LightRig rig2 = app::buildLightRig(o, b, cam);
    SR_ASSERT(rig2.attenuateRadius > 0.0f);
}

SR_TEST(hud_shows_linear_and_raw_for_light_colors) {
    // D10：输入值与线性值都显示。输入非 0/1 时两个串必须不同；输入 0/1 时相同
    // （解码曲线两端精确）⇒ 显示不会静默坍缩成一个。
    app::AppOptions o;
    o.keyColor[0] = o.keyColor[1] = o.keyColor[2] = 0.5f;
    app::SceneState s;
    s.rig = app::buildLightRig(o, sr::BBox{}, s.camera);
    const std::string line = app::shadeHudLine(o, s);
    char raw[64], lin[64];
    std::snprintf(raw, sizeof(raw), "%.2f,%.2f,%.2f", o.keyColor[0], o.keyColor[1], o.keyColor[2]);
    const sr::LinearColor lc = sr::decodeLightColor(0.5f, 0.5f, 0.5f);
    std::snprintf(lin, sizeof(lin), "%.2f,%.2f,%.2f", lc.r, lc.g, lc.b);
    SR_ASSERT(line.find(raw) != std::string::npos);
    SR_ASSERT(line.find(lin) != std::string::npos);
    SR_ASSERT(std::string(raw) != std::string(lin));   // 0.5 ⇒ 0.50 vs 0.21
    SR_ASSERT(line.find("PHONG") != std::string::npos);
    SR_ASSERT(line.find("ATT OFF") != std::string::npos);

    // 输入 1.0 ⇒ 原始与线性相同（两端精确）。
    o.keyColor[0] = o.keyColor[1] = o.keyColor[2] = 1.0f;
    s.rig = app::buildLightRig(o, sr::BBox{}, s.camera);
    const std::string line2 = app::shadeHudLine(o, s);
    char raw2[64], lin2[64];
    std::snprintf(raw2, sizeof(raw2), "%.2f,%.2f,%.2f", 1.0f, 1.0f, 1.0f);
    const sr::LinearColor lc2 = sr::decodeLightColor(1.0f, 1.0f, 1.0f);
    std::snprintf(lin2, sizeof(lin2), "%.2f,%.2f,%.2f", lc2.r, lc2.g, lc2.b);
    SR_ASSERT(line2.find(raw2) != std::string::npos);
    SR_ASSERT(std::string(raw2) == std::string(lin2));
}

SR_TEST(shade_mode_cycle_and_plan_route_the_three_shaders) {
    SR_ASSERT(app::parseShadeMode("unlit") == app::ShadeMode::Unlit);
    SR_ASSERT(app::parseShadeMode("gouraud") == app::ShadeMode::Gouraud);
    SR_ASSERT(app::parseShadeMode("phong") == app::ShadeMode::Phong);
    // 循环：Phong → Gouraud → Unlit → Phong（默认按一下到 Gouraud）。
    SR_ASSERT(app::cycleShadeMode(app::ShadeMode::Phong) == app::ShadeMode::Gouraud);
    SR_ASSERT(app::cycleShadeMode(app::ShadeMode::Gouraud) == app::ShadeMode::Unlit);
    SR_ASSERT(app::cycleShadeMode(app::ShadeMode::Unlit) == app::ShadeMode::Phong);
    // 路由表（P101：默认 FS 只在这里写一份）。
    const app::ShadePlan u = app::planShade(app::ShadeMode::Unlit);
    SR_ASSERT(u.vertexShader == sr::transformVS);
    SR_ASSERT(u.fragmentShader == sr::texturedFS);
    SR_ASSERT(std::string(u.name) == "UNLIT");
    const app::ShadePlan g = app::planShade(app::ShadeMode::Gouraud);
    SR_ASSERT(g.vertexShader == sr::lambertVS);
    SR_ASSERT(g.fragmentShader == sr::lambertPerVertexFS);
    SR_ASSERT(std::string(g.name) == "GOURAUD");
    const app::ShadePlan p = app::planShade(app::ShadeMode::Phong);
    SR_ASSERT(p.vertexShader == sr::transformVS);
    SR_ASSERT(p.fragmentShader == sr::blinnPhongPerPixelFS);
    SR_ASSERT(std::string(p.name) == "PHONG");
}

SR_TEST(nm_degenerate_flag_reaches_the_hud) {
    // P98：setNormalMatrix 的 bool 被消费进 normalMatrixDegenerate，HUD 据此显示。
    app::SceneState s;
    const app::AppOptions o;
    s.normalMatrixDegenerate = false;
    SR_ASSERT(app::shadeHudLine(o, s).find("NM DEGENERATE") == std::string::npos);
    s.normalMatrixDegenerate = true;
    SR_ASSERT(app::shadeHudLine(o, s).find("NM DEGENERATE") != std::string::npos);
    // 奇异 model ⇒ prepareFrameUniforms 置位；单位阵 ⇒ 复位。
    s.modelMatrix = sr::Mat4::scale(sr::Vec3{0.0f, 1.0f, 1.0f});
    sr::VertexUniforms vu;
    sr::FragmentUniforms fu;
    app::prepareFrameUniforms(s, 64, 64, vu, fu);
    SR_ASSERT(s.normalMatrixDegenerate);
    s.modelMatrix = sr::Mat4::identity();
    app::prepareFrameUniforms(s, 64, 64, vu, fu);
    SR_ASSERT_FALSE(s.normalMatrixDegenerate);
}

SR_TEST(unlit_mode_reproduces_the_unlit_picture) {
    // app 通路的 M4/M5 回归锚：--shade unlit 的画面必须与 BASE 的显式装配逐字节相同。
    // oracle = BASE renderFrame 程序分支的字面装配（transformVS + texturedFS，lights/mat
    // 全 nullptr）；新侧走 planShade(Unlit) + prepareFrameUniforms（挂 &rig）。texturedFS
    // 不读 lights/mat ⇒ 挂了指针也不改像素。两侧都用生产单例（fallbackCube/goldenChecker）。
    app::SceneState s;   // 默认相机 yaw0/pitch0.35/dist5，near0.1/far100
    sr::VertexUniforms vuOld;
    vuOld.model = sr::Mat4::identity();
    vuOld.view = s.camera.view();
    vuOld.proj = sr::makePerspective(sr::kDefaultFovYRadians, 1.0f, s.nearPlane, s.farPlane);
    sr::setNormalMatrix(vuOld, vuOld.model, "");
    // lights/mat 保持 nullptr（BASE 原样）
    sr::FragmentUniforms fuOld = litFu();
    fuOld.texture = &sr::goldenCheckerTexture();
    Canvas old{128, 128};
    sr::FrameStats so;
    renderContent(old, app::fallbackCube(), sr::transformVS, sr::texturedFS, vuOld, fuOld, so);

    s.shade = app::ShadeMode::Unlit;
    sr::VertexUniforms vuNew;
    sr::FragmentUniforms fuNew = litFu();   // baseColor/sampler 与 oracle 同（提交分支自填的那半）
    app::prepareFrameUniforms(s, 128, 128, vuNew, fuNew);   // 覆写 lights/mat 指针为 &rig/&proceduralMat
    const app::ShadePlan sp = app::planShade(s.shade);
    fuNew.texture = &sr::goldenCheckerTexture();
    Canvas neu{128, 128};
    sr::FrameStats sn;
    renderContent(neu, app::fallbackCube(), sp.vertexShader, sp.fragmentShader, vuNew, fuNew, sn);
    SR_ASSERT(so.fragmentsWritten > 100);
    SR_ASSERT(old.colorMem == neu.colorMem);   // 逐字节相同
}
