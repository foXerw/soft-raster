#pragma once

#include <chrono>
#include <cstdint>

namespace sr {

// 一个可重复使用的秒表，用于 FPS、`--bench` 与分阶段耗时（FrameStats 里的 ms* 字段）。
//
// 设计文档 §7.6 提到 QueryPerformanceCounter；这里用 std::chrono::steady_clock 是因为
// MSVC 上它的底层实现就是 QPC，而 steady_clock 不需要 <windows.h>，于是计时器能留在
// 核心库里（C4）。将来若需要更高精度，唯一要改的文件就是这个头。
class Timer {
  public:
    using Clock = std::chrono::steady_clock;

    Timer() : m_start(Clock::now()) {}

    void restart() { m_start = Clock::now(); }

    // 距上次 restart 的毫秒数。double 而不是 float：一帧是 16.67ms，
    // 而分阶段计时会累加到几十 ms，float 的 7 位有效数字在"帧时间求平均"时会看到漂移。
    double elapsedMs() const {
        const auto d = Clock::now() - m_start;
        return std::chrono::duration<double, std::milli>(d).count();
    }

    // 两次查询之间的间隔；一帧只有一个 dt，用它避免下面那个坑。
    double lapMs() {
        const auto now = Clock::now();
        const double ms = std::chrono::duration<double, std::milli>(now - m_start).count();
        m_start = now;
        return ms;
    }

  private:
    Clock::time_point m_start;
};

}  // namespace sr
