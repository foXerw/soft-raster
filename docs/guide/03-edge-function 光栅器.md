# 03 — edge-function 光栅器

## 这一章要解决什么问题

第 01 章给了你一块能写的像素缓冲（`uint32_t*` + 宽高 + pitch），第 02 章给了 `Vec`/`Mat`。
但还缺最核心的一步：**给定屏幕上三个顶点，哪些像素在三角形里面？** 这一章结束时，你的程序
要把一个纯色三角形画进缓冲、存成 BMP 看得见，并且理解四件看起来平凡、实则各自踩坑的事：
边函数、半空间符号、共享边的 fill rule、y 朝下视口里的绕向剔除。

整台光栅器的判定核心只有三个符号测试。下面先把数学推清楚，再给能跑的片段。

## 边函数：覆盖判定就是三个符号

有向边 `e = b - a`。定义 `E(p) = cross(e, p - a)`，展开就是
`e.x*(p.y-a.y) - e.y*(p.x-a.x)`，写成点积即 `n·(p-a)`，其中 **`n = {-e.y, e.x}`**。
二维叉积返回标量（`src/math/vec.h:43`），它的**符号**就是"点在有向边的哪一侧"。
代码把 `n·(p-a)` 拆成 `n·p + c`（`c = -n·a`），于是 `E(a)=E(b)=0`，边上恒零
（`src/render/plane_eq.h:47-57` 的 `EdgeFunc::fromEdge` / `evaluate`）。

为什么 `n={-e.y,e.x}` 指向内侧？用代码里自带的验证例（`plane_eq.h:32-33`）：
三角形 `(0,0)-(10,0)-(0,10)`，`cross((10,0),(0,10))=100>0`；边 `(0,0)→(10,0)` 的
`n={0,10}`，对内部点 `(2,2)` 求值得 `20>0`。**内侧为正**成立——但只对"面积叉积为正"
的那种顶点顺序成立。顺序反过来，内部全变负。所以 `buildTriangleGeometryChecked` 先算
`area2 = cross(v1-v0, v2-v0)`，取 `sign = area2>0 ? 1 : -1`，把绕向**吸收进法线**
（`src/render/triangle_geom.h:41-50`）。这样无论顶点怎么排，内侧统一为正，覆盖判定就是：

```
edges[0].covers(p) && edges[1].covers(p) && edges[2].covers(p)
```

（`triangle_geom.h:87-89`）。**不改顶点顺序**来吸收绕向：改顺序会让"取第一个顶点的 varying"
这种 flat 取值悄悄换掉一个顶点（`triangle_geom.h:45-46`）。代价：每像素每边 2 乘 2 加，×3。

## 最小片段 1：一个三角形进 BMP

下面这个文件独立编译。它含边函数、top-left 边界规则、像素包围盒、一个模板逐像素回调，
以及一个前向差分版覆盖计数用来和直接求值互相校验。BMP 写盘是最小 24 位实现（行序翻成 bottom-up）。

