#include "harness.h"

#include <cstdio>

namespace srtest {

int currentFailures = 0;

std::vector<std::pair<const char*, Fn>>& suite() {
    static std::vector<std::pair<const char*, Fn>> s;
    return s;
}

Register::Register(const char* name, Fn fn) { suite().emplace_back(name, fn); }

void reportFailure(const char* file, int line, const char* expr) {
    ++currentFailures;
    std::printf("    FAIL %s:%d  断言不成立: %s\n", file, line, expr);
}

void reportFailureNear(const char* file, int line, const char* expr,
                       double actual, double expected, double tol) {
    ++currentFailures;
    std::printf("    FAIL %s:%d  %s  实际 %.9g  期望 %.9g  容差 %.3g  误差 %.9g\n",
                file, line, expr, actual, expected, tol, std::fabs(actual - expected));
}

void reportFailureEq(const char* file, int line, const char* expr,
                     long long actual, long long expected) {
    ++currentFailures;
    std::printf("    FAIL %s:%d  %s  实际 %lld  期望 %lld  (0x%08llX vs 0x%08llX)\n",
                file, line, expr, actual, expected,
                static_cast<unsigned long long>(actual) & 0xFFFFFFFFull,
                static_cast<unsigned long long>(expected) & 0xFFFFFFFFull);
}

int runAll() {
    int failedCases = 0;
    const auto& s = suite();
    std::printf("[run] %d 个用例\n", static_cast<int>(s.size()));
    for (const auto& entry : s) {
        currentFailures = 0;
        entry.second();
        if (currentFailures != 0) {
            ++failedCases;
            std::printf("FAIL %s  (%d 处断言失败)\n", entry.first, currentFailures);
        }
    }
    std::printf("[done] %d 个用例，失败 %d 个\n", static_cast<int>(s.size()), failedCases);
    return failedCases;
}

}  // namespace srtest
