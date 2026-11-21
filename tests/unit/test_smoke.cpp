#include "harness.h"

#include <string>

// smoke：确认 harness 本身在被使用之前是可信的。
// Step 5 会临时把第一条断言改成 1 + 1 == 3，验证失败路径真的能被报告出来。

SR_TEST(harness_detects_a_passing_assert) { SR_ASSERT(1 + 1 == 2); }

SR_TEST(harness_near_tolerance_is_inclusive) { SR_ASSERT_NEAR(1.0f, 1.000001f, 1e-5f); }

SR_TEST(harness_eq_compares_integer_values) { SR_ASSERT_EQ(0xFF112233u, 0xFF112233u); }

// ---- --only 用例名过滤 ----------------------------------------------------
// 动机（M4.1）：一个用例崩溃时整个进程没有输出，"摘掉整个文件再跑"只能定位到文件。
// 过滤逻辑必须能单独断言，所以它是自由函数而不是 runAll 里的几行。

SR_TEST(harness_filter_matches_a_substring_anywhere_in_the_name) {
    SR_ASSERT(srtest::nameMatchesFilter("mip_chain_halves_until_one_reaches_one", "mip"));
    SR_ASSERT(srtest::nameMatchesFilter("mip_chain_halves_until_one_reaches_one", "chain_halves"));
    SR_ASSERT(srtest::nameMatchesFilter("mip_chain_halves_until_one_reaches_one", "one_reaches"));
    SR_ASSERT_FALSE(srtest::nameMatchesFilter("mip_chain", "chain_mip"));   // 不是集合相交，是子串
}

SR_TEST(harness_no_filter_selects_every_case) {
    // filter 为空指针或空串时必须放行全部 —— 否则 CTest 那条不带参数的命令行会静默跑 0 个用例。
    SR_ASSERT(srtest::nameMatchesFilter("any_case_name", nullptr));
    SR_ASSERT(srtest::nameMatchesFilter("any_case_name", ""));
}

SR_TEST(harness_parse_args_reads_the_only_flag) {
    char* argv[] = {const_cast<char*>("unit_tests"), const_cast<char*>("--only"),
                    const_cast<char*>("mip_chain")};
    srtest::Options opts;
    std::string err;
    SR_ASSERT(srtest::parseArgs(3, argv, opts, err));
    SR_ASSERT(err.empty());
    SR_ASSERT(opts.filter != nullptr);
    if (opts.filter != nullptr) SR_ASSERT(std::string(opts.filter) == "mip_chain");
}

SR_TEST(harness_parse_args_without_flags_runs_everything) {
    char* argv[] = {const_cast<char*>("unit_tests")};
    srtest::Options opts;
    opts.filter = "leftover";        // 证明 parseArgs 会把没给的参数复位，而不是沿用调用方的垃圾
    std::string err;
    SR_ASSERT(srtest::parseArgs(1, argv, opts, err));
    SR_ASSERT(opts.filter == nullptr);
}

SR_TEST(harness_parse_args_rejects_an_only_flag_with_no_value) {
    // 这条是防"手滑敲了 --only 就以为在跑过滤"：必须报错退出，不能当成没参数。
    char* argv[] = {const_cast<char*>("unit_tests"), const_cast<char*>("--only")};
    srtest::Options opts;
    std::string err;
    SR_ASSERT_FALSE(srtest::parseArgs(2, argv, opts, err));
    SR_ASSERT_FALSE(err.empty());
}

SR_TEST(harness_parse_args_rejects_an_unknown_flag) {
    char* argv[] = {const_cast<char*>("unit_tests"), const_cast<char*>("--onlyy"),
                    const_cast<char*>("mip")};
    srtest::Options opts;
    std::string err;
    SR_ASSERT_FALSE(srtest::parseArgs(3, argv, opts, err));
    // err 里必须带上那个拼错的 flag，否则报错信息和病因一样模糊。
    SR_ASSERT(err.find("--onlyy") != std::string::npos);
}