```cpp
// 片段 1：独立 edge-function 光栅器。纯色三角形 → uint32_t 缓冲 → BMP，打印覆盖数。
// g++ -std=c++20 -Wall -Wextra -ffp-contract=off snippet1.cpp -o snippet1
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
struct Vec2 { float x, y; };
static float cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
struct Edge {                                   // E(p) = n·p + c，内侧为正
    Vec2 n; float c;
    static Edge from(Vec2 a, Vec2 b, float s) {
        Vec2 e{b.x - a.x, b.y - a.y};
        Edge f{{-e.y * s, e.x * s}, 0.f};       // n = {-e.y, e.x} * sign
        f.c = -(f.n.x * a.x + f.n.y * a.y);     // 使 E(a) == E(b) == 0
        return f;
    }
    float eval(Vec2 p) const { return n.x * p.x + n.y * p.y + c; }
    bool tl() const { return n.x > 0.f || (n.x == 0.f && n.y > 0.f); }   // top-left
    bool covers(Vec2 p) const { float v = eval(p); return v > 0.f || (v == 0.f && tl()); }
};
struct Tri { Edge e[3]; int x0, y0, x1, y1; };
static bool build(const Vec2 v[3], int W, int H, Tri& t) {
    float a2 = cross({v[1].x - v[0].x, v[1].y - v[0].y}, {v[2].x - v[0].x, v[2].y - v[0].y});
    if (!(std::fabs(a2) > 1e-6f)) return false;                 // 退化
    float s = a2 > 0.f ? 1.f : -1.f;                            // 绕向用 sign 吸收
    for (int i = 0; i < 3; ++i) t.e[i] = Edge::from(v[i], v[(i + 1) % 3], s);
    float mnx = std::min({v[0].x, v[1].x, v[2].x}), mxx = std::max({v[0].x, v[1].x, v[2].x});
    float mny = std::min({v[0].y, v[1].y, v[2].y}), mxy = std::max({v[0].y, v[1].y, v[2].y});
    t.x0 = std::max(0, (int)std::floor(mnx - .5f)); t.x1 = std::min(W - 1, (int)std::ceil(mxx - .5f));
    t.y0 = std::max(0, (int)std::floor(mny - .5f)); t.y1 = std::min(H - 1, (int)std::ceil(mxy - .5f));
    return t.x0 <= t.x1 && t.y0 <= t.y1;                        // 夹到目标内
}
template <class Fn> static int forEachCovered(const Tri& t, Fn paint) {
    int cov = 0;
    for (int y = t.y0; y <= t.y1; ++y) for (int x = t.x0; x <= t.x1; ++x) {
        Vec2 p{x + .5f, y + .5f};                               // 像素中心
        if (t.e[0].covers(p) && t.e[1].covers(p) && t.e[2].covers(p)) { ++cov; paint(x, y); }
    }
    return cov;
}
static int coveredInc(const Tri& t) {           // 前向差分：行首求值一次，行内 E += n.x
    int cov = 0;
    for (int y = t.y0; y <= t.y1; ++y) {
        Vec2 st{t.x0 + .5f, y + .5f};
        float b[3] = {t.e[0].eval(st), t.e[1].eval(st), t.e[2].eval(st)};
        for (int x = t.x0; x <= t.x1; ++x) {
            bool in = true;
            for (int i = 0; i < 3; ++i) {
                if (!(b[i] > 0.f || (b[i] == 0.f && t.e[i].tl()))) in = false;
                b[i] += t.e[i].n.x;
            }
            cov += in;
        }
    }
    return cov;
}
static void bmp(const char* path, const std::vector<uint32_t>& px, int W, int H) {
    int rb = W * 3, pad = (4 - rb % 4) % 4; uint32_t ds = (uint32_t)((rb + pad) * H);
    uint8_t h[54] = {};
    auto p32 = [&](int o, uint32_t v) { for (int i = 0; i < 4; ++i) h[o + i] = (uint8_t)(v >> (8 * i)); };
    h[0] = 'B'; h[1] = 'M'; p32(2, 54 + ds); p32(10, 54); p32(14, 40);
    p32(18, (uint32_t)W); p32(22, (uint32_t)H); h[26] = 1; h[28] = 24; p32(34, ds); p32(38, 2835); p32(42, 2835); // DPI 同 bmp_writer.cpp
    FILE* f = std::fopen(path, "wb"); std::fwrite(h, 1, 54, f);
    std::vector<uint8_t> row(rb + pad, 0);
    for (int y = H - 1; y >= 0; --y) {          // 文件 bottom-up，内存 top-down，写时翻行
        for (int x = 0; x < W; ++x) {
            uint32_t c = px[(size_t)y * W + x];
            row[x * 3] = (uint8_t)c; row[x * 3 + 1] = (uint8_t)(c >> 8); row[x * 3 + 2] = (uint8_t)(c >> 16);
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
}
int main() {
    const Vec2 pick[3] = {{.5f, .5f}, {8.5f, .5f}, {.5f, 8.5f}};   // 与仓库 Pick 定理测试同夹具
    Tri t{}; build(pick, 16, 16, t);
    std::printf("pick16: covered=%d incremental=%d\n", forEachCovered(t, [](int, int) {}), coveredInc(t));
    const int W = 256, H = 256;
    std::vector<uint32_t> px((size_t)W * H, 0xFF000000u);
    const Vec2 v[3] = {{30.5f, 220.5f}, {225.5f, 190.5f}, {110.5f, 25.5f}};
    build(v, W, H, t);
    int cov = forEachCovered(t, [&](int x, int y) { px[(size_t)y * W + x] = 0xFFE03030u; });
    bmp("tri.bmp", px, W, H);
    std::printf("tri256: covered=%d incremental=%d bmp=tri.bmp\n", cov, coveredInc(t));
}
```

