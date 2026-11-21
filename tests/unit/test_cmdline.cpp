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
