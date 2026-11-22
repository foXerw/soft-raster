#pragma once
// 纹理的数据侧：ImageRGBA8（行主序 RGBA8 字节）、Texture（mip 链）、LOD 选择，
// 以及程序生成的测试贴图。**采样**在 sampler.h（它只 include 这里）。
#include <cstdint>
#include <vector>

#include "math/vec.h"

namespace sr {

struct ImageRGBA8 {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;   // 行主序，4 字节/像素，top-down
};

// 纹理单边上限（D13）。加载器必须在**分配之前**拿文件头问一次尺寸：
// image_stb 用头探测（只读头部、不解码），TGA 侧对应 tgaPayloadFits 的长度下界。
// 一个几十字节的头不该换走一次巨额分配 —— 8192x8192x4 = 256 MiB，而 M5 要载入
// 用户从网上下载的图片。错误消息里必须带上这个值，用户才知道该把图缩到多大。
inline constexpr int kMaxTextureSide = 4096;

// 纹理：mipmap 链 + 采样参数。级别 0 是原图。
struct Texture {
    int width = 0;
    int height = 0;
    std::vector<ImageRGBA8> levels;

    // true = 字节是 **sRGB 编码的颜色数据**（PNG/JPG/TGA 存的都是这个），采样时解码到线性空间。
    // false = 字节是**数据**（uv 调试图、M6 之后的法线贴图），解码会把 0.5 变成 0.73，
    // 于是"uv 视图"不再是 uv 视图。这就是 GPU 区分 SRGB8_ALPHA8 与 RGBA8 的理由。
    bool srgb = true;

    int levelCount() const { return static_cast<int>(levels.size()); }
    const uint8_t* texel(int level, int x, int y) const;
};

// ---------------------------------------------------------------------------
// 程序生成的测试贴图（spec §7.4：「程序生成的 checker/gradient/uv 调试图，永远可用，零 IO」）。
// 它同时是 golden 场景与 app 里那个立方体的贴图来源。
//
// 为什么不是"一张干净的棋盘"：棋盘格在**转置下恒等**、在 uv 翻转下只是明暗对调，
// 于是一张纯棋盘当夹具时，"u/v 接反"这类错误永远不会让任何断言变红
// （4.5 的方形足迹、4.3 的等差值、4.2 的整数乘积都是同一族坑）。
// 三个角上各放一个不同的标记色、第四角留素棋盘 ⇒ 正方形的**任何**对称都不保持这张图，
// 于是贴图朝向从"肉眼看像是对的"变成"三个断言可判"。副作用是它自解释：
// 看画面就能立刻说出哪个角是 uv(0,0)、u 与 v 各朝哪边。
// ---------------------------------------------------------------------------

// 棋盘贴图的调色板（RGBA8 字节）。颜色提成结构体而不是写死在函数里：
// 端到端测试要断言"这个像素是 uv(0,0) 那个角"，它需要的是**名字**而不是数字。
struct CheckerPalette {
    uint8_t dark[4] = {22, 24, 30, 255};        // ((cx+cy)&1)==0 的格
    uint8_t light[4] = {226, 228, 234, 255};    // ((cx+cy)&1)==1 的格
    uint8_t origin[4] = {226, 40, 40, 255};     // cell(0,0)            = uv(0,0)
    uint8_t uAxis[4] = {58, 190, 60, 255};      // cell(cells-1,0)      = uv(1,0)
    uint8_t vAxis[4] = {58, 110, 226, 255};     // cell(0,cells-1)      = uv(0,1)
};

// cells x cells 的棋盘，每格 cellPx x cellPx 像素（图是 cells*cellPx 见方）。
// 图像行主序、top-down ⇒ v 的正方向是"往下"，与 ColorTarget 的约定一致。
// 角标记与素格抢同一格时（cells==1 的极端情形）优先级是 origin > uAxis > vAxis > 素棋盘。
// cells <= 0 或 cellPx <= 0 ⇒ 返回空图（width 0），与 downsampleBox2 的兜底一致。
ImageRGBA8 makeCheckerImage(int cells, int cellPx, const CheckerPalette& palette);

// 盒式滤波生成下一级：2x2 平均，尺寸向下取整但**每一维都不低于 1**
// （2x1 的下一级是 1x1，沿只有的那一行复制采样；1x0 不是一级纹理，它会把链的尾巴吃掉）。
// 为什么必须是"下一级 = 上一级的 2x2 平均"而不是一级直接从原图算：
// 后者在奇数尺寸或长宽不等时会与硬件的 mip 链不一致，症状是 mipmap 越高层越糊得不均匀。
ImageRGBA8 downsampleBox2(const ImageRGBA8& src);

// 生成完整链，直到**两边都**变成 1（最后一级恒为 1x1）。
std::vector<ImageRGBA8> generateMipChain(const ImageRGBA8& base);

// LOD 选择的结果：level 是要采的那一级（已夹进 [0, levelCount-1]），
// frac 是朝**下一级**混合的比重 ∈ [0,1)。
// 为什么 frac 与 level 必须一起夹：三线性要读 levels[level] 与 levels[level+1]，
// 若只夹 level 不夹 frac，末级仍会带着 frac=0.7 去读那并不存在的下一级。
// 所以不变式是：**frac > 0 ⇒ level+1 < levelCount**（测试 lod_clamps_so_the_next_level_always_exists）。
struct Lod {
    int level = 0;
    float frac = 0.0f;
};

// 由屏幕导数选 mip 层级（spec §7.4：D3D11 那套 rho = max(rho_x, rho_y)，level = floor(log2 rho)）。
// 导数的单位是**归一化 UV / 像素**，也就是 TriangleSetup::dudx/dvdx/dudy/dvdy 直接给的东西
// （Mesh 的 uv 是 j/slices，取值 [0,1)）。换算成"纹素/像素"要乘纹理尺寸，这件事在这里做
// 而不是在调用方做：尺寸和 levelCount 都在 Texture 手里，交给调用方就等于安排它哪天传错。
// levelCount <= 0（空纹理）时返回 0；NaN / 无穷大的导数一律落到安全的一端。
// **lodFromDerivatives 是唯一的 log2 实现**，取整版只是它的一行包装，两份实现不会漂移。
Lod lodFromDerivatives(float texelsU, float texelsV, int levelCount, float dUdx, float dVdx,
                       float dUdy, float dVdy);

int mipLevelFromDerivatives(float texelsU, float texelsV, int levelCount, float dUdx, float dVdx,
                            float dUdy, float dVdy);

}  // namespace sr
