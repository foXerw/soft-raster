#include <filesystem>
#include <string>
#ifdef _MSC_VER
#include <crtdbg.h>
#include <csignal>
#include <cstdlib>
#include <exception>
#endif

#include "harness.h"

// 返回失败信号（0 = 全绿，1 = 有失败）：CTest 按退出码判定，非 0 即失败。
// `--only <子串>` 把范围缩到单条用例用（M4.1 定位崩溃时加的）。
int main(int argc, char** argv) {
#ifdef _MSC_VER
    // 关掉 Debug CRT 的模态报告框（M5.3 追加 b）。实测过的故障链：测试里未捕获异常
    // → terminate → abort() → **非交互会话里的消息框永久阻塞** → ctest 只报 Timeout，
    // 与"缺 DLL 进程起不来"同族：故障被伪装成另一种故障（M5.3 报告 §5.1）。
    // 为什么是关对话框而不是加 try/catch：未捕获异常/越界在这里是**缺陷信号**，
    // 不该被接住继续跑（接住会把红变成绿或半绿）；我们要的只是让它**可见且快速失败** ——
    // 报告改道 stderr、abort 不弹框不呼 Watson，进程当场非零退出。
    // 都是 CRT（<crtdbg.h>/<cstdlib>），不是 Win32；tests 目标本就不受 C4 约束。
    // 逐类型设置：_CrtSetReportMode/_CrtSetReportFile 的 reportType 不接受按位或
    // （实测 OR 进去整条调用无效，对话框照弹）。
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    // 关框之后 abort/terminate 默认是**静默**退出（实测 rc=3 但 stderr 一个字没有），
    // 缺陷信号仍然不可见。补两个 handler 把它打到 stderr：不是接住异常继续跑
    // （那会把红变绿），而是打印后立刻 _exit(3)。terminate 里 rethrow 只为取出
    // what() 展示，展示完即死，不做任何恢复。
    std::set_terminate([] {
        std::fprintf(stderr, "[harness] terminate: 未捕获异常");
        try {
            if (std::current_exception()) std::rethrow_exception(std::current_exception());
        } catch (const std::exception& e) {
            std::fprintf(stderr, ": %s", e.what());
        } catch (...) {}
        std::fprintf(stderr, "（缺陷信号，立即失败）\n");
        ::_exit(3);   // MSVC 的 _exit 在全局命名空间，不在 std
    });
    std::signal(SIGABRT, [](int) {
        std::fprintf(stderr, "[harness] SIGABRT: abort()（缺陷信号，立即失败）\n");
        ::_exit(3);   // MSVC 的 _exit 在全局命名空间，不在 std
    });
#endif
    // BMP 写出这类测试必须落盘一次（"写完能读回来"没法纯内存断言）。
    // 目录在这里统一建好，测试文件里就不需要每个用例各自 mkdir。
    std::error_code ec;
    std::filesystem::create_directories("sr_tmp_bmp", ec);

    srtest::Options opts;
    std::string err;
    if (!srtest::parseArgs(argc, argv, opts, err)) {
        std::fprintf(stderr, "参数错误: %s\n", err.c_str());
        return 2;
    }
    // POSIX 把退出码截断到 0–255："退出码 = 失败用例数"意味着正好 256 条失败时 rc 是 0
    // —— 一条会让后面所有任务的红/绿判读整体失效的陷阱（M4 终审 deferred #8）。夹到 1。
    const int failed = srtest::runAll(opts);
    return failed > 1 ? 1 : failed;
}
