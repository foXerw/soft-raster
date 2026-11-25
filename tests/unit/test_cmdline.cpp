#include "app/cmdline.h"

#include <cstddef>
#include <string>
#include <vector>

#include "harness.h"

namespace {

// argv 的形状要真实：app/cmdline.cpp 被测试目标链接，靠的就是它不碰任何 Win32 头（C4）。
struct Args {
    std::vector<std::string> store;
    std::vector<const char*> ptrs;
    explicit Args(std::vector<std::string> in) : store(std::move(in)) {
        store.insert(store.begin(), "soft-raster");
        ptrs.reserve(store.size());
        for (const auto& s : store) ptrs.push_back(s.c_str());
    }
    int argc() const { return static_cast<int>(ptrs.size()); }
    // const 限定在这里是安全的：ptrs 里的每个元素都指向 store 中稳定的 std::string，
    // const_cast 只是为了拿到 argv 的形状（char**），被调方不会写入。
    char** argv() const { return const_cast<char**>(ptrs.data()); }
};

app::AppOptions parse(const Args& in, bool& ok, std::string& err) {
    app::AppOptions opts;
    ok = app::parseCmdline(in.argc(), const_cast<char**>(in.argv()), opts, err);
    return opts;
}

}  // namespace

SR_TEST(cmdline_defaults_match_the_specification) {
    Args a{{}};
    bool ok = false;
    std::string err;
    const auto o = parse(a, ok, err);
    SR_ASSERT(ok);
    SR_ASSERT(err.empty());
    SR_ASSERT_EQ(o.width, 1280);      // 设计文档 §10.6：默认 1280x720
    SR_ASSERT_EQ(o.height, 720);
    SR_ASSERT_FALSE(o.offscreen);
    SR_ASSERT_EQ(o.frames, 1);
    SR_ASSERT(o.backend == "a");
    SR_ASSERT_EQ(o.threads, 0);        // 0 = "用满逻辑核"，由 Backend B 自己解释
    SR_ASSERT_FALSE(o.noFit);
    SR_ASSERT(o.obj.empty());
    SR_ASSERT(o.out.empty());
}

SR_TEST(cmdline_applies_overrides) {
    Args a{{"--width", "640", "--height", "360", "--backend", "b", "--threads", "4"}};
    bool ok = false;
    std::string err;
    const auto o = parse(a, ok, err);
    SR_ASSERT(ok);
    SR_ASSERT_EQ(o.width, 640);
    SR_ASSERT_EQ(o.height, 360);
    SR_ASSERT(o.backend == "b");
    SR_ASSERT_EQ(o.threads, 4);
}

SR_TEST(cmdline_offscreen_mode) {
    Args a{{"--offscreen", "--frames", "3", "--out", "shot.bmp"}};
    bool ok = false;
    std::string err;
    const auto o = parse(a, ok, err);
    SR_ASSERT(ok);
    SR_ASSERT(o.offscreen);
    SR_ASSERT_EQ(o.frames, 3);
    SR_ASSERT(o.out == "shot.bmp");
}

SR_TEST(cmdline_accepts_obj_and_no_fit) {
    Args a{{"--obj", "assets/sphere.obj", "--no-fit"}};
    bool ok = false;
    std::string err;
    const auto o = parse(a, ok, err);
    SR_ASSERT(ok);
    SR_ASSERT(o.obj == "assets/sphere.obj");
    SR_ASSERT(o.noFit);
}

SR_TEST(cmdline_object_path_with_spaces_is_kept_whole) {
    // Windows 用户会把模型放在 "C:\Users\...\My Models\thing.obj"。
    // argv 已经由 shell 拆好，这里要确认我们没有二次切分这个值。
    Args a{{"--obj", "C:\\Models\\My Thing\\thing.obj"}};
    bool ok = false;
    std::string err;
    const auto o = parse(a, ok, err);
    SR_ASSERT(ok);
    SR_ASSERT(o.obj == "C:\\Models\\My Thing\\thing.obj");
}

