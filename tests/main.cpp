#include "harness.h"

// 返回失败用例数：CTest 按退出码判定，非 0 即失败。
int main() {
    return srtest::runAll();
}
