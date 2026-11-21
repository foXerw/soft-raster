#pragma once

#include <string>

#include "app/dib.h"
#include "app/input.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace app {

// 一个固定尺寸的 Win32 窗口 + 消息泵 + 用于 BitBlt 的兼容内存 DC。
//
// 这一层刻意保持薄：它只做"把像素送上屏幕"和"把消息变成 InputState"，
// 不认识渲染管线的任何概念。设计文档 §7.6 决定窗口不可拖拽缩放（尺寸只由
// --width/--height 决定），所以这里没有 WM_SIZE 的重分配逻辑 —— tile 网格与
// 深度缓冲的容量在启动时就定了，这是 M8 并行化能保持简单的前提。
class Window {
  public:
    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool create(int width, int height, const std::wstring& title, std::string& err);
    void destroy();

    // 取空消息队列。返回 false 表示用户要求关闭（WM_QUIT 已到）。
    bool pumpInput(InputState& out);

    bool shouldClose() const { return m_shouldClose; }

    HDC windowDc() const { return m_windowDc; }
    HDC memoryDc() const { return m_memDc; }
    HBITMAP selectIntoMemory(HBITMAP bmp);  // 返回之前的位图，调用方负责恢复
    HWND hwnd() const { return m_hwnd; }

    // 把当前选在 memoryDc 里的位图 blit 到窗口。返回 BitBlt 的成败。
    bool blitMemoryToWindow(int width, int height, std::string& err);
    void requestRepaint();
    // 把当前选进 memoryDc 的位图换回原来的（必须在 DIB 被 DeleteObject 之前做，
    // 否则 DeleteObject 对一个仍被选中的位图会失败 —— 复审 I5 的泄漏）。
    void deselectFromMemory();

    int width() const { return m_width; }
    int height() const { return m_height; }

    static const wchar_t* kClassName;

  private:
    static LRESULT CALLBACK onMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    // 消息里带来的 hwnd 必须一路用下去：WM_NCCREATE 时成员 m_hwnd 还没被赋值
    // （它要等 CreateWindowExW 返回才写），用 m_hwnd 调 DefWindowProc 就是拿 NULL 当句柄，
    // 于是 NCCREATE 失败、CreateWindowExW 报 ERROR_INVALID_WINDOW_HANDLE(1400)。
    LRESULT handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    HWND m_hwnd = nullptr;
    HDC m_windowDc = nullptr;
    HDC m_memDc = nullptr;
    HBITMAP m_selectedPrevious = nullptr;
    bool m_shouldClose = false;
    int m_width = 0;
    int m_height = 0;

    // 鼠标状态：位移由"上一次位置"算出来，所以必须是成员而不是每帧局部变量。
    POINT m_lastCursor{0, 0};
    bool m_hasCursor = false;

    // 只在 pumpInput 期间非空：WndProc 是同步调用的，所以消息处理可以直接写进调用方
    // 这一帧的 InputState。用成员指针而不是 static 变量，是为了不给未来的多窗口
    // 留一份全局可变态。pumpInput 之外到达的消息（系统自己发的重绘等）没有归属帧，
    // 直接丢弃 —— 让它们写进"下一帧"会让一次拖动的残留在下一次 pump 里冒出来。
    InputState* m_inputSink = nullptr;
};

}  // namespace app