SR_TEST(cmdline_rejects_unknown_flag_and_names_it) {
    Args a{{"--wobble"}};
    bool ok = false;
    std::string err;
    parse(a, ok, err);
    SR_ASSERT_FALSE(ok);
    SR_ASSERT(err.find("--wobble") != std::string::npos);
}

SR_TEST(cmdline_rejects_missing_value) {
    Args a{{"--width"}};
    bool ok = false;
    std::string err;
    parse(a, ok, err);
    SR_ASSERT_FALSE(ok);
    SR_ASSERT(err.find("--width") != std::string::npos);
}

SR_TEST(cmdline_rejects_non_numeric_and_out_of_range_values) {
    bool ok = false;
    std::string err;
    {
        Args a{{"--width", "big"}};
        parse(a, ok, err);
        SR_ASSERT_FALSE(ok);
        SR_ASSERT(err.find("--width") != std::string::npos);
    }
    {
        Args a{{"--width", "0"}};
        parse(a, ok, err);
        SR_ASSERT_FALSE(ok);
    }
    {
        Args a{{"--width", "-4"}};
        parse(a, ok, err);
        SR_ASSERT_FALSE(ok);
    }
    {
        // 上限是防止"手滑打了 8 个 0"就把 8GB 内存分配下去：宁可报错。
        Args a{{"--width", "100000"}};
        parse(a, ok, err);
        SR_ASSERT_FALSE(ok);
        SR_ASSERT(err.find("100000") != std::string::npos);
    }
}

SR_TEST(cmdline_rejects_negative_threads_and_bad_backend) {
    bool ok = false;
    std::string err;
    {
        Args a{{"--threads", "-1"}};
        parse(a, ok, err);
        SR_ASSERT_FALSE(ok);
    }
    {
        Args a{{"--backend", "c"}};
        parse(a, ok, err);
        SR_ASSERT_FALSE(ok);
        SR_ASSERT(err.find("backend") != std::string::npos);
    }
}

SR_TEST(cmdline_bench_requires_frames_at_least_one) {
    bool ok = false;
    std::string err;
    {
        Args a{{"--bench", "30"}};
        const auto o = parse(a, ok, err);
        SR_ASSERT(ok);
        SR_ASSERT_EQ(o.bench, 30);
        SR_ASSERT(o.offscreen);   // bench 是离屏测量入口，隐含 offscreen
    }
    {
        Args a{{"--bench", "0"}};
        parse(a, ok, err);
        SR_ASSERT_FALSE(ok);
    }
}

SR_TEST(cmdline_help_is_not_an_error) {
    Args a{{"--help"}};
    bool ok = false;
    std::string err;
    const auto o = parse(a, ok, err);
    SR_ASSERT(ok);
    SR_ASSERT(o.help);
    SR_ASSERT_FALSE(app::usageString().empty());   // 报错时要把用法打出来，所以它必须非空
}

SR_TEST(cmdline_later_flags_win_so_a_typo_order_does_not_silently_take_the_default) {
    Args a{{"--width", "300", "--width", "400"}};
    bool ok = false;
    std::string err;
    const auto o = parse(a, ok, err);
    SR_ASSERT(ok);
    SR_ASSERT_EQ(o.width, 400);
}

SR_TEST(cmdline_auto_close_is_separate_from_offscreen_frames) {
    // 两个数字很容易混：--frames 说的是"离屏渲染多少帧"，--auto-close 说的是
    // "窗口开多久"。混用的话，冒烟测试就没法在不改变输出内容的前提下让窗口自己关掉。
    Args a{{"--auto-close", "5"}};
    bool ok = false;
    std::string err;
    const auto o = parse(a, ok, err);
    SR_ASSERT(ok);
    SR_ASSERT_EQ(o.autoClose, 5);
    SR_ASSERT_EQ(o.frames, 1);          // 没有被顺手改掉
    SR_ASSERT_FALSE(o.offscreen);       // 窗口模式依然开着
}

