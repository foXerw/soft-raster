#include "version.h"

#include <string>

// CMake 传入的两个宏；未定义时退化到可读的占位值，而不是让字符串缺一段。
#ifndef SR_VERSION
#define SR_VERSION "0.0.0-unspecified"
#endif
#ifndef SR_BUILD_CONFIG
#define SR_BUILD_CONFIG "ConfigUnspecified"
#endif

namespace sr {
namespace {

const char* compilerName() {
#if defined(_MSC_VER)
    return "MSVC";
#elif defined(__clang__)
    return "clang";
#elif defined(__GNUC__)
    return "GCC";
#else
    return "unknown-compiler";
#endif
}

}  // namespace

const char* buildInfoString() {
    // 静态局部：首次调用时拼一次，之后返回同一个 c_str()。
    // 这里用 std::string 是刻意的——它不在热路径上，而把三段字面量在编译期拼成
    // 一个定长数组需要 constexpr 字符串操作，代价不值。
    static const std::string s = std::string("soft-raster ") + SR_VERSION + " " +
                                 SR_BUILD_CONFIG + " " + compilerName();
    return s.c_str();
}

}  // namespace sr
