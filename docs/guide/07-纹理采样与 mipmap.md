# 07 — 纹理采样与 mipmap

第 05 章给了你透视正确的 uv 插值，第 06 章给了你遮挡与 golden 回归。但画面还是纯色：这一章把"uv → 颜色"接通。结束时你**自己写出的东西**是
采样器本身——纹素中心、wrap、mip 链、LOD 与三线性混合，外加四个让 M4 变贵的陷阱：**纹素中心差半格、夹具对称让测试永远不红、LOD 末端带朝黑色混、sRGB 解/编码各多做一次**。**贴棋盘的旋转立方体不是本章片段的产物**：本章两个片段都只在离屏缓冲里验采样数学，那个立方体要到"真 app"那节（`:372-376`）用 `build/Debug/soft-raster.exe` 看，别把终态记到片段头上。
所有片段本机实测：MSVC cl /W4 与 MSYS2 g++（`-ffp-contract=off`），零警告、两侧输出逐字相同。

## 纹素中心：u=0 是边缘，不是中心

约定只有一句：**u=0 落在第 0 个纹素的左边缘**，所以纹素 i 的中心在 `(i+0.5)/W`，索引 = `floor(u*W)`，边界在 `k/W`
（`src/render/sampler.h:49-52` 把它写死，`tests/unit/test_sampler.cpp:52-68` 钉住）。判别方法不是"取中心看看对不对"——错一半的写法在中心点上给出同样的答案——
而是**在边界两侧各取一点**：`u=0.24` 与 `u=0.26`（W=4）必须落在相邻的两个不同纹素上。另一种常见写法 `round(u*W)` 把边界挪到纹素中心，
整张贴图偏移半个纹素：症状不是崩溃，是"贴图好像没对齐"，而你会先怀疑 uv 插值。双线性因此要把坐标平移回"中心格点"：
`centered = u*W - 0.5`，`floor(centered)` 是左/下 tap，小数部分是朝右/下的比重（`src/render/sampler.cpp:75-96、127-128`）。
nearest 与 bilinear 共用 `wrapCoord`，bilinear 的 `lowerTap` 与 `texelIndex` 同规则（fmod/clamp/floor）：uv→下标写两遍，就会有一处悄悄漂半格。

## 最小片段 1：nearest/bilinear，以及 off-by-half 的对照

8×8 手工贴图，四个象限四个值（30/220/45/235）——任何轴对调或翻转都会改数（M4 notes §1 的夹具纪律）。`shift=0.5` 是仓库约定，`shift=0` 复刻"忘了 -0.5"；
两张 128×128 的 BMP 是 16 倍放大的双线性结果。片段把字节当**数据**纹理采样（等价 `srgb=false`，不做 gamma 解码）；解码在片段 2 与下文算清。

