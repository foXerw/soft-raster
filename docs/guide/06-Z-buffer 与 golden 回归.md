# 06 — Z-buffer 与 golden 回归

## 这一章要解决什么问题

第 03 章的光栅器无条件写颜色：谁后画谁赢（painter 缺陷）。第 05 章给了透视正确插值，深度已在 `setup.planeDepth`
里（`src/render/interpolate.h:31`），且**深度在屏幕空间是仿射的**，不需要除（`interpolate.h:21`；
`depth_is_affine_in_screen_space`，`tests/unit/test_interpolate.cpp:84`）。这一章消费它：每像素一个 float，近的赢；
第二件交付物是**一张会失败的基准图**——golden 的价值不在"它绿着"，而在"你弄坏东西时它真的红过"。

## 深度缓冲：三个约定，一个都不能反

`DepthTarget` 是非拥有 float 视图（`src/render/render_target.h:32-50`）；约定来自设计文档（`docs/superpowers/specs/2026-11-21-soft-raster-design.md:164、167`）：

| 约定 | 代码 | 反了的症状 |
|---|---|---|
| 非 reverse-Z：`ndcZ*0.5+0.5`，[−1,1]→[0,1]，**近小远大** | `src/render/viewport.cpp:15` | 所有遮挡反过来 |
| clear 到 **1.0**（最远），float32 | `render_target.h:44` | 清成 0 ⇒ 首个片段之后全被拒 |
| 比较用 **`<=`** | `src/render/render_backend.cpp:95` | 共面平局换赢家（见下） |

深度值是非线性 `z/w`（近高远密）；方向由 `depth_values_are_written_and_monotonic_with_distance` 钉住：近处中心 <
远处边缘，都在 [0,1]（`tests/unit/test_depth.cpp:200-212`）。**"哪个面赢"是行为，"数值方向"是另一件事**，分开红。

## 为什么是 `<=`，不是 `<`

生产代码（`render_backend.cpp:92-102`）：读旧值，`if (!(ip.depth <= d))` 则拒并计数。两个细节：**共面平局**——
深度相等时 `<=` 让**后画者赢**，Z-fighting 的取舍被 pin 住而不是假装不存在
（`coplanar_triangles_last_writer_wins_because_comparison_is_less_or_equal`，`test_depth.cpp:93-129`：同几何同深度
只换颜色，断言第二个画进去了）；`<` 则先画者赢，共面的后画细节（贴花、同深度覆盖层）永远出不来。两种选择都在
平局上依赖绘制顺序，设计文档拍板 `<=` 且两后端必须一致（设计文档 :167）。**否定形式顺带挡 NaN**：`!(NaN <= d)` 为真
⇒ NaN 被拒，写成 `if (ip.depth > d) reject` 会放 NaN 进去。`depthTest` 与 `depthWrite` **互相独立**
（`src/render/pipeline.h:120-121`）：write=false 既不遮挡也不污染深度
（`render_backend.cpp:119`；`depth_write_false_does_not_occlude_and_does_not_pollute`，`test_depth.cpp:131-167`）。
绑在一起是容易犯的错，所以有专门测试分开失败。

### 最小片段 1：两种提交顺序 × 两种比较

独立编译。远 F 覆盖 6 像素、近 N 覆盖 3 像素且全在 F 内；近先/远先各一遍，再跑共面一对的 `<=` 与 `<`。