编译与运行（两条工具链都实测过，输出一致）：

```
g++ -std=c++20 -Wall -Wextra -ffp-contract=off snippet1.cpp -o snippet1 && ./snippet1
cl /nologo /EHsc /std:c++20 /W4 /D_CRT_SECURE_NO_WARNINGS snippet1.cpp /Fe:snippet1.exe && snippet1.exe
```

实际打印：

```
pick16: covered=36 incremental=36
tri256: covered=17805 incremental=17805 bmp=tri.bmp
```

`tri.bmp` 是一个黑底红色三角形。我用一段独立读回代码数了红色像素：**17805**，与覆盖数吻合；
按行剖面看，底部窄、中部宽、顶部收尖，形状正确。三个要点：

- **顶点在像素中心**（`1.5` 是像素 `(1,1)` 的中心）。顶点像素本身**不保证**被覆盖——它同时落在
  两条边上，fill rule 只让顶边/左边的边界像素归属本三角形（仓库测试 `test_raster_scalar.cpp:46-48`
  专门点了这件事；我的 BMP 顶部顶点所在行红色数为 0，正是此理）。
- **包围盒 + scissor**：从顶点 min/max 算，`floor(min-0.5)..ceil(max-0.5)` 是像素中心的保守超集，
  再 `clampedTo` 夹到目标内（`triangle_geom.h:52-67`、`rect.h:17-25`），扫描量从整屏 `W×H` 降到盒大小。
  fill rule 决定边界像素**归谁**，目标边界决定**写不写**：`writePacked` 越界静默丢弃，`contains` 用
  无符号化一次比较挡住负与越界（`render_target.cpp:12-17、29-41`）。部分在屏外的三角形 `covered >= written`
  （`test_raster_scalar.cpp:215-232`）；flat 路径盒已夹好，二者相等。
- **前向差分**：`E` 仿射 ⇒ `E(p+(1,0)) = E(p) + n.x`，行内只加常数（`coveredInc`），两夹具上与直接
  求值计数相等（36、17805）。这正是 M9 要向量化的东西：§7.3（200-204 行）把 SIMD 范围**明确**为覆盖判定、
  plane-equation 插值的前向差分、深度比较掩码、颜色写入，**不含** shading 本体。代价：float 逐步累加随行宽
  漂移，`==0` 边界判定可能翻面；M9 若改定点 edge function，坐标必须有界（六面裁剪保证，M2 notes §1）。
  **当前 Backend A 仍每像素直接求值**（`triangle_geom.h:86-89`），`src` 里没有 `b0/b1/b2`——那是划给 M9 的优化。

## 指回本仓库：生产版本多了什么

### 绕向与背面剔除（y 朝下）

本项目的屏幕 **y 朝下**（C8，设计文档 96-98 行；`render_target.h:11-12`）。视口映射把 NDC 的
+y（上）翻成第 0 行（上边缘）：`pixel_y = (1 - (ndcY*0.5+0.5)) * height`（`viewport.cpp:12-14`）。
这一翻**只翻有向面积的符号，不翻肉眼旋向**——正面三角形肉眼看来仍是逆时针（178e9db 实测更正了
仓库里"正面是 CW"的旧错词）。权威是测试里的四步链（`test_triangle_setup.cpp:53-60`，取符号结论）：

1. Mesh 生成器约定：从物体外面看是逆时针（右手系，世界空间；`src/scene/mesh.cpp:11`）。
2. `view*proj` 保持手性进入 NDC（y 朝上）：正面三角形 shoelace 叉积为**正**。
3. Viewport 把 y 翻成 top-down ⇒ 叉积**符号**翻转（肉眼旋向不变）。
4. 所以**屏幕空间里 `area2 < 0` 的是正面**。判据只信 `area2` 符号 + 语义 `dot`，不信 "CW/CCW" 这个词。

剔除据此写：`backFacing = frontFaceIsClockwise ? (area2>0) : (area2<0)`
（`triangle_setup.cpp:46-53`；`CullState` 默认 `frontFaceIsClockwise=true`，`pipeline.h:107-111`）。
**连仓库注释都错过这个词**——178e9db 一次改了 6 处渲染注释，而这条测试第 4 步注释（:59-60）至今仍写 "clockwise"，只有它的**断言** `s.area2 < 0`（:69）是权威。所以只信符号和语义判据，别口算顺逆。
写反的症状：所有正面三角形 `covered=0`，一个像素都不画，但剔除/裁剪计数全正常，看起来像"相机没对上"
（M1 notes §1、M2 notes §3c）；或者立方体整个内外翻。

