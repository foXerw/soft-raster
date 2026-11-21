#pragma once
// 极简自注册测试 harness（设计文档 C2/C3：不引入第三方测试框架；§9 的 builder 决定）。
//
// 用法：
//   SR_TEST(some_behaviour) {
//       SR_ASSERT(2 + 2 == 4);
//       SR_ASSERT_NEAR(compute(), 1.5f, 1e-5f);
//       SR_ASSERT_EQ(pixelCount(), 16);
//   }
// 用例通过静态初始化自动登记，tests/main.cpp 里 runAll() 依次调用。
//
// 为什么所有数值断言都是容差而不是"精确等于"：见 C1′——g++ 与 MSVC 的标准库
// sin/cos/pow/log2 不保证逐位一致，浮点相等比较会在换编译器时假性失败。

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace srtest {

using Fn = void (*)();

std::vector<std::pair<const char*, Fn>>& suite();

struct Register {
    explicit Register(const char* name, Fn fn);
};

// 当前用例内的失败次数；runAll() 在每个用例开始前清零。
extern int currentFailures;

// 按用例名挑选用例。filter 为空（nullptr 或 ""）时放行全部。
// 为什么是**子串**而不是全名相等：调试崩溃时手里往往只有半个名字（"mip_chain"），
// 要求敲全名会让过滤器本身变成新的错误来源 —— 敲错一个字母 = 一条都没跑 = 看起来全绿。
bool nameMatchesFilter(const char* name, const char* filter);

struct Options {
    const char* filter = nullptr;   // 指向 argv，进程活多久它活多久
};

// 解析命令行。目前只有 `--only <子串>`。
// 陌生参数与缺值的 --only 都必须报错返回 false —— 静默忽略的代价是
// "以为在跑过滤、其实跑的是全量（或 0 条）"，这类假通过本项目已经踩过几次。
bool parseArgs(int argc, char** argv, Options& out, std::string& err);

int runAll(const Options& opts = {});

void reportFailure(const char* file, int line, const char* expr);
void reportFailureNear(const char* file, int line, const char* expr,
                       double actual, double expected, double tol);
void reportFailureEq(const char* file, int line, const char* expr,
                     long long actual, long long expected);

// 断言里的比较必须经过这三个函数。原因很实际：`SR_ASSERT(1 + 1 == 2)` 这种常量断言
// 在 MSVC /W4 下触发 C4127（"条件表达式是常量"），而构建输出必须保持干净——
// 一旦允许噪声，真正的新警告就看不见了。
inline bool condIsFalse(bool v) { return !v; }
inline bool intsDiffer(long long a, long long b) { return a != b; }
inline bool nearDiffers(double a, double b, double tol) { return !(std::fabs(a - b) <= tol); }

}  // namespace srtest

#define SR_TEST(name)                                                    \
    static void name();                                                  \
    static const ::srtest::Register srtest_reg_##name(#name, &name);     \
    static void name()

#define SR_ASSERT(cond)                                                              \
    do {                                                                             \
        if (::srtest::condIsFalse(static_cast<bool>(cond)))                          \
            ::srtest::reportFailure(__FILE__, __LINE__, #cond);                      \
    } while (0)

#define SR_ASSERT_FALSE(cond) SR_ASSERT(!(cond))

// 容差比较。tol 用 fabs(a-b) <= tol 的形式，边界是**包含**的（有测试专门 pin 这一点）。
#define SR_ASSERT_NEAR(actual, expected, tol)                                        \
    do {                                                                             \
        const double sr_a_ = static_cast<double>(actual);                            \
        const double sr_b_ = static_cast<double>(expected);                          \
        const double sr_t_ = static_cast<double>(tol);                               \
        if (::srtest::nearDiffers(sr_a_, sr_b_, sr_t_))                              \
            ::srtest::reportFailureNear(__FILE__, __LINE__,                           \
                                        #actual " ~= " #expected, sr_a_, sr_b_, sr_t_); \
    } while (0)

// 整型/枚举相等。失败时打印两个值——像素计数、字节值这类断言，光看表达式没用的。
#define SR_ASSERT_EQ(actual, expected)                                               \
    do {                                                                             \
        const long long sr_a_ = static_cast<long long>(actual);                      \
        const long long sr_b_ = static_cast<long long>(expected);                    \
        if (::srtest::intsDiffer(sr_a_, sr_b_))                                       \
            ::srtest::reportFailureEq(__FILE__, __LINE__,                             \
                                      #actual " == " #expected, sr_a_, sr_b_);        \
    } while (0)