SR_TEST(cmdline_rejects_auto_close_below_one) {
    Args a{{"--auto-close", "0"}};
    bool ok = false;
    std::string err;
    parse(a, ok, err);
    SR_ASSERT_FALSE(ok);
    SR_ASSERT(err.find("--auto-close") != std::string::npos);
}

SR_TEST(cmdline_golden_write_textured_is_a_separate_entry) {
    // 两个 golden 入口必须**互不越界**：写 M4 的参考图不能顺手把 M3 的路径也填上。
    // 若哪天有人合并它们，这条会红 —— 而合并的真实代价是"重新生成基准图后一切正常"，
    // 那种故障没有别的信号。
    Args a{{"--golden-write-textured", "assets/golden/m4_textured_cube.bmp"}};
    bool ok = false;
    std::string err;
    const auto o = parse(a, ok, err);
    SR_ASSERT(ok);
    SR_ASSERT(err.empty());
    SR_ASSERT(o.goldenWriteTextured == "assets/golden/m4_textured_cube.bmp");
    SR_ASSERT(o.goldenWrite.empty());   // 没有被顺手填上
    SR_ASSERT(o.offscreen);             // 写 golden 不需要窗口（与 --golden-write 一致）
    SR_ASSERT_EQ(o.frames, 1);          // 也没有顺手改掉帧数
}

SR_TEST(cmdline_rejects_two_golden_entries_together) {
    // 同时给两个入口时，"先检查谁"决定写哪张图 —— 于是第二个被**静默忽略**：
    // 用户以为两张都刷新了，实际只有一张变了。这与 Ruling 38 守的是同一个坑
    // （golden 相关的故障都不该有"看起来一切正常"这条路径），所以宁可拒绝。
    Args a{{"--golden-write", "a.bmp", "--golden-write-textured", "b.bmp"}};
    bool ok = false;
    std::string err;
    parse(a, ok, err);
    SR_ASSERT_FALSE(ok);
    if (err.find("golden") == std::string::npos) std::printf("[cmdline] 错误消息是：%s\n", err.c_str());
    SR_ASSERT(err.find("golden") != std::string::npos);
    // 顺序反过来也必须拒绝（否则"先写谁"又变成参数顺序的事）
    Args b{{"--golden-write-textured", "b.bmp", "--golden-write", "a.bmp"}};
    parse(b, ok, err);
    SR_ASSERT_FALSE(ok);
    SR_ASSERT(err.find("golden") != std::string::npos);
}

SR_TEST(cmdline_rejects_golden_write_textured_without_a_path) {
    Args a{{"--golden-write-textured"}};
    bool ok = false;
    std::string err;
    parse(a, ok, err);
    SR_ASSERT_FALSE(ok);
    SR_ASSERT(err.find("--golden-write-textured") != std::string::npos);
}

SR_TEST(cmdline_emit_checker_tga_is_a_separate_entry) {
    // Ruling 38 的第三个实例：资产生成入口不带默认值、不越界填别的字段。
    // 它不渲染（只写 makeCheckerImage 的字节），所以也不该顺手打开 offscreen。
    Args a{{"--emit-checker-tga", "assets/tga/checker.tga"}};
    bool ok = false;
    std::string err;
    const auto o = parse(a, ok, err);
    SR_ASSERT(ok);
    SR_ASSERT(err.empty());
    SR_ASSERT(o.emitCheckerTga == "assets/tga/checker.tga");
    SR_ASSERT(o.goldenWrite.empty());           // 没有被顺手填上
    SR_ASSERT(o.goldenWriteTextured.empty());
    SR_ASSERT(o.obj.empty());
    SR_ASSERT_FALSE(o.offscreen);               // 不渲染，无需离屏
}

SR_TEST(cmdline_emit_checker_tga_requires_a_value) {
    Args a{{"--emit-checker-tga"}};
    bool ok = false;
    std::string err;
    parse(a, ok, err);
    SR_ASSERT_FALSE(ok);
    SR_ASSERT(err.find("--emit-checker-tga") != std::string::npos);
    SR_ASSERT(err.find("缺少值") != std::string::npos);
}

