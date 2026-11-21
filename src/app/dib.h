#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX   // 否则 windows.h 的 min/max 宏会毁掉 std::min/std::max
#endif
#include <windows.h>

#include <string>

namespace app {

// 一块 GDI 设备无关位图（DIB），32bpp，**top-down**。
//
// 这是整个项目里唯一与"屏幕内存布局"打交道的地方。用负 biHeight 拿到 top-down 的
// DIB，是为了让翻转点归一（设计文档 C8）：核心库按 top-down 写像素，窗口这边一行都不用翻，
// 唯一另一次转换在 bmp_writer 里（那是 BMP 格式自己的要求）。
//
// 对比另一种写法（正 biHeight + 写 DIB 时手动翻行）：那种做法多一个翻转点，
// 而多一个翻转点意味着多一处"上下颠倒"的 bug 可能，且它与 golden 的颠倒看起来一模一样。
class Dib {
  public:
    Dib() = default;
    ~Dib();

    Dib(const Dib&) = delete;
    Dib& operator=(const Dib&) = delete;

    bool create(HDC hdc, int width, int height, std::string& err);
    void destroy();

    uint32_t* pixels() const { return reinterpret_cast<uint32_t*>(m_bits); }
    HBITMAP bitmap() const { return m_bitmap; }
    int width() const { return m_width; }
    int height() const { return m_height; }
    int pitch() const { return m_width * 4; }

  private:
    HBITMAP m_bitmap = nullptr;
    void* m_bits = nullptr;
    int m_width = 0;
    int m_height = 0;
};

}  // namespace app