```cpp
// 片段 1：独立 Z-buffer 光栅器。两个重叠三角形、两种提交顺序、<= 与 < 两种比较。
// g++ -std=c++20 -Wall -Wextra -ffp-contract=off snippet1.cpp -o snippet1
// cl /nologo /EHsc /std:c++20 /W4 /D_CRT_SECURE_NO_WARNINGS snippet1.cpp /Fe:snippet1.exe
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
struct Vec2 { float x, y; };
static float cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
struct Edge {                                   // E(p) = n·p + c，内侧为正（第 03 章）
    Vec2 n; float c;
    static Edge from(Vec2 a, Vec2 b, float s) {
        Vec2 e{b.x - a.x, b.y - a.y};
        Edge f{{-e.y * s, e.x * s}, 0.f};
        f.c = -(f.n.x * a.x + f.n.y * a.y);     // 使 E(a) == E(b) == 0
        return f;
    }
    float eval(Vec2 p) const { return n.x * p.x + n.y * p.y + c; }
    bool tl() const { return n.x > 0.f || (n.x == 0.f && n.y > 0.f); }
    bool covers(Vec2 p) const { float v = eval(p); return v > 0.f || (v == 0.f && tl()); }
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
struct Counters { int covered = 0, written = 0, rejected = 0; };
static const int W = 4, H = 4;
struct Frame {
    uint32_t color[W * H]; float depth[W * H];
    void clear() { for (int i = 0; i < W * H; ++i) { color[i] = 0xFF000000u; depth[i] = 1.0f; } }
};
// 深度比较与生产同形：render_backend.cpp 的 `if (!(ip.depth <= d)) reject`。
// lessOrEqual=false 时改成 `<`，演示共面平局下两种选择的差别。
static void draw(Frame& f, const Vec2 v[3], float depth, uint32_t color, bool le, Counters& c) {
    Tri t{};
    if (!build(v, W, H, t)) return;
    for (int y = t.y0; y <= t.y1; ++y) for (int x = t.x0; x <= t.x1; ++x) {
        Vec2 p{x + .5f, y + .5f};
        if (!(t.e[0].covers(p) && t.e[1].covers(p) && t.e[2].covers(p))) continue;
        ++c.covered;
        float d = f.depth[y * W + x];
        if (!(le ? (depth <= d) : (depth < d))) { ++c.rejected; continue; }
        f.color[y * W + x] = color; f.depth[y * W + x] = depth; ++c.written;
    }
}
static void report(const char* name, const Frame& f, const Counters& c) {
    std::printf("%-16s covered=%2d written=%2d rejected=%2d  probe(0,0)=%08X depth=%.2f\n",
                name, c.covered, c.written, c.rejected, f.color[0], (double)f.depth[0]);
}
int main() {
    // 深度映射与 viewport.cpp:15 同式：ndcZ*0.5+0.5，[-1,1] -> [0,1]，近小远大（非 reverse-Z）。
    std::printf("map: ndcZ=-1 -> %.1f   ndcZ=+1 -> %.1f\n", (double)(-1.f * .5f + .5f),
                (double)(1.f * .5f + .5f));
    const Vec2 F[3] = {{.5f, .5f}, {3.5f, .5f}, {.5f, 3.5f}};   // 大三角形，6 像素
    const Vec2 N[3] = {{.5f, .5f}, {2.5f, .5f}, {.5f, 2.5f}};   // 小三角形，3 像素，全在 F 内
    const uint32_t green = 0xFF00FF00u, blue = 0xFF0000FFu;     // 0xAARRGGBB（蓝是 0xFF0000FF）
    const uint32_t red = 0xFFFF0000u, cyan = 0xFF00FFFFu;
    Frame f; Counters c;
    f.clear(); draw(f, N, 0.25f, green, true, c); draw(f, F, 0.75f, blue, true, c);
    report("near-first <=", f, c);                              // 情形 1：近先远后
    f.clear(); c = Counters{};
    draw(f, F, 0.75f, blue, true, c); draw(f, N, 0.25f, green, true, c);
    report("far-first  <=", f, c);                              // 情形 1：远先近后
    f.clear(); c = Counters{};
    draw(f, F, 0.5f, red, true, c); draw(f, F, 0.5f, cyan, true, c);
    report("coplanar   <=", f, c);                              // 情形 2：共面，<= 后画者赢
    f.clear(); c = Counters{};
    draw(f, F, 0.5f, red, false, c); draw(f, F, 0.5f, cyan, false, c);
    report("coplanar   <", f, c);                               // 情形 2：< 先画者赢
    return 0;
}
```

实际打印（g++ 与 cl 一致）：

```
map: ndcZ=-1 -> 0.0   ndcZ=+1 -> 1.0
near-first <=    covered= 9 written= 6 rejected= 3  probe(0,0)=FF00FF00 depth=0.25
far-first  <=    covered= 9 written= 9 rejected= 0  probe(0,0)=FF00FF00 depth=0.25
coplanar   <=    covered=12 written=12 rejected= 0  probe(0,0)=FF00FFFF depth=0.50
coplanar   <     covered=12 written= 6 rejected= 6  probe(0,0)=FFFF0000 depth=0.50
```