SR_TEST(cmdline_rejects_emit_checker_tga_together_with_golden_entries) {
    // 与 cmdline_rejects_two_golden_entries_together 同一条理由：main 里
    // emit-checker-tga 分支先返回，同时给 golden 入口会被**静默忽略** ——
    // "我以为参考图也刷新了"正是 Ruling 38 要消灭的那类假通过。
    Args a{{"--emit-checker-tga", "a.tga", "--golden-write-textured", "b.bmp"}};
    bool ok = false;
    std::string err;
    parse(a, ok, err);
    SR_ASSERT_FALSE(ok);
    SR_ASSERT(err.find("emit-checker-tga") != std::string::npos);
    Args b{{"--golden-write", "a.bmp", "--emit-checker-tga", "b.tga"}};
    parse(b, ok, err);
    SR_ASSERT_FALSE(ok);
    SR_ASSERT(err.find("emit-checker-tga") != std::string::npos);
}

SR_TEST(cmdline_golden_write_obj_pairs_with_obj) {
    // Ruling 38 的第四个实例：OBJ golden 有自己的显式入口，不带默认场景。
    // 它需要 --obj 说明载入哪个模型；两个 flag 的顺序不该影响结果（检查在循环之后）。
    Args a{{"--obj", "assets/obj/cube.obj", "--golden-write-obj", "build/m5.bmp"}};
    bool ok = false;
    std::string err;
    const auto o = parse(a, ok, err);
    SR_ASSERT(ok);
    SR_ASSERT(err.empty());
    SR_ASSERT(o.goldenWriteObj == "build/m5.bmp");
    SR_ASSERT(o.obj == "assets/obj/cube.obj");
    SR_ASSERT(o.goldenWrite.empty());           // 没有被顺手填上
    SR_ASSERT(o.goldenWriteTextured.empty());
    SR_ASSERT(o.emitCheckerTga.empty());
    SR_ASSERT(o.offscreen);                     // 写 golden 不需要窗口
    Args b{{"--golden-write-obj", "build/m5.bmp", "--obj", "assets/obj/cube.obj"}};
    const auto o2 = parse(b, ok, err);
    SR_ASSERT(ok);
    SR_ASSERT(o2.goldenWriteObj == "build/m5.bmp");
    SR_ASSERT(o2.obj == "assets/obj/cube.obj");
}

SR_TEST(cmdline_golden_write_obj_without_obj_is_an_error_not_a_default_scene) {
    // 缺 --obj ⇒ 明确报错。给一个"默认场景"意味着忘了指定模型时画出来的东西
    // 看起来一切正常 —— 与 Ruling 38 要消灭的假通过同族。
    Args a{{"--golden-write-obj", "build/m5.bmp"}};
    bool ok = false;
    std::string err;
    parse(a, ok, err);
    SR_ASSERT_FALSE(ok);
    if (err.find("--obj") == std::string::npos) std::printf("[cmdline] 错误消息是：%s\n", err.c_str());
    SR_ASSERT(err.find("--obj") != std::string::npos);
    SR_ASSERT(err.find("--golden-write-obj") != std::string::npos);
}

SR_TEST(cmdline_golden_write_obj_requires_a_value) {
    Args a{{"--obj", "x.obj", "--golden-write-obj"}};
    bool ok = false;
    std::string err;
    parse(a, ok, err);
    SR_ASSERT_FALSE(ok);
    SR_ASSERT(err.find("--golden-write-obj") != std::string::npos);
    SR_ASSERT(err.find("缺少值") != std::string::npos);
}