"恰好剔掉 6 个"是怎么钉住的：`setup_culls_exactly_the_three_far_faces_of_a_cube`
（`test_triangle_setup.cpp:110-165`）在一般姿态下断言 `frontFaceIsClockwise=true` 通过 6 个、`=false` 通过 6 个，
**且两个索引集合互不相交**。6 不是自然律，是几何：那个姿态下 6 个面里 3 个朝向相机（每面 2 三角形）。
它的权威来源是与符号无关的语义判据 `dot(外法线, eye-顶点) > 0`
（`culling_accepts_exactly_the_faces_whose_normals_face_the_camera`，81-108 行，一般姿态实测 6）。
**只断言"数量相等"会放过"剔错了面"的实现**——不相交那条才是约定断言。集成测试再钉
12 提交 / 6 剔除 / 6 光栅化，换姿态不变，关剔除则 12 全进（`test_pipeline_integration.cpp:85-107`）。
（这些是仓库测试源码里的断言值，我读过、未在本章重跑；本章"实测"数字仅指上面两个片段的输出。）

### fill rule 与共享边

"内部"必须定义成：两个三角形共享的像素**恰好被覆盖一次**。否则要么缝（谁都不画，露背景），
要么双写（半透明混合时接缝处颜色翻倍）。标准答案是 **top-left 规则**：像素中心恰好落在边上时，
只有**顶边**和**左边**把它算进来。本仓库的判据直接用内侧法线（`plane_eq.h:59-74`）：
左边 ⟺ `n.x>0`；顶边 ⟺ `n.x==0 && n.y>0`。`covers`：`>0` 一定进，`==0` 只有顶/左边才进。

为什么不用"第三顶点在哪侧"来表达？旧版正是那么写的（`opposite.x > p0.x || opposite.x > p1.x`），
**那不是侧向判定**：陡边（`|dx|<|dy|`）上会同时产生缝隙与双写（复审 C3，commit `cd7c6b0`）。
两条 ±45° 对角线测试恰好落在 `opposite.x == min` 的边界上，看不见它。片段 2 把这个坑复现给你看：