读法：**两种顺序最终像素相同**（probe 都是绿、深度 0.25）——Z-buffer 相对 painter 的全部意义；但**计数器不同**：
近先画，远的 3 个重叠像素被拒；远先画，一个不拒。仓库测试刻意"近先远后"（`test_depth.cpp:53-55`）：反过来画，
`fragmentsDepthRejected > 0` 测的其实是"绘制顺序恰好让深度没机会起作用"（M3 notes §3）。共面两行是 `<=` 与 `<` 的
唯一差别：cyan（后画）对 red（先画），rejected 0 对 6。

## 三个计数器：差就是诊断

`FrameStats` 分 `fragmentsCovered`（过覆盖判定）、`fragmentsWritten`（实际落色）、`fragmentsDepthRejected`（被深度拒）
（`src/render/stats.h:22-27`）。无 FS discard 时 **covered − written = depthRejected**；有 discard 时差里还含被丢弃的
片段（`render_backend.cpp:112` 的 `return` 在深度通过之后、深度写入之前 ⇒ discard 也不污染深度）。为什么要三个：
written 少，可能没覆盖、可能被深度拒、可能被 FS 丢——**比值定位阶段**：`covered=0` 查绕向/剔除（第 03 章），
`covered≫written` 且 `rejected≈0` 查 FS，该遮挡的场景 `rejected=0` 查开关与绘制顺序（M8 后端一致性靠它们对账，`stats.h:23`）。

**实测一例**（我直接编译生产函数运行，MSVC）：`renderGoldenScene` 与 `renderGoldenSceneTextured` 都给
`submitted=12 rasterized=6 culled=6`，`covered=written=shaded=4239，depthRejected=0`。凸立方体 + 背面剔除 ⇒ 正面互不
遮挡，**0 是期望值**（仓库这样断言，`tests/unit/test_golden_scene_textured.cpp:333-334`）；在此夹具断言 `>0` 是空转
（非零拒绝需要重叠几何，`test_depth.cpp:70-79` 刻意近先远后）。

## golden 回归：怎么写、怎么比、容差在哪

**参考图必须由被测的同一个函数生成**（M3 notes §6）：场景在 `src/scene/golden_scene.cpp`，测试与 app 的
`--golden-write*` 调同一个函数（`src/scene/golden_scene.h:46-48`）。320×180 抓回归非画质（`golden_scene.h:13-16`）；
完全确定：无随机无时间项（:12），姿态常量生产与测试共用（:25-29）；HUD 也进 golden，一次覆盖字体表、像素写入、
打包顺序（`golden_scene.cpp:94-97`）。写出是**四个显式 flag，
没有默认场景**（`src/app/cmdline.cpp:109-131`，Ruling 38；互斥 :294-301；`--golden-write-obj` 必须配 `--obj`，:305-307）
——默认值会让"忘指定场景"把 M4 的图画进 M3 的路径，症状是"重新生成之后一切正常"（`assets/golden/README.md:14-16`）。

比对只有一份循环（`tests/golden/golden.cpp:41-86`）：两份副本会让容差/报告/缺图行为只被改到一处，没被改的那份
一直绿着（:38-40）。三条硬规则：**缺参考图 = 失败不是 SKIP**（:50-58，静默 SKIP 让"从没跑过"看起来像全绿）；
**路径由 CMake 注入 `SR_SOURCE_DIR`**（`CMakeLists.txt:151`，CTest 的 cwd 是 build/，相对路径把"找不到"误读成
"没提交"）；**只在 MSVC 判定**，g++ 打印可见 `[skip]`（:91-95，C1′，第 00 章）。BMP 往返：写侧**先校验、再拼帧、
最后才开文件**（`src/loader/bmp_writer.cpp:40-55`，坏参数会截断已有截图、看起来像渲染回归），行序翻转只此一次
（:91-101）；读回在 `tests/golden/bmp_read.h` 而非 loader（:2-4），只收 BI_RGB 24bpp（:25），行序还原一次（:31-38）。

**容差分两档，精确比对另有其主**（`assets/golden/README.md:22-32`，Ruling 50）：

| 门禁 | 对象 | 容差 |
|---|---|---|
| ctest 三张参考图 | M3/M4/M6 lit | **±1/255/通道**（`golden.cpp:25`，设计文档 :303） |
| M4 另要求 md5 逐字节可复现 | `m4_textured_cube.bmp` | **0**（锚 3 依赖此性质） |
| 锚 3：OBJ 场景 vs M4 参考 | 进程内，跳过 HUD 条 | **0**（`test_obj_golden.cpp:162-233`） |
| 确定性/非它帧等守卫 | 各测试 | 逐像素精确（如 `test_golden_scene_textured.cpp:351`） |