SR_TEST(cmdline_rejects_golden_write_obj_together_with_other_golden_entries) {
    // 与 cmdline_rejects_two_golden_entries_together 同一条理由：main 里先匹配的分支
    // 先返回，同时给两个入口会被静默忽略 —— "我以为两张都刷新了"没有别的信号。
    Args a{{"--obj", "x.obj", "--golden-write-obj", "a.bmp", "--golden-write", "b.bmp"}};
    bool ok = false;
    std::string err;
    parse(a, ok, err);
    SR_ASSERT_FALSE(ok);
    SR_ASSERT(err.find("golden") != std::string::npos);
    Args b{{"--obj", "x.obj", "--golden-write-textured", "b.bmp", "--golden-write-obj", "a.bmp"}};
    parse(b, ok, err);
    SR_ASSERT_FALSE(ok);
    SR_ASSERT(err.find("golden") != std::string::npos);
}

SR_TEST(cmdline_rejects_emit_checker_tga_together_with_golden_write_obj) {
    Args a{{"--obj", "x.obj", "--emit-checker-tga", "a.tga", "--golden-write-obj", "b.bmp"}};
    bool ok = false;
    std::string err;
    parse(a, ok, err);
    SR_ASSERT_FALSE(ok);
    SR_ASSERT(err.find("emit-checker-tga") != std::string::npos);
}

// ---- M6.8：lit golden 入口（Ruling 38 的第五个实例）--------------------------

SR_TEST(cmdline_golden_write_lit_is_a_separate_entry) {
    // lit golden 有自己的显式入口，不带默认场景选择（Ruling 38）：与另外四个
    // golden/emit 入口互不越界。合并入口的真实代价是"重新生成基准图后一切正常"。
    Args a{{"--golden-write-lit", "assets/golden/m6_lit_sphere.bmp"}};
    bool ok = false;
    std::string err;
    const auto o = parse(a, ok, err);
    SR_ASSERT(ok);
    SR_ASSERT(err.empty());
    SR_ASSERT(o.goldenWriteLit == "assets/golden/m6_lit_sphere.bmp");
    SR_ASSERT(o.goldenWrite.empty());           // 没有被顺手填上
    SR_ASSERT(o.goldenWriteTextured.empty());
    SR_ASSERT(o.goldenWriteObj.empty());
    SR_ASSERT(o.emitCheckerTga.empty());
    SR_ASSERT(o.offscreen);                     // 写 golden 不需要窗口
    SR_ASSERT_EQ(o.frames, 1);
}

SR_TEST(cmdline_golden_write_lit_requires_a_value) {
    Args a{{"--golden-write-lit"}};
    bool ok = false;
    std::string err;
    parse(a, ok, err);
    SR_ASSERT_FALSE(ok);
    SR_ASSERT(err.find("--golden-write-lit") != std::string::npos);
    SR_ASSERT(err.find("缺少值") != std::string::npos);
}

SR_TEST(cmdline_rejects_golden_write_lit_together_with_every_other_entry) {
    // 5 选 1（M5.3/M5.7 的既有写法扩成五个）：lit 与另外四个入口两两同给都必须拒绝，
    // 两个顺序都拒绝（否则"先写谁"又变成参数顺序的事）。main 里先匹配的分支先返回，
    // 同时给两个入口会被静默忽略 —— "我以为两张都刷新了"没有别的信号。
    bool ok = false;
    std::string err;
    const char* others[] = {"--golden-write", "--golden-write-textured", "--golden-write-obj",
                            "--emit-checker-tga"};
    for (const char* other : others) {
        Args a{{"--golden-write-lit", "a.bmp", other, "b.bmp"}};
        parse(a, ok, err);
        SR_ASSERT_FALSE(ok);
        SR_ASSERT(err.find("golden") != std::string::npos ||
                  err.find("emit-checker-tga") != std::string::npos);
        Args b{{other, "b.bmp", "--golden-write-lit", "a.bmp"}};
        parse(b, ok, err);
        SR_ASSERT_FALSE(ok);
        SR_ASSERT(err.find("golden") != std::string::npos ||
                  err.find("emit-checker-tga") != std::string::npos);
    }
}

// ---- M6.6：光照 flag（G-M6.4 / D24 命令行清单）------------------------------