```cpp
// 片段 2：共享边不变式。覆盖数之和 == 并集像素数 ⇒ 不重不漏。并演示被推翻的旧判据。
// g++ -std=c++20 -Wall -Wextra -ffp-contract=off snippet2.cpp -o snippet2
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
struct Vec2 { float x, y; };
static float cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
struct Edge {
    Vec2 a, b, n; float c;
    static Edge from(Vec2 p0, Vec2 p1, float s) {
        Vec2 e{p1.x - p0.x, p1.y - p0.y};
        Edge f{p0, p1, {-e.y * s, e.x * s}, 0.f};
        f.c = -(f.n.x * p0.x + f.n.y * p0.y);
        return f;
    }
    float eval(Vec2 p) const { return n.x * p.x + n.y * p.y + c; }
    bool tl() const { return n.x > 0.f || (n.x == 0.f && n.y > 0.f); }        // 现行：内侧法线
    bool tlOld(Vec2 opp) const {                                              // 旧：第三顶点 x 比较
        if (a.y == b.y) return opp.y > a.y;
        return opp.x > a.x || opp.x > b.x;
    }
};
struct Tri { Edge e[3]; int x0, y0, x1, y1; };
static bool build(const Vec2 v[3], int W, int H, Tri& t) {
    float a2 = cross({v[1].x - v[0].x, v[1].y - v[0].y}, {v[2].x - v[0].x, v[2].y - v[0].y});
    if (!(std::fabs(a2) > 1e-6f)) return false;
    float s = a2 > 0.f ? 1.f : -1.f;
    for (int i = 0; i < 3; ++i) t.e[i] = Edge::from(v[i], v[(i + 1) % 3], s);
    float mnx = std::min({v[0].x, v[1].x, v[2].x}), mxx = std::max({v[0].x, v[1].x, v[2].x});
    float mny = std::min({v[0].y, v[1].y, v[2].y}), mxy = std::max({v[0].y, v[1].y, v[2].y});
    t.x0 = std::max(0, (int)std::floor(mnx - .5f)); t.x1 = std::min(W - 1, (int)std::ceil(mxx - .5f));
    t.y0 = std::max(0, (int)std::floor(mny - .5f)); t.y1 = std::min(H - 1, (int)std::ceil(mxy - .5f));
    return t.x0 <= t.x1 && t.y0 <= t.y1;
}
struct Cover { int covered = 0; std::vector<int> hits; };
static Cover cover(const Vec2 v[3], int W, int H, bool old) {
    Cover r; r.hits.assign((size_t)W * H, 0);
    Tri t{}; if (!build(v, W, H, t)) return r;
    for (int y = t.y0; y <= t.y1; ++y) for (int x = t.x0; x <= t.x1; ++x) {
        Vec2 p{x + .5f, y + .5f}; bool in = true;
        for (int i = 0; i < 3; ++i) {
            float val = t.e[i].eval(p);
            bool ok = val > 0.f || (val == 0.f && (old ? t.e[i].tlOld(v[(i + 2) % 3]) : t.e[i].tl()));
            if (!ok) { in = false; break; }
        }
        if (in) { ++r.covered; ++r.hits[(size_t)y * W + x]; }
    }
    return r;
}
static void pair(const char* name, const Vec2 A[3], const Vec2 B[3], int W, int H, bool old) {
    Cover ra = cover(A, W, H, old), rb = cover(B, W, H, old);
    int uni = 0, dbl = 0;
    for (size_t i = 0; i < ra.hits.size(); ++i) {
        int h = ra.hits[i] + rb.hits[i];
        if (h > 0) ++uni;
        if (h > 1) ++dbl;
    }
    std::printf("%-10s coveredA=%2d coveredB=%2d sum=%2d union=%2d double=%d\n",
                name, ra.covered, rb.covered, ra.covered + rb.covered, uni, dbl);
}
int main() {
    const Vec2 d1a[3] = {{.5f, .5f}, {4.5f, .5f}, {4.5f, 4.5f}};   // 4x4 正方形，对角线劈分
    const Vec2 d1b[3] = {{.5f, .5f}, {4.5f, 4.5f}, {.5f, 4.5f}};
    const Vec2 d2a[3] = {{.5f, .5f}, {4.5f, .5f}, {.5f, 4.5f}};    // 另一条对角线
    const Vec2 d2b[3] = {{4.5f, .5f}, {4.5f, 4.5f}, {.5f, 4.5f}};
    pair("diag1-new", d1a, d1b, 4, 4, false);
    pair("diag2-new", d2a, d2b, 4, 4, false);
    pair("diag1-old", d1a, d1b, 4, 4, true);     // 旧规则在 ±45° 上恰好看不出错了
    const Vec2 sl[3] = {{.5f, .5f}, {1.5f, 10.5f}, {.7f, 5.5f}};   // 陡边 |dx|<|dy|，左为薄片
    const Vec2 sr[3] = {{1.5f, 10.5f}, {.5f, .5f}, {2.5f, 5.5f}};
    pair("steep-new", sl, sr, 8, 12, false);
    pair("steep-old", sl, sr, 8, 12, true);      // 共享端点像素被两边认领 ⇒ double=1
    const Vec2 q0{.5f, .5f}, q1{10.3f, .7f}, q2{10.1f, 9.6f}, q3{.7f, 9.4f};
    const Vec2 qa[3] = {q0, q1, q2}, qb[3] = {q0, q2, q3};
    Cover ca = cover(qa, 16, 16, false), cb = cover(qb, 16, 16, false);
    Vec2 e{q2.x - q0.x, q2.y - q0.y};
    float len = std::sqrt(e.x * e.x + e.y * e.y);
    Vec2 u{e.x / len, e.y / len};
    int near = 0, gaps = 0;
    for (int y = 0; y < 16; ++y) for (int x = 0; x < 16; ++x) {
        Vec2 p{x + .5f, y + .5f}, d{p.x - q0.x, p.y - q0.y};
        float along = d.x * u.x + d.y * u.y;
        float perp = std::fabs(-d.x * u.y + d.y * u.x);
        if (along < .75f || along > len - .75f || perp > .25f) continue;   // 只查开线段贴边像素
        ++near;
        if (ca.hits[(size_t)y * 16 + x] + cb.hits[(size_t)y * 16 + x] == 0) ++gaps;
    }
    std::printf("awkward:   nearEdge=%d gaps=%d\n", near, gaps);
}
```

