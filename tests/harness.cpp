#include "harness.h"

#include <cstdio>
#include <cstring>

namespace srtest {

int currentFailures = 0;

std::vector<std::pair<const char*, Fn>>& suite() {
    static std::vector<std::pair<const char*, Fn>> s;
    return s;
}

Register::Register(const char* name, Fn fn) { suite().emplace_back(name, fn); }

// 三个 report* 都要 fflush：stdout 重定向到文件/管道时是**全缓冲**的，
// 用例先断言失败、紧接着又踩出崩溃（M4.1 的 chain[3] 越界就是这个形状）时，
// 缓冲区里的 FAIL 行会随进程一起消失，看起来像"崩得毫无征兆"。
void reportFailure(const char* file, int line, const char* expr) {
    ++currentFailures;
    std::printf("    FAIL %s:%d  断言不成立: %s\n", file, line, expr);
    std::fflush(stdout);
}

void reportFailureNear(const char* file, int line, const char* expr,
                       double actual, double expected, double tol) {
    ++currentFailures;
    std::printf("    FAIL %s:%d  %s  实际 %.9g  期望 %.9g  容差 %.3g  误差 %.9g\n",
                file, line, expr, actual, expected, tol, std::fabs(actual - expected));
    std::fflush(stdout);
}

void reportFailureEq(const char* file, int line, const char* expr,
                     long long actual, long long expected) {
    ++currentFailures;
    std::printf("    FAIL %s:%d  %s  实际 %lld  期望 %lld  (0x%08llX vs 0x%08llX)\n",
                file, line, expr, actual, expected,
                static_cast<unsigned long long>(actual) & 0xFFFFFFFFull,
                static_cast<unsigned long long>(expected) & 0xFFFFFFFFull);
    std::fflush(stdout);
}

bool nameMatchesFilter(const char* name, const char* filter) {
    if (filter == nullptr || filter[0] == '\0') return true;
    return std::strstr(name, filter) != nullptr;
}

bool parseArgs(int argc, char** argv, Options& out, std::string& err) {
    out.filter = nullptr;      // 复位：不能沿用调用方结构体里的残留值
    err.clear();
    for (int i = 1; i < argc; ++i) {
        const std::string arg = (argv[i] != nullptr) ? argv[i] : "";
        if (arg == "--only") {
            if (i + 1 >= argc) {
                err = "--only 后面缺少用例名";
                return false;
            }
            out.filter = argv[++i];
        } else {
            err = "无法识别的参数: " + arg;
            return false;
        }
    }
    return true;
}

int runAll(const Options& opts) {
    const auto& s = suite();
    const bool filtering = (opts.filter != nullptr && opts.filter[0] != '\0');

    int matched = 0;
    for (const auto& entry : s) {
        if (nameMatchesFilter(entry.first, opts.filter)) ++matched;
    }
    if (filtering && matched == 0) {
        // 一条都没匹配上必须是**失败**：过滤器拼错时报"0 个用例，失败 0 个 → 退出码 0"，
        // 比崩溃更难发现，因为它长得像全绿。
        std::printf("FAIL --only \"%s\" 没有匹配任何用例（共 %d 条，检查拼写）\n",
                    opts.filter, static_cast<int>(s.size()));
        return 1;
    }

    int failedCases = 0;
    if (filtering) {
        std::printf("[run] %d/%d 个用例（过滤: %s）\n", matched, static_cast<int>(s.size()),
                    opts.filter);
    } else {
        std::printf("[run] %d 个用例\n", static_cast<int>(s.size()));
    }
    for (const auto& entry : s) {
        if (!nameMatchesFilter(entry.first, opts.filter)) continue;
        currentFailures = 0;
        if (filtering) {
            // 崩在哪个用例，就让它成为 stdout 上最后一行。**必须 fflush**：
            // 进程被杀时缓冲区里的东西不会落盘，而"崩溃且没有任何输出"正是这次要解决的问题。
            std::printf("[case] %s\n", entry.first);
            std::fflush(stdout);
        }
        entry.second();
        if (currentFailures != 0) {
            ++failedCases;
            std::printf("FAIL %s  (%d 处断言失败)\n", entry.first, currentFailures);
        }
    }
    std::printf("[done] %d 个用例，失败 %d 个\n", matched, failedCases);
    return failedCases;
}

}  // namespace srtest