SR_TEST(cmdline_accepts_the_light_flags) {
    // 设计 §9 的清单：--key-color --key-dir --key-intensity --point-color --point-pos
    // --point-intensity --ambient --ambient-color --attenuate --attenuate-radius --shade
    // （10 个光 flag + --shade = 11）。每个 flag 一条断言；值一律**原始 sRGB 域**存进
    // AppOptions（解码归 buildLightRig，这里不许提前解）。
    Args a{{"--key-color", "0.2", "0.4", "0.6",
            "--key-dir", "0", "0", "-1",
            "--key-intensity", "0.7",
            "--point-color", "0.1", "0.3", "0.5",
            "--point-pos", "1", "2", "3",
            "--point-intensity", "0.9",
            "--ambient", "0.5",
            "--ambient-color", "0.8", "0.6", "0.4",
            "--attenuate",
            "--attenuate-radius", "5",
            "--shade", "gouraud"}};
    bool ok = false;
    std::string err;
    const auto o = parse(a, ok, err);
    SR_ASSERT(ok);
    SR_ASSERT(err.empty());
    SR_ASSERT_NEAR(o.keyColor[0], 0.2f, 1e-6f);
    SR_ASSERT_NEAR(o.keyColor[1], 0.4f, 1e-6f);
    SR_ASSERT_NEAR(o.keyColor[2], 0.6f, 1e-6f);
    SR_ASSERT_NEAR(o.keyDir[0], 0.0f, 1e-6f);
    SR_ASSERT_NEAR(o.keyDir[2], -1.0f, 1e-6f);
    SR_ASSERT_NEAR(o.keyIntensity, 0.7f, 1e-6f);
    SR_ASSERT_NEAR(o.pointColor[0], 0.1f, 1e-6f);
    SR_ASSERT_NEAR(o.pointColor[1], 0.3f, 1e-6f);
    SR_ASSERT_NEAR(o.pointColor[2], 0.5f, 1e-6f);
    SR_ASSERT_NEAR(o.pointPos[0], 1.0f, 1e-6f);
    SR_ASSERT_NEAR(o.pointPos[1], 2.0f, 1e-6f);
    SR_ASSERT_NEAR(o.pointPos[2], 3.0f, 1e-6f);
    SR_ASSERT(o.pointPosGiven);
    SR_ASSERT_NEAR(o.pointIntensity, 0.9f, 1e-6f);
    SR_ASSERT_NEAR(o.ambientIntensity, 0.5f, 1e-6f);
    SR_ASSERT_NEAR(o.ambientColor[0], 0.8f, 1e-6f);
    SR_ASSERT_NEAR(o.ambientColor[1], 0.6f, 1e-6f);
    SR_ASSERT_NEAR(o.ambientColor[2], 0.4f, 1e-6f);
    SR_ASSERT(o.attenuate);
    SR_ASSERT_NEAR(o.attenuateRadius, 5.0f, 1e-6f);
    SR_ASSERT(o.shade == "gouraud");

    // 缺值报错必须带 flag 名（用户唯一能立刻行动的信息）。逐个 valued flag 验。
    const char* valued[] = {"--key-color", "--key-dir", "--key-intensity", "--point-color",
                            "--point-pos", "--point-intensity", "--ambient", "--ambient-color",
                            "--attenuate-radius", "--shade"};
    for (const char* f : valued) {
        Args m{{f}};
        parse(m, ok, err);
        SR_ASSERT_FALSE(ok);
        if (err.find(f) == std::string::npos)
            std::printf("[cmdline] %s 缺值错误是：%s\n", f, err.c_str());
        SR_ASSERT(err.find(f) != std::string::npos);
    }
    // 三元组只给一个分量 ⇒ 第二个分量缺值，同样点名 flag（不是静默补 0）。
    Args partial{{"--key-color", "0.5"}};
    parse(partial, ok, err);
    SR_ASSERT_FALSE(ok);
    SR_ASSERT(err.find("--key-color") != std::string::npos);
}