实际打印（g++ 与 cl 一致）：

```
diag1-new  coveredA=10 coveredB= 6 sum=16 union=16 double=0
diag2-new  coveredA=10 coveredB= 6 sum=16 union=16 double=0
diag1-old  coveredA=10 coveredB= 6 sum=16 union=16 double=0
steep-new  coveredA= 0 coveredB= 7 sum= 7 union= 7 double=0
steep-old  coveredA= 1 coveredB= 8 sum= 9 union= 8 double=1
awkward:   nearEdge=6 gaps=0
```

读法：4×4 正方形沿对角线劈开，16 个像素中心必须各被写一次——`sum==union==16`、`double==0`
（仓库 `test_raster_scalar.cpp:71-108` 两条对角线都测：只有一方向失败 ⇒ top/left 符号错）。
**判据用覆盖数之和，不需要手算 mask**：缝 ⇒ 和 < 16，双写 ⇒ 和 > 16。陡边那组左三角形是个薄片，
一个像素中心都没落进去（`covered=0`）——**覆盖数不是面积**。旧规则在 ±45° 上 `double=0`（看不出），
在陡边上 `double=1`（共享端点被两边认领）：这就是为什么一组对称夹具不够。

`awkward` 那组带小数、非轴对齐，对应仓库的共享边不变式测试
（`shared_edge_leaves_no_gap_even_with_awkward_coordinates`，`test_raster_scalar.cpp:266-308`）。
**它带着一个撤回**：commit `5b90d52` 的标题就是 "shared-edge coverage invariant (and a retraction)"。
作者本想复现"立方体正面一条点状对角缝"，**没复现出来**——直接读那张图沿对角线的像素颜色一路平滑，
先前那句"C3 的修复可能还不完整"是错的，撤回；所谓缝是把缩略图重采样的图案当成了测量结果。
测试保留，因为它断言的不变式是真的：贴着共享边的像素至少被一个三角形认领。根因本该是 `evaluate(p)==0`
这个**精确比较**：共享边两侧用相反顶点顺序算边函数，实数域里 `v_A=-v_B`，浮点域里差几个 ULP，
边界像素可能同时被判外侧 ⇒ 缝。注意它只查**开线段**贴边像素（`along` 去掉两端 0.75），端点角点另当别论。

还有一条被推翻的期望值：`covered ≈ 面积 32` 是错的，精确值是 **36**（片段 1 的 `pick16` 实测）。
Pick 定理 `A=I+B/2-1 ⇒ 32=I+24/2-1 ⇒ I=21`，加顶边 8 + 左边 8 − 左上角重复 1 = 36；斜边上的
8 个边界点一个都不算（`test_raster_scalar.cpp:234-252`、M1 notes §6）。像素中心计数本来就不等于面积。

### 后端为什么是一个接口

生产版把光栅器藏在一个接口后：`RenderBackend::drawRange(mesh, state, firstIndex, indexCount)`
（`render_backend.h:10-24`）。**虚调用每个 draw call 一次，内层不再有**（C5，设计文档 92-93 行）。
内层逐像素用函数指针 shader（`pipeline.h:102-105`）和模板回调单态化——`forEachCoveredPixel` 是模板
（`triangle_geom.h:75-76`），`rasterOne` 是 lambda（`render_backend.cpp:56-57`），都是为了编译器能内联，
任何间接派发都会废掉 M9 的 SIMD。最简单的 FS 就是 `flatColorFS`：`out.color = u.baseColor; return true;`
（`builtin_shaders.cpp:38-41`）。接口存在的理由：M8 的 tiled 后端、M9 的 SIMD 后端要**加进来**而不是
推翻重写（设计文档 §6，117-124 行）；两个后端共享同一份 `TriangleGeom` 与 fill rule
（`triangle_geom.h:14-16`），正确性不因优化而改变（§7.8，±1/255，305-306 行）。

`drawRange` 的偏移是 **`mesh.indices` 的下标**，不是顶点下标（M5.0，`render_backend.h:15-18`）——
OBJ 多材质分组要按同一单位喂子区间。坏区间**只许少画，不许越界读**：`begin=min(firstIndex,n)`、
`count=min(indexCount,n-begin)`（`render_backend.cpp:36-39`）。测试用**非恒等**索引
`{3,4,5,0,1,2}` 把"当成顶点下标"的变异钉死（`test_draw_range.cpp:14-17、62-86`），
并专门测越界夹钳（117-143 行：`firstIndex=99` ⇒ 零三角形不崩；`indexCount=UINT32_MAX` ⇒ 夹成剩余全量）。