我实测（MSVC 直接编译生产源码）：M4 今天生成 vs 参考 **md5 同为 `1f767a114a49d38c454448eec4465d51`**；M3 ±1 绿、
精确差 **2 字节各 −1**（`cmp -l` 偏移 64925/91699，与 README 记录一致）——参考图停在 `3f8cc2a`，之后
src/render+src/scene 变了 12 个提交而图没重生成。**拿 md5 当 M3 门禁会误判自己的重构**；"只改 1/255"的回归在 M3
上抓不住——弱一档门禁的诚实代价。

**抵消不是逐字节精确的（Ruling 77）**：M5 同时删掉 OBJ 的 `v=1-v` 与 TGA 的 origin 翻转，画面肉眼正确，锚 3 仍红
——**3 个像素、各 ±1/通道**（M5 notes §1 记坐标 (183,64)/(181,84)/(148,104)；我读参考图核对期望侧字节 FFE2E3E9 /
FF1D1F24 / FF646569，逐值相符；三坐标都在 HUD 条之外）。残差来自三线性混合的浮点舍入。三档结论：±1 门禁下抵消
**完全不可见**；零容差能红，**但抓它的是舍入噪声不是语义**，换编译器/SIMD 可能消失；可靠判别只能是锚 1/2（M5 notes §1）。

**重生成铁律**：golden 只有产品方**亲自肉眼验收**后才许重生成（验证纪律 #4；`assets/golden/README.md:52-62`："然后
肉眼看一次再提交"，且必须补一行原因）。M6 lit 图在 2026-11-26 人眼门通过后才生成（`assets/golden/README.md:39-40`；
M6 账本 Ruling 82）。练习写临时路径（`--golden-write build/my_m3.bmp`），别动 `assets/golden/`。

### 最小片段 2：一个会红的 golden

确定性图案（R 竖直渐变、G 水平渐变——**夹具必须在 y 翻转下不对称**）；BMP 写→读→精确/±1 比对；再故意翻 y 映射。

