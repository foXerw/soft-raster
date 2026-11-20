#include "app/surface.h"

#include <vector>

#include "app/dib.h"
#include "app/win32_window.h"

namespace app {
namespace {

class OffscreenSurface final : public Surface {
  public:
    OffscreenSurface(int width, int height) {
        m_width = width;
        m_height = height;
        // 顺序很重要：先把内存分配到位，再让视图指向它。反过来做的话，
        // 之后任何一次 resize 都会把 m_target 里的指针变成悬垂。
        m_color.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u, 0);
        m_depth.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 1.0f);
        m_target.color = sr::ColorTarget{reinterpret_cast<uint32_t*>(m_color.data()), width, height,
                                         width * 4};
        m_target.depth = sr::DepthTarget{m_depth.data(), width, height};
    }

    bool present(std::string&) override { return true; }  // 离屏没有屏幕可上
    bool pumpInput(InputState& out) override {
        out = InputState{};
        return true;
    }
    const char* kind() const override { return "offscreen"; }

  private:
    std::vector<uint8_t> m_color;
    std::vector<float> m_depth;
};

class WindowSurface final : public Surface {
  public:
    bool init(int width, int height, const std::wstring& title, std::string& err) {
        if (!m_window.create(width, height, title, err)) return false;

        HDC dc = m_window.windowDc();
        if (!m_dib.create(dc, width, height, err)) {
            err = "createWindowSurface: " + err;
            return false;
        }
        m_width = width;
        m_height = height;
        m_depth.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 1.0f);
        // DIB 是 top-down（负 biHeight），所以核心库按 C8 写的行序可以直接 BitBlt，
        // 这里不需要任何翻转 —— 这正是 dib.h 里那条决定的兑现时刻。
        m_target.color = sr::ColorTarget{m_dib.pixels(), width, height, m_dib.pitch()};
        m_target.depth = sr::DepthTarget{m_depth.data(), width, height};
        return true;
    }

    bool present(std::string& err) override {
        if (!m_window.blitMemoryToWindow(m_width, m_height, err)) return false;
        m_window.requestRepaint();
        return true;
    }

    bool pumpInput(InputState& out) override { return m_window.pumpInput(out); }
    bool shouldClose() const override { return m_window.shouldClose(); }
    const char* kind() const override { return "window"; }

  private:
    Window m_window;
    Dib m_dib;
    // 深度不放 DIB 里：DIB 只是颜色。它必须由 Surface 拥有，否则 m_target.depth
    // 会指向一块没人管的地盘。
    std::vector<float> m_depth;
};

}  // namespace

std::unique_ptr<Surface> createOffscreenSurface(int width, int height) {
    return std::unique_ptr<Surface>(new OffscreenSurface(width, height));
}

std::unique_ptr<Surface> createWindowSurface(int width, int height, const std::wstring& title,
                                             std::string& err) {
    auto s = std::unique_ptr<WindowSurface>(new WindowSurface());
    if (!s->init(width, height, title, err)) return nullptr;
    return std::unique_ptr<Surface>(s.release());
}

}  // namespace app
