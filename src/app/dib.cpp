#include "app/dib.h"

#include <cstring>

namespace app {
namespace {

// 32bpp BI_RGB 没有调色板，所以只需要 BITMAPINFOHEADER。
// 直接声明 BITMAPINFO 会带 1024 项 RGBQUAD（16KB 栈上数组，而且 MSVC 会警告），
// 这里按需要的最小尺寸铺开：header + 一点点余量。
struct DibInfo {
    BITMAPINFOHEADER header;
    RGBQUAD spare[3];
};

}  // namespace

Dib::~Dib() { destroy(); }

bool Dib::create(HDC hdc, int width, int height, std::string& err) {
    destroy();
    err.clear();

    if (hdc == nullptr) {
        err = "Dib::create: 传入的 HDC 为空（窗口尚未创建？）";
        return false;
    }
    if (width <= 0 || height <= 0) {
        err = "Dib::create: 尺寸必须是正数";
        return false;
    }

    DibInfo info;
    std::memset(&info, 0, sizeof(info));
    info.header.biSize = sizeof(BITMAPINFOHEADER);
    info.header.biWidth = width;
    info.header.biHeight = -height;   // **负值 ⇒ top-down**：第 0 行是上边缘（C8）
    info.header.biPlanes = 1;
    info.header.biBitCount = 32;
    info.header.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&info), DIB_RGB_COLORS, &bits,
                                   nullptr, 0);
    if (bmp == nullptr || bits == nullptr) {
        err = "Dib::create: CreateDIBSection 失败，GDI 错误码 " + std::to_string(GetLastError()) +
              "（尺寸 " + std::to_string(width) + "x" + std::to_string(height) + "）";
        return false;
    }

    m_bitmap = bmp;
    m_bits = bits;
    m_width = width;
    m_height = height;
    return true;
}

void Dib::destroy() {
    if (m_bitmap != nullptr) {
        DeleteObject(m_bitmap);
        m_bitmap = nullptr;
    }
    // m_bits 由 GDI 拥有，随 DIB section 一起释放，绝不能自己 delete。
    m_bits = nullptr;
    m_width = 0;
    m_height = 0;
}

}  // namespace app
