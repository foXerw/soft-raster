#include <filesystem>
#include <string>

#include "harness.h"

// 返回失败用例数：CTest 按退出码判定，非 0 即失败。
// `--only <子串>` 把范围缩到单条用例用（M4.1 定位崩溃时加的）。
int main(int argc, char** argv) {
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
    return srtest::runAll(opts);
}