```cpp
// 片段 2：最小 golden 回归。确定性图案 -> BMP 写 -> 读回 -> 精确/±1 两种比对；
// 然后故意翻 y 映射（M3 notes §6 的破坏性验证），看 golden 变红。
// g++ -std=c++20 -Wall -Wextra -ffp-contract=off snippet2.cpp -o snippet2
// cl /nologo /EHsc /std:c++20 /W4 /D_CRT_SECURE_NO_WARNINGS snippet2.cpp /Fe:snippet2.exe
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
static const int W = 16, H = 16;
// 确定性图案：R 竖直渐变（y*16）、G 水平渐变（x*16）。竖直渐变在 y 翻转下每行都变
// —— 夹具必须不对称，否则变异 invisible（M4 notes §1 那族坑）。
static void render(std::vector<uint32_t>& px, bool flipY) {
    px.assign((size_t)W * H, 0xFF000000u);
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        int row = flipY ? (H - 1 - y) : y;          // 被测的"视口 y 映射"
        uint32_t r = (uint32_t)(row * 16), g = (uint32_t)(x * 16);
        px[(size_t)y * W + x] = 0xFF000000u | (r << 16) | (g << 8) | 64u;
    }
}
static bool writeBmp(const char* path, const std::vector<uint32_t>& px) {
    int rb = W * 3, pad = (4 - rb % 4) % 4; uint32_t ds = (uint32_t)((rb + pad) * H);
    uint8_t h[54] = {};
    auto p32 = [&](int o, uint32_t v) { for (int i = 0; i < 4; ++i) h[o + i] = (uint8_t)(v >> (8 * i)); };
    h[0] = 'B'; h[1] = 'M'; p32(2, 54 + ds); p32(10, 54); p32(14, 40);
    p32(18, (uint32_t)W); p32(22, (uint32_t)H); h[26] = 1; h[28] = 24; p32(34, ds);
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fwrite(h, 1, 54, f);
    std::vector<uint8_t> row((size_t)rb + pad, 0);
    for (int y = H - 1; y >= 0; --y) {              // 文件 bottom-up，内存 top-down，写时翻行
        for (int x = 0; x < W; ++x) {
            uint32_t c = px[(size_t)y * W + x];
            row[(size_t)x * 3] = (uint8_t)c; row[(size_t)x * 3 + 1] = (uint8_t)(c >> 8);
            row[(size_t)x * 3 + 2] = (uint8_t)(c >> 16);
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}
static bool readBmp(const char* path, std::vector<uint32_t>& px) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    uint8_t h[54];
    if (std::fread(h, 1, 54, f) != 54 || h[0] != 'B' || h[1] != 'M') { std::fclose(f); return false; }
    int stride = (W * 3 + 3) & ~3;
    std::vector<uint8_t> row((size_t)stride);
    px.assign((size_t)W * H, 0u);
    for (int y = 0; y < H; ++y) {
        std::fseek(f, (long)(54 + (size_t)(H - 1 - y) * stride), SEEK_SET);  // 唯一一次行序还原
        if (std::fread(row.data(), 1, row.size(), f) != row.size()) { std::fclose(f); return false; }
        for (int x = 0; x < W; ++x)
            px[(size_t)y * W + x] = 0xFF000000u | ((uint32_t)row[(size_t)x * 3 + 2] << 16) |
                                    ((uint32_t)row[(size_t)x * 3 + 1] << 8) | row[(size_t)x * 3];
    }
    std::fclose(f);
    return true;
}
struct Diff { int exact = 0, tol1 = 0; int firstX = -1, firstY = -1; };
static Diff compare(const std::vector<uint32_t>& got, const std::vector<uint32_t>& want) {
    Diff d;
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) {
        uint32_t a = got[(size_t)y * W + x], b = want[(size_t)y * W + x];
        if (a == b) continue;
        if (d.exact == 0) { d.firstX = x; d.firstY = y; }
        ++d.exact;
        int dr = std::abs((int)((a >> 16) & 0xFF) - (int)((b >> 16) & 0xFF));
        int dg = std::abs((int)((a >> 8) & 0xFF) - (int)((b >> 8) & 0xFF));
        int db = std::abs((int)(a & 0xFF) - (int)(b & 0xFF));
        if (dr > 1 || dg > 1 || db > 1) ++d.tol1;   // ±1/255/通道（golden.cpp:25）
    }
    return d;
}
static void report(const char* name, const Diff& d) {
    std::printf("%-22s exact=%3d  ±1=%3d  first=(%d,%d)  %s\n", name, d.exact, d.tol1,
                d.firstX, d.firstY, d.tol1 > 0 ? "RED" : "green");
}
int main() {
    std::vector<uint32_t> ref, back, got;
    render(ref, false);
    if (!writeBmp("ref.bmp", ref) || !readBmp("ref.bmp", back)) { std::printf("BMP 往返失败\n"); return 1; }
    report("roundtrip", compare(back, ref));        // 写->读->比：0/0 才说明往返无损
    got = back;                                     // 模拟 M3 那种 ±1 漂移：三个像素 R 通道 +1
    got[(size_t)2 * W + 3] += 0x00010000u;
    got[(size_t)5 * W + 7] += 0x00010000u;
    got[(size_t)9 * W + 1] += 0x00010000u;
    report("drift +1 (3 px)", compare(got, back));  // 精确红、±1 绿：容差放过了什么
    render(got, true);                              // 破坏性验证：翻 y（等价于删 viewport.cpp:14 的 1.0f-）
    report("mutant flipY", compare(got, back));     // 两种门禁都必须红
    int failed = compare(got, back).tol1 > 0 ? 1 : 0;   // 退出码夹到 1（tests/main.cpp:62-65 同形）
    std::printf("exit=%d\n", failed);
    return failed;
}
```

实际打印（两工具链一致；退出码 1，最后一组故意红）：

```
roundtrip              exact=  0  ±1=  0  first=(-1,-1)  green
drift +1 (3 px)        exact=  3  ±1=  0  first=(3,2)  green
mutant flipY           exact=256  ±1=256  first=(0,0)  RED
exit=1
```

读法：往返 0/0 证明写读互逆；+1 漂移**精确红、±1 绿**——容差放过的正是 M3 那 2 字节那一类；翻 y 后 256/256 全红
（竖直渐变每行都变）。仓库做过同款破坏性验证：删 `Viewport` 的 y 翻转 ⇒ golden 大量差异并失败，恢复 ⇒ 全绿
（M3 notes §6；`assets/golden/README.md:71-74`）。**没做过这一步的 golden 框架等于没有。**

### golden 证明不了什么