```cpp
// snippet1.cpp — 纹素中心 + nearest/bilinear + off-by-half 对照 + 两张放大 BMP
// g++ -std=c++20 -Wall -Wextra -ffp-contract=off snippet1.cpp -o snippet1
// cl /nologo /EHsc /std:c++20 /W4 /D_CRT_SECURE_NO_WARNINGS snippet1.cpp /Fe:snippet1.exe
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
enum class Wrap { Repeat, Clamp };
static int wrapCoord(int i, int n, Wrap w) {
    if (w == Wrap::Clamp) return std::clamp(i, 0, n - 1);
    const int m = i % n;                    // -1 % 8 == -1，负数必须掰正
    return m < 0 ? m + n : m;
}
static int texelIndex(float u, int n, Wrap w) {
    float s = u * float(n);
    if (w == Wrap::Repeat) s = std::fmod(s, float(n));
    else s = std::clamp(s, -1.0f, float(n));
    return wrapCoord(int(std::floor(s)), n, w);   // floor，不是截断
}
struct Img { int w, h; std::vector<uint8_t> px; };
static float at(const Img& t, int x, int y) { return t.px[size_t(y) * t.w + x]; }
// 双线性。shift=0.5 是仓库约定（u*W-0.5）；shift=0 复刻"忘了 -0.5"的错写法。
static float bilinear(const Img& t, float u, float v, Wrap w, float shift) {
    auto tap = [](float c, int n, Wrap wr, float& f) {
        float s = c;
        if (wr == Wrap::Repeat) { s = std::fmod(s, float(n)); if (s < 0.0f) s += float(n); }
        else s = std::clamp(s, -1.0f, float(n));
        const float lo = std::floor(s); f = s - lo; return int(lo);
    };
    float fu = 0, fv = 0;
    const int x0 = tap(u * float(t.w) - shift, t.w, w, fu);
    const int y0 = tap(v * float(t.h) - shift, t.h, w, fv);
    const int xa = wrapCoord(x0, t.w, w), xb = wrapCoord(x0 + 1, t.w, w);
    const int ya = wrapCoord(y0, t.h, w), yb = wrapCoord(y0 + 1, t.h, w);
    return (1-fu)*(1-fv)*at(t,xa,ya) + fu*(1-fv)*at(t,xb,ya)
         + (1-fu)*fv*at(t,xa,yb) + fu*fv*at(t,xb,yb);
}
static void bmp(const char* path, const std::vector<uint8_t>& g, int W, int H) {
    const int stride = (W * 3 + 3) & ~3; uint8_t h[54] = {};
    auto p32 = [&](int o, uint32_t v) { for (int i = 0; i < 4; ++i) h[o+i] = uint8_t(v >> (8*i)); };
    h[0]='B'; h[1]='M'; p32(2, uint32_t(54 + stride*H)); p32(10, 54); p32(14, 40);
    p32(18, uint32_t(W)); p32(22, uint32_t(H)); h[26]=1; h[28]=24; p32(34, uint32_t(stride*H));
    FILE* f = std::fopen(path, "wb"); std::fwrite(h, 1, 54, f);
    std::vector<uint8_t> row(size_t(stride), 0);
    for (int y = H-1; y >= 0; --y) {        // 文件 bottom-up，内存 top-down
        for (int x = 0; x < W; ++x)
            row[size_t(x)*3] = row[size_t(x)*3+1] = row[size_t(x)*3+2] = g[size_t(y)*W+x];
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
}
int main() {
    Img t{8, 8, std::vector<uint8_t>(64)};  // 四象限四个值：任何轴对调/翻转都会改数
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x)
        t.px[size_t(y)*8+x] = uint8_t((x < 4 ? 30 : 220) + (y < 4 ? 0 : 15));
    for (const float u : {0.0625f, 0.24f, 0.25f})
        std::printf("u=%.4f floor=%d round=%d\n", double(u),
                    int(std::floor(u*8)), int(std::floor(u*8+0.5f)));
    const float pts[][2] = {{0.0625f,0.0625f},{0.5f,0.5f},{0.4375f,0.5f},{-0.1f,0.5f},{1.25f,0.5f}};
    for (const auto& p : pts) {
        const int nx = texelIndex(p[0],8,Wrap::Repeat), ny = texelIndex(p[1],8,Wrap::Repeat);
        std::printf("(%.4f,%.4f) nearest=%3.0f bilinear=%7.2f noShift=%7.2f\n",
                    double(p[0]), double(p[1]), at(t,nx,ny),
                    double(bilinear(t,p[0],p[1],Wrap::Repeat,0.5f)),
                    double(bilinear(t,p[0],p[1],Wrap::Repeat,0.0f)));
    }
    for (const float u : {-0.1f, 1.25f})
        std::printf("Clamp u=%.2f nearest=%3.0f bilinear=%7.2f\n", double(u),
                    at(t, texelIndex(u,8,Wrap::Clamp), 4),
                    double(bilinear(t,u,0.5f,Wrap::Clamp,0.5f)));
    const int W = 128;                      // 16x 放大：正确约定 vs 忘 -0.5
    std::vector<uint8_t> ok(size_t(W)*W), bad(size_t(W)*W);
    for (int y = 0; y < W; ++y) for (int x = 0; x < W; ++x) {
        const float u = (float(x)+0.5f)/float(W), v = (float(y)+0.5f)/float(W);
        ok[size_t(y)*W+x] = uint8_t(std::lround(bilinear(t,u,v,Wrap::Clamp,0.5f)));
        bad[size_t(y)*W+x] = uint8_t(std::lround(bilinear(t,u,v,Wrap::Clamp,0.0f)));
    }
    bmp("sampler_ok.bmp", ok, W, W); bmp("sampler_half.bmp", bad, W, W);
    int eOk=-1, eBad=-1;                    // 行 y=8 上半高（>=125）边缘的 x 位置
    for (int x = 0; x < W; ++x) {
        if (eOk<0 && ok[size_t(8)*W+x]>=125) eOk=x;
        if (eBad<0 && bad[size_t(8)*W+x]>=125) eBad=x;
    }
    std::printf("half-height edge: ok x=%d noShift x=%d shift=%d px\n", eOk, eBad, eOk-eBad);
}
```