SR_TEST(cmdline_shade_mode_parses_and_rejects_typo) {
    bool ok = false;
    std::string err;
    {
        Args a{{"--shade", "unlit"}};
        const auto o = parse(a, ok, err);
        SR_ASSERT(ok);
        SR_ASSERT(o.shade == "unlit");
    }
    {
        Args a{{"--shade", "gouraud"}};
        const auto o = parse(a, ok, err);
        SR_ASSERT(ok);
        SR_ASSERT(o.shade == "gouraud");
    }
    {
        Args a{{"--shade", "phong"}};
        const auto o = parse(a, ok, err);
        SR_ASSERT(ok);
        SR_ASSERT(o.shade == "phong");
    }
    {
        // 拼错必须响亮拒绝（与 --backend c 同形）：静默退回默认会让人以为拼对了。
        Args a{{"--shade", "phongg"}};
        parse(a, ok, err);
        SR_ASSERT_FALSE(ok);
        SR_ASSERT(err.find("--shade") != std::string::npos);
        SR_ASSERT(err.find("phongg") != std::string::npos);
    }
    {
        // 默认就是 phong（D19）：不给 --shade 时不是 unlit。
        Args a{{}};
        const auto o = parse(a, ok, err);
        SR_ASSERT(ok);
        SR_ASSERT(o.shade == "phong");
    }
}

SR_TEST(cmdline_light_defaults_match_d19_and_attenuate_radius_sentinel) {
    // 默认值是不饱和高光的前提（D19）。pointIntensity 从简报初稿 1.0 下调到 0.4 的
    // 推导见 test_scene 的 default_rig_on_the_lit_sphere_does_not_saturate。
    Args a{{}};
    bool ok = false;
    std::string err;
    const auto o = parse(a, ok, err);
    SR_ASSERT(ok);
    SR_ASSERT_NEAR(o.keyIntensity, 0.35f, 1e-6f);
    SR_ASSERT_NEAR(o.pointIntensity, 0.4f, 1e-6f);
    SR_ASSERT_FALSE(o.attenuate);          // D7 默认关
    SR_ASSERT_FALSE(o.pointPosGiven);      // 默认跟随 eye（D5）
    SR_ASSERT(o.attenuateRadius < 0.0f);   // 哨兵：默认 = bounds.radius()*3，由 buildLightRig 算
    // --attenuate-radius 0 / 负数在解析层就被拒（P97：不许到达 shading.h 的除法）。
    {
        Args z{{"--attenuate-radius", "0"}};
        parse(z, ok, err);
        SR_ASSERT_FALSE(ok);
        SR_ASSERT(err.find("--attenuate-radius") != std::string::npos);
    }
    {
        Args n{{"--attenuate-radius", "-2"}};
        parse(n, ok, err);
        SR_ASSERT_FALSE(ok);
        SR_ASSERT(err.find("--attenuate-radius") != std::string::npos);
    }
}

