#include "app/win32_window.h"

#include <cstdio>
#include <shellapi.h>   // DragAcceptFiles / DragQueryFileW / DragFinish（WIN32_LEAN_AND_MEAN 不带它）
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

#include "app/path.h"   // utf8FromWide：拖放路径 UTF-16 → UTF-8（D18）

namespace app {

const wchar_t* Window::kClassName = L"SoftRasterWindowClass";

namespace {

bool gClassRegistered = false;

// 从 GWLP_USERDATA 取回 this。WM_NCCREATE 之前它是 0，见 onMessage 的处理顺序。
Window* selfOf(HWND hwnd) {
    return reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

}  // namespace

Window::~Window() { destroy(); }

bool Window::create(int width, int height, const std::wstring& title, std::string& err) {
    err.clear();
    if (m_hwnd != nullptr) {
        err = "Window::create: 已经创建过了";
        return false;
    }
    if (width <= 0 || height <= 0) {
        err = "Window::create: 尺寸必须是正数";
        return false;
    }

    if (!gClassRegistered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &Window::onMessage;
        wc.hInstance = GetModuleHandleW(nullptr);
        // IDC_ARROW 本身已经是 MAKEINTRESOURCE(...) 的形式（伪装成 LPSTR 的序号），
        // 所以这里只做指针类型重新解释，不能再套一层 MAKEINTRESOURCEW。
        wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        // hbrBackground 留空：每一帧我们都写满整个客户区，让系统擦背景只会带来闪烁。
        wc.lpszClassName = kClassName;
        if (RegisterClassW(&wc) == 0) {
            err = "Window::create: RegisterClassW 失败，错误码 " + std::to_string(GetLastError());
            return false;
        }
        gClassRegistered = true;
    }

    // 客户区尺寸才等于 --width/--height，所以用 AdjustWindowRect 把标题栏与边框算进去。
    // 没有 WS_THICKFRAME/WS_MAXIMIZEBOX ⇒ 不可缩放、不可最大化（设计文档 §7.6）。
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rect{0, 0, width, height};
    AdjustWindowRect(&rect, style, FALSE);

    HWND hwnd = CreateWindowExW(0, kClassName, title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                                GetModuleHandleW(nullptr), this);
    if (hwnd == nullptr) {
        err = "Window::create: CreateWindowExW 失败，错误码 " + std::to_string(GetLastError());
        return false;
    }

    m_hwnd = hwnd;
    m_width = width;
    m_height = height;
    m_windowDc = GetDC(hwnd);
    m_memDc = CreateCompatibleDC(m_windowDc);
    if (m_memDc == nullptr) {
        err = "Window::create: CreateCompatibleDC 失败，错误码 " + std::to_string(GetLastError());
        destroy();
        return false;
    }

    // 拖放 .obj 载入（M5.6）：接受 shell 的 WM_DROPFILES。后缀守卫不在这里 ——
    // 这里只管"shell 给了个路径字符串"，是不是 .obj 由 SceneState::load 判（A4）。
    DragAcceptFiles(hwnd, TRUE);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return true;
}

void Window::destroy() {
    if (m_memDc != nullptr) {
        if (m_selectedPrevious != nullptr) {
            SelectObject(m_memDc, m_selectedPrevious);
            m_selectedPrevious = nullptr;
        }
        DeleteDC(m_memDc);
        m_memDc = nullptr;
    }
    if (m_hwnd != nullptr && m_windowDc != nullptr) {
        ReleaseDC(m_hwnd, m_windowDc);
    }
    m_windowDc = nullptr;
    if (m_hwnd != nullptr) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    m_inputSink = nullptr;
}

HBITMAP Window::selectIntoMemory(HBITMAP bmp) {
    if (m_memDc == nullptr) return nullptr;
    HBITMAP previous = static_cast<HBITMAP>(SelectObject(m_memDc, bmp));
    if (m_selectedPrevious == nullptr) m_selectedPrevious = previous;
    return previous;
}

bool Window::blitMemoryToWindow(int width, int height, std::string& err) {
    if (m_windowDc == nullptr || m_memDc == nullptr) {
        err = "blitMemoryToWindow: DC 还没建好";
        return false;
    }
    const BOOL ok = BitBlt(m_windowDc, 0, 0, width, height, m_memDc, 0, 0, SRCCOPY);
    if (ok == 0) {
        err = "blitMemoryToWindow: BitBlt 失败，错误码 " + std::to_string(GetLastError());
        return false;
    }
    return true;
}

void Window::deselectFromMemory() {
    if (m_memDc != nullptr && m_selectedPrevious != nullptr) {
        SelectObject(m_memDc, m_selectedPrevious);
        m_selectedPrevious = nullptr;
    }
}

void Window::requestRepaint() {
    if (m_hwnd != nullptr) InvalidateRect(m_hwnd, nullptr, FALSE);
}

LRESULT CALLBACK Window::onMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    Window* self = selfOf(hwnd);
    if (self != nullptr) return self->handle(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Window::handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // pumpInput 之外到达的消息没有归属帧，输入类事件直接丢弃（见头文件里的说明）。
    InputState* in = m_inputSink;

    switch (msg) {
        case WM_CLOSE:
            m_shouldClose = true;
            DestroyWindow(hwnd);   // 让 WM_DESTROY 去做收尾，句柄失效由系统负责
            return 0;
        case WM_DESTROY:
            m_shouldClose = true;
            PostQuitMessage(0);
            return 0;
        case WM_LBUTTONDOWN:
            m_hasCursor = true;
            m_lastCursor = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            SetCapture(hwnd);
            m_buttons.setLeft(true);
            if (in != nullptr) in->leftPressed = true;
            return 0;
        case WM_LBUTTONUP:
            ReleaseCapture();
            m_buttons.setLeft(false);
            if (in != nullptr) in->leftPressed = false;
            return 0;
        case WM_RBUTTONDOWN:
            m_hasCursor = true;
            m_lastCursor = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            m_buttons.setRight(true);
            if (in != nullptr) in->rightPressed = true;
            return 0;
        case WM_RBUTTONUP:
            m_buttons.setRight(false);
            if (in != nullptr) in->rightPressed = false;
            return 0;
        case WM_MOUSEMOVE: {
            const POINT now{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (in != nullptr) {
                // 位移在一帧内**累计**：一次拖动可能送来多条 WM_MOUSEMOVE，
                // 只取最后一条的话，高帧率下同样的拖动会看起来"越来越慢"。
                if (m_hasCursor) {
                    in->dx += static_cast<float>(now.x - m_lastCursor.x);
                    in->dy += static_cast<float>(now.y - m_lastCursor.y);
                }
                in->cursorX = now.x;
                in->cursorY = now.y;
            }
            m_lastCursor = now;
            m_hasCursor = true;
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (in != nullptr) {
                in->wheel += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) /
                             static_cast<float>(WHEEL_DELTA);
            }
            return 0;
        case WM_DROPFILES: {
            // 拖放（M5.6）：只取第一个文件（模型槽只有一个，多文件拖放取首个）。
            // 路径是 UTF-16，转成 UTF-8 写进本帧快照；main 再经 nativePath 走与
            // --obj 完全相同的 SceneState::load（D18：转换点唯一）。非 .obj 由
            // load 的后缀守卫写 errorLine，不静默忽略（A4）。
            HDROP drop = reinterpret_cast<HDROP>(wp);
            const UINT len = DragQueryFileW(drop, 0, nullptr, 0);   // 0 缓冲 ⇒ 返回所需长度
            if (len > 0 && in != nullptr) {
                std::wstring wide(static_cast<size_t>(len) + 1, L'\0');   // +1 给结尾 null
                // 查返回值（评审 Minor）：0 = 查询失败；> len = 被截断（缓冲按 len 分配，
                // 不该发生，防御）。两种都不把半截路径当模型路径喂下去。
                const UINT copied = DragQueryFileW(drop, 0, wide.data(), len + 1);
                if (copied > 0 && copied <= len) {
                    wide.resize(copied);
                    // 事件标记与路径分开（终审 M3）：转换失败时 droppedObj 为空，但事件
                    // 发生过 —— main 凭 dropEvent 仍走 load，空路径拿到自己的错误消息，
                    // 不静默忽略（A4）。
                    in->dropEvent = true;
                    in->droppedObj = utf8FromWide(wide);
                }
            }
            DragFinish(drop);   // 无论有没有归属帧都要释放 shell 的内存
            return 0;
        }
        case WM_KEYDOWN:
            switch (wp) {
                case VK_ESCAPE: m_shouldClose = true; break;
                default: break;
            }
            if (in != nullptr) {
                switch (wp) {
                    case 'P': in->togglePause = true; break;
                    case 'O': in->stepFrame = true; break;
                    case 'S': in->screenshot = true; break;
                    case 'V': in->cycleView = true; break;
                    case 'W': in->toggleWireframe = true; break;
                    case 'R': in->reloadModel = true; break;
                    case 'F': in->refit = true; break;
                    // 数字这一排：虚拟键码 VK_1 / VK_2 的字面值就是 ASCII '1' / '2'（0x31 / 0x32），
                    // 所以与上面的字母同族。（本机 SDK 的 winnt.h 在 WINVER < WIN10 时**不定义**
                    // VK_0..VK_9 —— 写 VK_1 会得到 C2065，症状像"这个键不存在"，其实只是宏没开。）
                    case '1': in->toggleMip = true; break;
                    case '2': in->cycleFilter = true; break;
                    default: break;
                }
            }
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

bool Window::pumpInput(InputState& out) {
    out = InputState{};   // 位移与"本帧发生过什么"每帧重置
    m_buttons.seed(out);  // 按键的持续状态必须跨帧：漏掉这一行的症状就是"拖动一卡一卡"
    m_inputSink = &out;

    MSG m{};
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) {
            m_inputSink = nullptr;
            m_shouldClose = true;
            return false;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    m_inputSink = nullptr;
    return !m_shouldClose;
}

}  // namespace app