编译运行（两条工具链实测，输出一致）：`g++ -std=c++20 -Wall -Wextra -ffp-contract=off snippet1.cpp -o snippet1 && ./snippet1`；
cl 侧用文件头第三行的命令（vcvars64 环境里）。实际打印：

```text
u=0.0625 floor=0 round=1
u=0.2400 floor=1 round=2
u=0.2500 floor=2 round=2
(0.0625,0.0625) nearest= 30 bilinear=  30.00 noShift=  30.00
(0.5000,0.5000) nearest=235 bilinear= 132.50 noShift= 235.00
(0.4375,0.5000) nearest= 45 bilinear=  37.50 noShift= 140.00
(-0.1000,0.5000) nearest=235 bilinear= 227.50 noShift= 197.00
(1.2500,0.5000) nearest= 45 bilinear=  37.50 noShift=  45.00
Clamp u=-0.10 nearest= 45 bilinear=  37.50
Clamp u=1.25 nearest=235 bilinear= 227.50
half-height edge: ok x=64 noShift x=56 shift=8 px
```

读法：`u=0.0625` 是第 0 个纹素的**中心**，`round` 写法却给 1——off-by-half 连中心点都错，不只是边界。`(0.5,0.5)` 在正确约定下是四个象限的交点，
双线性给四值平均 132.5；忘 -0.5 的写法把它当成 texel(4,4) 的中心，给 235。负坐标与 >1 坐标：Repeat 下 `-0.1` 绕到最后一列（235 侧）、`1.25` 绕回第 2 列；
Clamp 下分别钉在第 0 列与最后一列——**同一坐标两种 wrap 必须给不同答案**，否则"Clamp 其实没生效"会被掩盖（`tests/unit/test_sampler.cpp:88-106` 专门断言这个差）。
两张 BMP 各 49206 字节，打开看：`sampler_ok.bmp` 的十字边界在画面正中，`sampler_half.bmp` 整体左移/上移 8 像素 = 半纹素 × 16 倍放大（打印的 `shift=8 px` 是它的测量）。

## 最小片段 2：mip 链、屏幕导数选 LOD、三线性

mip 链的规则（`src/render/texture.cpp:16-57`，Ruling 24）：下一级 = 上一级的 2×2 盒式平均；**int 累加**（四个 8bit 最大 1020，不溢出；
浮点平均会引入 0.5 舍入歧义，"平均保持性"测试对舍入方式敏感，:32-34）；`(sum+2)/4` 四舍五入而不是截断（截断让每级系统性偏暗，
"mip 越高越暗"会看起来像设计，:48-52）；奇数尺寸向下取整，但**每维下限 1**（2×1 的下一级是 1×1，夹取 = 复制边缘纹素、分母仍是 4，:24-25、:37-41）。
注意链平均的是**字节**：对 sRGB 图这是编码空间的平均，一个近似；线性空间的正确性在采样混合那一步保证（见下）。