SR_TEST(cmdline_light_colors_reject_out_of_range_and_nan_at_parse) {
    // R103（P103 骑手，R100 同形）：--key-color 2 0 0 曾在解析时合法，buildLightRig 的
    // srgbToLinear 把它**静默改写**成 1 0 0（夹取 + NaN→0）—— 命令行是明确表达的意图，
    // 不许被下游悄悄改掉。三个颜色 flag 的分量必须在解析时就落在 [0,1]（sRGB 域），
    // NaN 同样拒绝（`!(v>=0 && v<=1)` 对 NaN 天然为真）。错误消息点名 flag 与允许范围。
    bool ok = false;
    std::string err;
    {
        Args a{{"--key-color", "2", "0", "0"}};
        parse(a, ok, err);
        SR_ASSERT_FALSE(ok);
        SR_ASSERT(err.find("--key-color") != std::string::npos);
        SR_ASSERT(err.find("[0, 1]") != std::string::npos);   // 允许范围要在消息里
    }
    {
        Args a{{"--point-color", "0", "-0.1", "0"}};
        parse(a, ok, err);
        SR_ASSERT_FALSE(ok);
        SR_ASSERT(err.find("--point-color") != std::string::npos);
        SR_ASSERT(err.find("[0, 1]") != std::string::npos);
    }
    {
        // NaN：strtof 接受 "nan"，srgbToLinear 会把它映射成 0 —— 必须在边界拒绝。
        Args a{{"--ambient-color", "nan", "0", "0"}};
        parse(a, ok, err);
        SR_ASSERT_FALSE(ok);
        SR_ASSERT(err.find("--ambient-color") != std::string::npos);
        SR_ASSERT(err.find("[0, 1]") != std::string::npos);
    }
    {
        // 边界 0 与 1 本身合法（闭区间）；三个 flag 都走同一条检查。
        Args a{{"--key-color", "0", "1", "0.5", "--point-color", "1", "0", "1",
                "--ambient-color", "0.5", "0.5", "0.5"}};
        const auto o = parse(a, ok, err);
        SR_ASSERT(ok);
        SR_ASSERT_NEAR(o.keyColor[0], 0.0f, 1e-6f);
        SR_ASSERT_NEAR(o.keyColor[1], 1.0f, 1e-6f);
        SR_ASSERT_NEAR(o.pointColor[2], 1.0f, 1e-6f);
    }
    {
        // --key-dir / --point-pos 是方向/坐标，**不是**颜色 ⇒ 不受 [0,1] 约束
        //（这条边界区分是故意的：R103 的界限来自 sRGB 域，不是审美上限）。
        Args a{{"--key-dir", "-2", "0", "0", "--point-pos", "9", "8", "7"}};
        const auto o = parse(a, ok, err);
        SR_ASSERT(ok);
        SR_ASSERT_NEAR(o.keyDir[0], -2.0f, 1e-6f);
        SR_ASSERT_NEAR(o.pointPos[0], 9.0f, 1e-6f);
    }
}

SR_TEST(cmdline_steppable_intensities_reject_above_three_at_parse) {
    // R100（P103）：--key-intensity 5 曾在解析时合法，第一次按 = 才被夹到 3 ——
    // 命令行是明确表达的意图，不许被静默改写（与 R99 同构）。>3 必须在**解析时**拒绝，
    // 错误消息点名 flag 与允许范围。按键步进的运行时夹钳 [0,3] 保留（那是另一个边界）。
    bool ok = false;
    std::string err;
    {
        Args a{{"--key-intensity", "5"}};
        parse(a, ok, err);
        SR_ASSERT_FALSE(ok);
        SR_ASSERT(err.find("--key-intensity") != std::string::npos);
        SR_ASSERT(err.find("3") != std::string::npos);   // 允许范围要在消息里
    }
    {
        // 点光强度走同一个 [0,3] 按键夹钳（[/] 键），同一条理由适用。
        Args a{{"--point-intensity", "5"}};
        parse(a, ok, err);
        SR_ASSERT_FALSE(ok);
        SR_ASSERT(err.find("--point-intensity") != std::string::npos);
        SR_ASSERT(err.find("3") != std::string::npos);
    }
    {
        // 边界 3 本身合法（夹钳区间是闭区间）。
        Args a{{"--key-intensity", "3", "--point-intensity", "3"}};
        const auto o = parse(a, ok, err);
        SR_ASSERT(ok);
        SR_ASSERT_NEAR(o.keyIntensity, 3.0f, 1e-6f);
        SR_ASSERT_NEAR(o.pointIntensity, 3.0f, 1e-6f);
    }
    {
        // --ambient 没有按键步进 ⇒ 没有 [0,3] 夹钳 ⇒ 5 是合法的线性乘子。
        // 这条边界区分是故意的：R100 的界限来自"运行时会被夹"，不是审美上限。
        Args a{{"--ambient", "5"}};
        const auto o = parse(a, ok, err);
        SR_ASSERT(ok);
        SR_ASSERT_NEAR(o.ambientIntensity, 5.0f, 1e-6f);
    }
}
