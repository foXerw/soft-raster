#include <filesystem>

#include "harness.h"

// 返回失败用例数：CTest 按退出码判定，非 0 即失败。
int main() {
    // BMP 写出这类测试必须落盘一次（"写完能读回来"没法纯内存断言）。
    // 目录在这里统一建好，测试文件里就不需要每个用例各自 mkdir。
    std::error_code ec;
    std::filesystem::create_directories("sr_tmp_bmp", ec);
    return srtest::runAll();
}