```cpp
// snippet2.cpp — mip 链（int 累加盒式滤波）+ 屏幕导数选 LOD + 三线性 + sRGB 数值锚
// g++ -std=c++20 -Wall -Wextra -ffp-contract=off snippet2.cpp -o snippet2
// cl /nologo /EHsc /std:c++20 /W4 /D_CRT_SECURE_NO_WARNINGS snippet2.cpp /Fe:snippet2.exe
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
struct Img { int w = 0, h = 0; std::vector<uint8_t> px; };   // 灰度当 RGBA 的 r 通道看
static uint8_t at(const Img& t, int x, int y) { return t.px[size_t(y) * t.w + x]; }
// 与仓库 downsampleBox2 同规则：int 累加、(sum+2)/4 四舍五入、边缘复制、每维下限 1
static Img downsampleBox2(const Img& src) {
    Img out; out.w = std::max(1, src.w / 2); out.h = std::max(1, src.h / 2);
    out.px.assign(size_t(out.w) * out.h, 0);
    for (int y = 0; y < out.h; ++y) for (int x = 0; x < out.w; ++x) {
        int sum = 0;                        // 四个 8bit 相加最大 1020，int 不溢出
        for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx)
            sum += at(src, std::min(x*2+dx, src.w-1), std::min(y*2+dy, src.h-1));
        out.px[size_t(y) * out.w + x] = uint8_t((sum + 2) / 4);
    }
    return out;
}
static void printLevel(const Img& t, int level) {
    double sum = 0.0;
    std::printf("level %d (%dx%d):", level, t.w, t.h);
    for (int y = 0; y < t.h; ++y) {
        std::printf("\n  ");
        for (int x = 0; x < t.w; ++x) { sum += at(t,x,y); std::printf("%3d ", at(t,x,y)); }
    }
    std::printf("\n  mean=%.2f\n", sum / (double(t.w) * t.h));
}
// 某一级上的双线性（Repeat）：索引用**该级自己的**宽高，不是 level 0 的
static float bilinearLevel(const std::vector<Img>& chain, int level, float u, float v) {
    const Img& t = chain[size_t(level)];
    auto tap = [](float c, int n, float& f) {
        float s = std::fmod(c, float(n)); if (s < 0.0f) s += float(n);
        const float lo = std::floor(s); f = s - lo; return int(lo);
    };
    float fu = 0, fv = 0;
    const int x0 = tap(u * float(t.w) - 0.5f, t.w, fu);
    const int y0 = tap(v * float(t.h) - 0.5f, t.h, fv);
    const int xa = x0 % t.w, xb = (x0+1) % t.w, ya = y0 % t.h, yb = (y0+1) % t.h;
    return (1-fu)*(1-fv)*float(at(t,xa,ya)) + fu*(1-fv)*float(at(t,xb,ya))
         + (1-fu)*fv*float(at(t,xa,yb)) + fu*fv*float(at(t,xb,yb));
}
struct Lod { int level = 0; float frac = 0.0f; };
// 与仓库 lodFromDerivatives 同式：导数单位是 UV/像素，乘纹理尺寸换成纹素/像素
static Lod lodFrom(float W, float H, int n, float dudx, float dvdx, float dudy, float dvdy) {
    Lod out;
    const float ux = std::fabs(dudx)*W, vy = std::fabs(dvdy)*H;
    const float uy = std::fabs(dvdx)*H, vx = std::fabs(dudy)*W;
    const float rho = std::max(std::sqrt(ux*ux+uy*uy), std::sqrt(vx*vx+vy*vy));
    if (!(rho > 1.0f)) return out;          // 放大：level 0、frac 0
    const float l = std::log2(rho);
    const int level = int(std::floor(l));
    if (level >= n - 1) { out.level = n - 1; return out; }   // 夹到末级必须同时 frac=0
    out.level = level; out.frac = l - float(level);
    return out;
}
static float trilinear(const std::vector<Img>& chain, float u, float v, const Lod& lod) {
    const float lo = bilinearLevel(chain, lod.level, u, v);
    if (lod.frac <= 0.0f) return lo;
    const float hi = bilinearLevel(chain, lod.level + 1, u, v);
    return lo + lod.frac * (hi - lo);
}
static float encodeS(float v) {   // IEC 61966-2-1（与 src/render/color.cpp 同曲线）
    return v <= 0.0031308f ? 12.92f*v : 1.055f*std::pow(v, 1.0f/2.4f) - 0.055f;
}
static float decodeS(float s) {
    return s <= 0.04045f ? s/12.92f : std::pow((s+0.055f)/1.055f, 2.4f);
}
int main() {
    Img base{8, 8, std::vector<uint8_t>(64)};   // 左 2 列黑、右 6 列白：各级在采样点上互不相同
    for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x)
        base.px[size_t(y)*8+x] = x < 2 ? 0 : 255;
    std::vector<Img> chain{base};
    while (chain.back().w > 1 || chain.back().h > 1) chain.push_back(downsampleBox2(chain.back()));
    for (size_t l = 0; l < chain.size(); ++l) printLevel(chain[l], int(l));
    std::printf("-- LOD: 8x8 texture, dU/dx = rho/8, others 0 --\n");
    for (const float rho : {0.5f, 2.0f, 2.8284271f, 4.0f, 10.0f}) {
        const Lod l = lodFrom(8, 8, int(chain.size()), rho/8, 0, 0, 0);
        std::printf("rho=%7.4f -> level=%d frac=%.4f\n", double(rho), l.level, double(l.frac));
    }
    for (const int P : {8, 4, 2}) {         // uv 铺满 quad、quad 占 PxP 像素 => dU/dx = 1/P
        const Lod l = lodFrom(8, 8, 4, 1.0f/float(P), 0, 0, 1.0f/float(P));
        std::printf("quad %dx%d px -> level=%d frac=%.2f\n", P, P, l.level, double(l.frac));
    }
    const float u = 0.125f, v = 0.5f;
    std::printf("-- trilinear at u=0.125 v=0.5 (Repeat) --\n");
    for (int l = 0; l < 4; ++l)
        std::printf("bilinear level %d = %.2f\n", l, double(bilinearLevel(chain, l, u, v)));
    const Lod half = lodFrom(8, 8, 4, 2.8284271f/8, 0, 0, 0);
    std::printf("rho=2*sqrt2: lo=%.2f hi=%.2f blend=%.3f\n",
                double(bilinearLevel(chain, half.level, u, v)),
                double(bilinearLevel(chain, half.level+1, u, v)),
                double(trilinear(chain, u, v, half)));
    const float last = bilinearLevel(chain, 3, u, v);
    const float bugFrac = std::log2(10.0f) - 3.0f;   // 错夹到 levelCount 时末级还带的 frac
    std::printf("rho=10: clamped -> %.2f; buggy clamp frac=%.4f -> %.2f (toward black)\n",
                double(last), double(bugFrac), double(last * (1.0f - bugFrac)));
    std::printf("-- sRGB anchors --\n");
    std::printf("encode(0.5)=%.6f -> byte %d\n", double(encodeS(0.5f)),
                int(std::lround(encodeS(0.5f) * 255.0f)));
    std::printf("decode(0.5)=%.4f decode twice=%.4f\n", double(decodeS(0.5f)),
                double(decodeS(decodeS(0.5f))));
    std::printf("encode twice=%.4f -> byte %d\n", double(encodeS(encodeS(0.5f))),
                int(std::lround(encodeS(encodeS(0.5f)) * 255.0f)));
    std::printf("bytes 0/128: linear avg=%.4f  encoded-space avg=%.4f\n",
                double(0.5f * (decodeS(0.0f) + decodeS(128.0f/255.0f))),
                double(0.5f * (0.0f + 128.0f/255.0f)));
}
```