它只钉一个姿态一个场景：足迹外的 bug 不可见；也分不清"正确"与"重生成到与 bug 一致"。所以参考图周围有一圈守卫，
各防一种假通过：确定性（同函数两帧逐像素相同，`test_golden_scene_textured.cpp:337-353`）；**非它帧**（与 M3 帧差
>1500 像素且 HUD 区 >0，:355-379）；**不饱和**（lit 表面区最大通道 <255 且高光像素 >0，
`tests/unit/test_golden_lit.cpp:341-381`——过曝白饼的坏重生成必红某条）；lit 帧非 M3/M4 帧（:383-414）。字节值依赖
"输入只解码一次、输出单一编码点"（M6 notes §④）。最后一道门永远是人眼。

## 三条锚：一条方向约定，一颗独立的钉子

M5 把三条独立方向约定翻译进管线（M5 notes §1）：OBJ 的 vt（v=0 是图片下边缘）、TGA 的 origin bit、PNG/JPG 行序
（stb 翻转显式关掉）。前两条**都是翻一次行**：obj 忘翻 + tga 忘翻 ⇒ 合成画面看起来完全正确——本项目最危险的
一类假通过。结构性解法（M5 设计 §10，`docs/superpowers/specs/2026-11-22-soft-raster-m5-obj-design.md:301-330`）：
**每条翻转各对着一个非对称基准被钉住，绝不允许只有合成路径的断言。**

| 锚 | 测试种类 | 变异③（双翻转）下的红 |
|---|---|---|
| 1 | **逐字段数值**：cube.obj 解析 vs makeCube，规范化三角形 24 float 逐元素，1e-5（`test_obj_golden.cpp:94-160`） | 12/12 三角形（语义级） |
| 2 | **逐字节**：checker.tga 读回 vs makeCheckerImage 独立基准 | 49152 字节（语义级） |
| 3 | **合成零容差**：OBJ 场景 vs M4 参考图，跳过 HUD 条（`test_obj_golden.cpp:162-233`，:215） | 仅 3 个 ±1 舍入像素 |

（红数记录于 M5 notes §1 与 `test_obj_golden.cpp:175-180` 注释；我核对过参考图侧字节，未重跑变异。）锚 1 规范化取
角序三旋转的字典序最小者：**吸收旋转、不吸收镜像**（:54-56）——镜像必红。锚 2 的棋盘必须三角不对称标记色
（Ruling 36，纯棋盘翻转下不变）。锚 3 的 `map_Kd` 按后缀路由到**我们自己的** TGA 解析器（Ruling 63），否则锚 2 守的是
golden 不走的路。③下锚 3 的红是舍入残差，**不可依赖**——最坏读法"锚 3 已能抓抵消，锚 1/2 是保险"会诱导删掉两条语义锚（P33）。

**为什么逐字段数值断言是杀手、包含性检查不是**——有编号的裁定（M5 账本 **Ruling 69**，fit 相机 D16 陈述被变异
证伪）：`near=r/100` 放松 100 倍，8 角 NDC 包含断言**仍绿**；`distance×2` 仍绿；退化阈值放宽到 1e-3，**包含绿、
逐字段数值断言红**。包含断言钉"模型在这个距离上装得下"这个承诺，数值断言钉 distance/near/far 三个数，不可互替
（Ruling 72 同族：负 aspect 镜像 eye，包含断言投影的 target 永远在视轴上 ⇒ 字段断言是唯一杀手）。锚的架构是同一
认识论：**谓词在错误下不变，就抓不住那个错误**；逐字段/逐字节打破不变性，合成与包含检查不破。

## 工具链现实：会变成测试失败的那几件

一行一条，细节在第 00 章与 notes：

- **缺 DLL = CTest Timeout**，不是报错：进程起不来（M0 notes §7c）。纪律：`ctest` 一律带 `--timeout`（第 00 章坑 1）。
  同族：Debug CRT 模态框、abort 弹框（`tests/main.cpp:16-49` 关框 + 改道 stderr + 立即 `_exit(3)`；M0 §7b、Ruling 60）。
- **退出码夹到 1**：`return failed > 1 ? 1 : failed`（`tests/main.cpp:62-65`）。POSIX 截断到 0–255，恰好 256 条失败
  ⇒ rc=0 像全绿。M4 终审发现、**M5.0 修复**（commit `1fdf43f`；`git log -S "failed > 1 ? 1 : failed" -- tests/main.cpp`
  命中它，`-- tests/harness.cpp` 为空——夹钳从来不在 harness 里）。