### 三个计数器

`FrameStats` 分 `fragmentsCovered`（通过覆盖判定）、`fragmentsWritten`（实际落色）、
`fragmentsDepthRejected`（被深度拒）（`stats.h:22-27`）。**差就是诊断**：无 discard 时
`covered − written = depthRejected`。深度比较用 `<=`（共面后画者赢，Z-fighting 的取舍，
`render_backend.cpp:89-99`；设计文档 §7.1 166-167 行：top-left、`<=`、clear 1.0、float32，两后端必须一致）。
片段 1 的 flat 路径没有深度，所以 `covered==written`；`RasterStats` 只有
`pixelsCovered/pixelsWritten/trianglesRejected`（`raster_scalar.h:14-18`）。计数用 `int64_t`，
几何求值用 `float`——这是项目各处的选择（`stats.h:14-27` vs `plane_eq.h:40-41`）。

## 坑（一手来自 docs/notes/）

| 坑 | 症状 | 权威 / 修法 |
|---|---|---|
| 内侧法线只对一种绕向成立 | 正面全 `covered=0`，像相机没对上 | `fromEdge` 加 `sign`；`reversed_winding_covers_exactly_the_same_pixels`（M2 §3c） |
| 口算"顺时针" | 测试第一版与实现相反 | 四步推导链 + 语义 `dot` 判据（M2 §3a/b） |
| 旧 top-left 用第三顶点 x | 陡边缝 + 双写，±45° 看不见 | 改内侧法线判侧向（`cd7c6b0` C3） |
| `covered≈面积 32` | 实测 36 | Pick 定理 + 顶/左边界（M1 §6） |
| "对角缝"声称 | 缩略图重采样图案 | 撤回（`5b90d52`） |

**注释会说谎，权威是代码和测试——这是刚发生的一次清扫（commit `d927025`）。** 我核对时
发现三处陈旧陈述，主控端已修：`vec.h` 原把内侧法线写成 `{e.y,-e.x}`（与 `plane_eq.h:52` 的
`{-e.y*sign, e.x*sign}` 相反），现 `vec.h:40-42` 改正为 `{-e.y,e.x}` 并指回实现与验证例。
`mesh.cpp` 原写"正对姿态 8 个侧面退化"，与 `test_pipeline_integration.cpp:110-125` 实测
（侧面 dot=-1 是**背对**，剔除 10、退化 0）矛盾，而 M2 notes:55 早记了撤回。**同一个错误在
三个地方各活了一份**：mesh.cpp 注释、`test_triangle_setup.cpp:99` 的"棱对"、还有一个**测试名**
（原名含 `edge_on_faces_as_degenerate_not_backfacing`，断言却是 backfacing=10/degenerate=0，
已改名 `axis_aligned_cube_reports_side_faces_as_backfacing_not_degenerate`）。测试名说反比注释
更贵——名字是别人 grep 的东西。同批还扫掉 `triangle_geom.h:77` 返回值括号（extent 在 :66 已
clampedTo，循环无第二次边界判定）。一次撤回到处漏：**权威是测试，不是我的推导**（M1 §2）。

## 你应该看到 X；如果你看到 Y，说明 Z 错了

你应该看到：`pick16: covered=36`、`tri256: covered=17805`，`tri.bmp` 是黑底红色三角形
（独立读回红色像素 17805）；片段 2 两条对角线 `sum=union=16 double=0`，`steep-new double=0`，
`steep-old double=1`，`awkward gaps=0`。

如果你看到：
- **全黑 / `covered=0`**：内侧法线符号写反，或忘了用 `sign` 吸收绕向（负面三角形全被拒）。
- **画满整屏**：符号反到另一侧，"内侧"判成了外侧的补集。
- **对角线 `sum<16`**：有缝（边界像素谁都不认领）；**`sum>16`**：双写。先查 top-left 判据。
- **`steep-old double=0`**：你的旧规则复刻不对，或夹具换了对称形状——这个坑依赖第三顶点位置。
- **接上立方体后内外翻 / 该剔的没剔**：绕向约定与 y 翻转没耦合好，回到四步链，别口算顺逆时针。