编译命令见文件头的两行命令（g++ / cl，cl 侧在 vcvars64 环境里），与片段 1 同形。实际打印（两工具链一致）：

```text
level 0 (8x8):
    0   0 255 255 255 255 255 255   （×8 行，mean=191.25）
level 1 (4x4):
    0 255 255 255   （×4 行，mean=191.25）
level 2 (2x2):
  128 255   （×2 行，mean=191.50）
level 3 (1x1):
  192   mean=192.00
-- LOD: 8x8 texture, dU/dx = rho/8, others 0 --
rho= 0.5000 -> level=0 frac=0.0000
rho= 2.0000 -> level=1 frac=0.0000
rho= 2.8284 -> level=1 frac=0.5000
rho= 4.0000 -> level=2 frac=0.0000
rho=10.0000 -> level=3 frac=0.0000
quad 8x8 px -> level=0 frac=0.00
quad 4x4 px -> level=1 frac=0.00
quad 2x2 px -> level=2 frac=0.00
-- trilinear at u=0.125 v=0.5 (Repeat) --
bilinear level 0 = 0.00
bilinear level 1 = 0.00
bilinear level 2 = 159.75
bilinear level 3 = 192.00
rho=2*sqrt2: lo=0.00 hi=159.75 blend=79.875
rho=10: clamped -> 192.00; buggy clamp frac=0.3219 -> 130.19 (toward black)
-- sRGB anchors --
encode(0.5)=0.735357 -> byte 188
decode(0.5)=0.2140 decode twice=0.0376
encode twice=0.8732 -> byte 223
bytes 0/128: linear avg=0.1079  encoded-space avg=0.2510
```

（level 0–2 把程序打印的重复行折叠成"×N 行"以便阅读；mean 与逐像素值都是实测原值。）读法三组：**链**——128 是 (0+255)/2 四舍五入、
192 是末级全局平均，均值 191.25→192 在 ±1 内保持（仓库同条测试 `tests/unit/test_texture.cpp:103-119`）。**LOD**——`rho` 是"沿屏幕走一像素在纹素空间跨多远"，
`level=floor(log2 rho)`、`frac` 是朝下一级混合的比重；quad 占 4×4 像素 ⇒ 每像素跨 2 纹素 ⇒ level 1；`rho=2√2` 给 {1, 0.5}，与仓库断言一致
（`tests/unit/test_sampler_mip.cpp:50-53`）。**三线性**——level 1 与 level 2 **各做四个 tap 的双线性**再按 frac 混（0 与 159.75 混出 79.875）；
不是"八个 tap 一起加权"，也不是"先混级再插值"。末行是末端带 bug 的复现：夹取界错写成 `levelCount` 时末级还带 frac=0.3219，去混不存在的下一级（全 0）
⇒ 192 变 130.19，**朝黑色混**；仓库里这个 bug 的症状是"拉远相机物体慢慢变黑"，实测 0.208 而不是 0.502（M4 notes §5，修复 commit `6f625a8`）。sRGB 四行是下文"单一编码点"的数值锚。

