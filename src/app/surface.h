#pragma once

#include <memory>
#include <string>

#include "app/input.h"
#include "render/render_target.h"

namespace app {

// 像素的出口。两种实现：窗口（DIB + BitBlt）与离屏（堆内存）。
//
// 为什么要这个抽象：golden 回归与 `--bench` 必须能在**没有窗口**的情况下渲染整帧
// （设计文档 C1′ 与 §7.6），而渲染代码不该有两条路径。于是渲染只认识 RenderTarget，
// 显示方式由 present() 决定。
//
// 生命周期约定：Surface 拥有像素内存，target() 返回的视图指向它自己的成员，
// 因此 Surface **不可拷贝也不可移动**（第一版 BMP 测试就是因为按值返回了含内部裸指针的
// 结构体，指针当场悬垂，症状是测试进程挂住）。工厂函数一律返回 unique_ptr。
class Surface {
  public:
    virtual ~Surface() = default;
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;

    sr::RenderTarget& target() { return m_target; }
    int width() const { return m_width; }
    int height() const { return m_height; }

    virtual bool present(std::string& err) = 0;
    virtual bool pumpInput(InputState& out) {
        out = InputState{};
        return true;
    }
    virtual bool shouldClose() const { return false; }
    virtual const char* kind() const = 0;

  protected:
    Surface() = default;
    sr::RenderTarget m_target{};
    int m_width = 0;
    int m_height = 0;
};

std::unique_ptr<Surface> createOffscreenSurface(int width, int height);

// title 用 wstring：窗口标题里有中文是常见需求，而 argv 是 UTF-8，转换只发生在这一个入口。
std::unique_ptr<Surface> createWindowSurface(int width, int height, const std::wstring& title,
                                             std::string& err);

}  // namespace app