- **测试 TU 必须显式列进 `add_executable`**（`CMakeLists.txt:86-88` 注释、:92-140 列表，`test_depth.cpp` 在 :115）：
  做成静态库再链，自注册 TU 被整个丢掉，**零用例零报错**（第 00 章坑 3）。
- **Release 验证一律 `--clean-first`**（M4 notes §10）：增量构建藏警告——`plane_eq.h` 那条 C4723 是全量重建才定位到
  的。配套：**构建失败绝不读测试结果**。

## 这一章教的验证纪律：三条，各有一个案名

1. **一次 abort 的运行不算变异验证的证据**（M5 账本 **Ruling 57**，M5.2）：负索引夹具让错解落表外，OOB 读发生在
   任何断言之前 ⇒ MSVC Debug STL abort，拿不到"红了哪几条"。改成错解落表内的夹具（4v/2vt + `f 1/-3`）⇒ 干净断言级红
   （M5 notes §2）。
2. **变异必须被展示为真的红过**：M3 §6 删 y 翻转→红→恢复→绿（片段 2 复现）。推论是 **Ruling 107**（M6 账本）：
   每条"已验证"都必须能在仓库或 build/ 找到原始输出；把没跑过的验证写成跑过并通过，是本章一直在防的那件事的完成形态。
3. **夹具在目标错误下不变，就抓不住那个错误**：`nearer_triangle_wins` 第一版远先画，`fragmentsDepthRejected` 恒 0
   ——测的是"绘制顺序恰好让深度没机会起作用"（M3 notes §3，`test_depth.cpp:53-55`）。家族最大成员是双翻转抵消
   （M5 §1）；M4 notes §1 有此前五例。固定一问：**把两个轴/权重/取整/翻转对调，夹具会给出不同的数吗？**

## 绕向：只信符号，不信词

判据只有两个：`area2` 符号 + 语义 `dot(外法线, eye-顶点)`。top-down 屏幕空间里 **`area2 < 0` 是正面**
（`src/render/triangle_setup.cpp:46-49`），四步推导链在 `tests/unit/test_triangle_setup.cpp:53-60`，权威断言
`s.area2 < 0` 在 :69。正面肉眼是**逆**时针；y 翻转只翻有向面积符号，不翻肉眼旋向。**永远别拿 "CW/CCW" 当权威**
——`178e9db` 一批改 6 处 src 注释（另同步 M2/M6 notes），`a66fdfc` 第二批 4 处，含一条自称"权威"的测试注释，
顺带修掉 bmp_writer 的"96 DPI"假数字（实为 72，`bmp_writer.cpp:15-17`）。完整链条见第 03 章。

## 你应该看到 X；如果你看到 Y，说明 Z 错了

你应该看到：片段 1 四行计数如上（near-first rejected=3、far-first rejected=0、共面 `<=` 赢 cyan、`<` 赢 red）；片段 2
往返 0/0、漂移 exact=3/±1=0、flipY 256/256 且退出码 1。仓库侧：`ctest --test-dir build -C Debug --timeout 60` 全绿
（MSVC）；g++ 侧 golden `[skip]` 是 C1′ 预期。如果你看到：

- **遮挡反过来 / 远处赢**：深度方向反了（reverse-Z 混进来）或 depthTest 没开。先跑 `--only depth_values_are_written`，
  再跑 `--only cube_depth_flag`（`test_depth.cpp:169-198`：同场景只差一个标志，painter 缺陷与深度修正并排）。
- **共面后画者没赢**：你写成了 `<`；仓库约定是 `<=`，改回去或改测试前先想清楚在改哪条约定。
- **golden 红**：先看差异像素数与第一个坐标（`golden.cpp:75、82` 会打印）。个位数 ±1：多半是 M3 那类漂移或浮点
  路径变化；成千上万：真回归或拿错参考图。**不许未经产品方肉眼验收就重生成。**
- **golden "全绿"但你刚改了渲染**：查参考图是不是被顺手重生成过、`--only` 是不是拼错（0 匹配必须失败，
  `tests/harness.cpp:76-82`）、测试 TU 是不是没列进 CMake。**CTest Timeout** 则先查缺 DLL / 模态框（第 00 章坑 1）。

下一章（07）：纹理采样与 mipmap——uv 变成真的贴图坐标，关 mip 会闪，纹素中心约定。