## 指回本仓库：生产版本多了什么、为什么多

**wrap 与坏输入。** `texelIndex` 先乘到纹素尺度再收进 int 值域：Repeat 用 `fmod` 摘整数圈、ClampEdge 直接夹（`src/render/sampler.cpp:26-44`）；
`fmod` 只对 Repeat 做——对 ClampEdge 做会把 u=2.0 折回 0，而它该钉在末纹素。floor 而不是截断：`-0.1*4=-0.4` 截断给 0、floor 给 -1
（Repeat 下 = 末纹素），`-0.25*4=-1.0` 两者同值、分不开这两种写法（`tests/unit/test_sampler.cpp:79-85`）。NaN/±inf 给有定义落点 0（Ruling 28）；
`texel()` 交回 nullptr 时静默全 0，但 debug 下有一个越界采样计数把"静默"变可观测（`src/render/texture.h:36-47`）。

**解码在采样时，不在加载时。** `Texture::srgb` 是**输入侧**标志（`src/render/texture.h:31-34`）：字节永远 8bit 原样，`toTexelF` 采样时查 256 项表解码
（`src/render/sampler.cpp:50-73`，表在 `src/render/color.cpp:39-46`）；加载器只设标志 + 建链（`src/loader/model.cpp:197-198`）。三个理由：加载时解码要存
float（内存 ×4，链再 ×4/3）；数据纹理（uv 调试图、法线贴图）不该被解码——0.5 会被解成 0.214，"uv 视图"不再是 uv；alpha 是覆盖率不是颜色，永远不解码（:71）。
**tap 混合严格在线性空间**：四个 tap 各自解码后再加权（Ruling 29，`sampler.cpp:136-147`）；字节 0 与 128 的正确线性平均是 0.1079，编码空间平均是 0.2510
（片段 2 实测），肉眼差就是"中间调发灰"（`tests/unit/test_sampler_bilinear.cpp:99-113`）。nearest/bilinear 共用 `toTexelF`：解码规则写两遍，将来只改一遍
就会"同一张图两种过滤给出不同的暗部"。

**LOD 的导数从哪来。** 公式是 `rhoX=|(∂u/∂x·W, ∂v/∂x·H)|`、`rhoY` 同理、`rho=max(rhoX,rhoY)`（`src/render/texture.cpp:120-124`）——max 取在**两个屏幕方向**上，
每个方向是 uv 两分量的合长度；几何平均会让高压缩方向欠采样，症状是一条方向闪、另一条正常。导数由 Setup 在三角形重心处用商法则从 `uv/w` 与 `1/w` 两个平面求出
（`src/render/triangle_setup.cpp:73-84`），光栅循环填进 `gradU=(∂u/∂x,∂u/∂y)`、`gradV` 同形（`src/render/render_backend.cpp:109-110`、`src/render/pipeline.h:89-95`）；
Vec2 入口的转置只发生在 `sampler.cpp:159-163` 一行。**单位是归一化 UV/像素**，乘纹理尺寸在函数内部做（Ruling 25）：上一版测试按纹素/像素传值，四条断言整齐差
log2(256)=8 级——"差一个固定常数"就是单位错的指纹（`tests/unit/test_texture.cpp:121-147`：256 纹素铺 64 像素 ⇒ level 2；x 跨 16、y 跨 1 ⇒ level 4）。
梯度接反（gradU/gradV 转置）不崩不黑，只让 mip 选错；方形屏幕足迹下两式恒等、永远测不出，所以端到端守卫用 400×100 的宽扁四边形（`tests/unit/test_texture_pipeline.cpp:149-188`）。

**两个正交旋钮与按键。** `filter` 与 `useMip` 是独立字段（`src/render/sampler.h:34-43`，Ruling 31）：`useMip=false + Trilinear` 合法，语义是"永远 level 0 的双线性"；
`Nearest + useMip=true` 是"在被选中级上取单纹素"。统一入口 `sample()` 先定 LOD 再分派（`sampler.cpp:186-199`）。app 里 **`1` 键开关 mip、`2` 键循环
nearest→bilinear→trilinear**（`src/app/win32_window.cpp:246-247`、`src/app/main.cpp:414-421`）；没有 `--mip` 命令行旗标，状态打在终端 `[mip] 开/关` 与 HUD 的
`MIP ON/OFF`（`main.cpp:418、499`）。golden 立方体用 ClampEdge：每面 uv 正好铺满 [0,1]，Repeat 会让右边缘回绕抄到第 0 列的颜色（`src/scene/golden_scene.cpp:35-45`）；
导入模型则用 Repeat（外部 uv 常越界，`main.cpp:196-201`）。

**单一编码点，与 188。** 颜色目标的每次写入恰好在写那一刻编码一次：光栅出口 `encodeSRGB(fo.color)`（`src/render/render_backend.cpp:117`），FS 输出永远是线性
（`builtin_shaders.cpp:48-59`）。于是两种症状可以算出来（片段 2 实测）：解码是压缩，0.5 解一次 0.2140、解两次 0.0376 ⇒ 暗部死黑；编码是膨胀，0.5 编一次得字节 **188**、
编两次 223 ⇒ 整体发白。188 来自 IEC 61966-2-1 分段曲线（`src/render/color.cpp:10-14`）：0.5 高于接缝 0.0031308，走幂段 `1.055·0.5^(1/2.4)−0.055≈0.7354`，
×255=187.516 ⇒ lround=188。这是单一编码点的**正确后果**（法线视图因此是粉彩的），**不是** `srgb=false` 那件事——后者管输入侧解不解码，两件事别混
（`docs/notes/M6-光照与休眠接口.md:195-203`）。编码走 4096 项 LUT 而不是 256 项：256 项等于量化两次，暗部 2–3 LSB 的带状条纹在渐变上可见（`color.cpp:8、29-37`）。

**图像方向：翻转点全清单。** 全栈一条规则：图像第 0 行 = 顶部、采样器 v=0 = 第 0 行、mesh 的 v=0 = 面的顶边
⇒ 贴图与图像同向（`src/scene/mesh.cpp:51-54`；`ImageRGBA8` top-down，`texture.h:14`）。各格式在各自门口翻译一次：

| 位置 | 格式约定 | 翻译 |
|---|---|---|
| BMP 写出 | 行自底向上 | 翻一次，y 递减（`src/loader/bmp_writer.cpp:86-91`） |
| DIB | 正 biHeight 即 bottom-up | 负 biHeight ⇒ 不翻（`src/app/dib.cpp:37`） |
| TGA 读 | header bit5 定 origin | bit5=0 翻行（`src/loader/image_tga.cpp:120-125`） |
| TGA 写 | 故意写 bottom-up | bit5=0，让读侧那条翻转有东西可守（`src/loader/tga_writer.h:9-12`） |
| stb（PNG/JPG） | 总是 top-down | 显式 `stbi_set_flip_vertically_on_load(0)`（`src/loader/image_stb.cpp:80-82`） |
| OBJ `vt` | v=0 是图片下边缘 | 解析器出口一次 `v=1-v`（`src/loader/obj.cpp:54-59`） |

`assets/tga/checker.tga` 的头实测：128×128、32bpp、descriptor=0x08（bit5=0）——夹具**故意**存成 bottom-up，读回必须翻一次行。
**规则：两次翻转可以互相抵消，然后画面看起来完全正确。** obj 忘翻 + tga 忘翻的合成帧与正确帧只差 3 个 ±1 像素（三线性舍入残差），±1 容差门禁完全放过；
真正抓抵消的是两条语义锚（OBJ 解析 vs 程序 cube、TGA 解码 vs 程序 checker），每条翻转各养一条独立的锚（M5 notes §1，证据在那里，这里只给规则）。

**夹具与 golden。** 棋盘贴图三个角各一种标记色、第四角留素棋盘 ⇒ 正方形的任何对称都不保持这张图
（`src/render/texture.h:54-64`；7 条对称逐条断言，`tests/unit/test_texture_gen.cpp:107-141`）——纯棋盘在转置下恒等，
"u/v 接反"永远红不起来。golden 是**第二个入口**而不是改第一个：`renderGoldenSceneTextured` 与 M3 场景共存，
比对逻辑一份、容差 ±1/通道（`tests/golden/golden.cpp:25、117-126`）。`textured_golden_frame_is_not_the_m3_frame`
守"新函数真的接了纹理"：我用现有 build/Debug 产物实跑，打印 `[golden-tex] 与 M3 帧相差 4257 个像素，其中 HUD 区 18 个`，
断言是 diffs>1500 且 HUD 差>0（`tests/unit/test_golden_scene_textured.cpp:355-379`）——4257/18 是那次运行的实测值。
诚实的边界：4×4 格贴图端到端**分不出** Bilinear 与 Trilinear（level 0/1/2 的格心都是纯标记色），所以那条边界测试只断言"混合中间调、分不清"（:281-321），
mip 的守门是 LOD 单测 + 参考图逐字节比对 + 肉眼验收（M4 notes §8）。`map_Kd` 的选项 arity 教训（表写多 ⇒ 多吞一个 token 吃掉文件名 ⇒ 整个文件被拒）属于第 08 章（M5 notes §4）。

## 坑（一手来自 docs/notes/）

| 坑 | 症状 | 权威 / 修法 |
|---|---|---|
| 边界落在纹素中心 | 整张贴图偏半格，不崩 | 边界两侧各取一点断言（M4 §2） |
| 夹具对称（等差值/方形足迹/整数乘积/纯棋盘） | 实现错、测试永远不红 | 换不对称夹具 + 变异验证（M4 §1） |
| LOD 夹取界差一格 | 拉远相机物体慢慢变黑 | 末级 frac 归零；守门测试自己也要补开区间（M4 §5，`6f625a8`） |
| 2×1 的下一级算成 1×0 | 链尾被吃、越界索引带走整个测试进程 | 每维下限 1；测试不许越界索引自己的输出（M4 §6） |
| cube uv 蝴蝶结 + 藏着的 90° | 标记色出现在错误的角 | uv = 角在面局部系里的角色；没被消费过的属性不会自己报错（M4 §7） |
| 导数单位传成纹素/像素 | 断言整齐差 log2(W) 级 | 单位钉死 UV/像素，换算在函数内（M4 §5） |
| 两次翻转抵消 | 画面完全正确的假通过 | 每条翻转一条独立语义锚（M5 §1） |

绕向沿用第 03 章的判据：**只信 `area2` 符号 + 语义 `dot(外法线, eye−顶点)`，不信 "CW/CCW" 这个词**——`178e9db` 与 `a66fdfc` 两批清扫
刚把仓库里说反的注释改完（git show 可核：第一批六处、第二批四处）。贴贴图不改变剔除约定，但会让你第一次**看见** uv 接反：绕向词错与 uv 转置是同族"口算方向"的病。

## 你应该看到 X；如果你看到 Y，说明 Z 错了

片段 1：`floor/round` 三行在中心点就分岔；`(0.5,0.5)` 双线性 132.5 而 noShift 235；`shift=8 px`；两张 BMP 的十字边界一个居中、一个偏 8 像素。
看到两者边界重合 ⇒ 你的 noShift 分支其实也减了 0.5。片段 2：链均值 191.25→192（±1 内）；`rho=2.8284 → {1, 0.5}`；blend=79.875；buggy clamp 130.19 < 192。
看到 blend 等于 lo 或 hi ⇒ 没混级；看到末级带 frac ⇒ 夹取界差一格；看到均值逐级下降 ⇒ 平均写成了截断。sRGB 四行：188 / 0.2140 与 0.0376 / 223 / 0.1079 与 0.2510；
看到 encode(0.5)=128 ⇒ 没编码；=186 ⇒ 用了 gamma-2.2 纯幂。

真 app（`build/Debug/soft-raster.exe`，或你自己编的）：旋转的棋盘立方体，HUD 有 `MIP ON  TRILINEAR`。按 `1`：终端打 `[mip] 关（永远 level 0，会闪）`、
HUD 变 `MIP OFF`；**盯着斜面与远处看**——level 0 在缩小处欠采样，旋转时相邻帧采到不同纹素，棋盘格会闪烁/爬行；再按 `1` 恢复，闪消失。
按 `2` 循环过滤：NEAREST 时格边是硬边、BILINEAR 时格边是混合中间调。"关 mip 会闪"是 spec §8 M4 的验收判据（`main.cpp:416-418` 注释；M4 notes :3 记"关会闪、打开不闪"，产品方肉眼验收的记录在 `test_golden_scene_textured.cpp:291-292`）；
静态证据是 `minified_quad_keeps_level_zero_aliasing`：它在 **mip 开着**（useMip=true）时跑，正确接线 rho=0.64<1 ⇒ level 0 保留锯齿（64 像素扫描里跳变 ≥10 次），gradU/gradV 转置会选到 level 1 均匀灰而红——它守的是梯度接线，不是 mip 开/关对照。我本章验证的是按键通路、状态打印与这条静态断言，
闪烁本身请你在窗口里亲眼看——那是这一章的肉眼验收门。若关 mip 后**不闪**：先确认 HUD 真的变了 OFF，再看的是不是缩小/斜视的面（正对且放大时 level 0 本来就够）；
若贴图整体偏半格或某个角颜色不对，回片段 1 的边界断言与 M4 §7 的 uv 角色化，别先怀疑采样器。
