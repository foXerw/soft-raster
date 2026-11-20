# soft-raster M0–M3 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task（本计划由产品方明确指示"直接实施"，执行方式已由作者定为 Native）。Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 搭起一个纯 CPU 软件光栅化渲染器能跑起来的骨架，到 M3 结束时：窗口里有一个 MVP 变换、遮挡正确、顶点色平滑的旋转立方体，并有可回归的单元测试与 golden BMP。

**Architecture:** 可编程阶段管线（VS → 六面裁剪 → 透视除法 → 视口 → Primitive Setup → Rasterize → FS → Output sink），阶段边界是接口、阶段内部是直白代码。两个光栅后端（Backend A 标量逐像素 / Backend B tile+SIMD+多线程）消费同一份"裁剪后屏幕空间三角形"。颜色与深度归显式 `RenderTarget`，不属于后端。平台代码全部关在 `src/app/`，`soft_raster_core` 不知道 Win32 存在。

**Tech Stack:** C++20，CMake ≥ 3.24，MSVC (VS 2022) 主路径 + MSYS2 g++ 16.1 编译/纯逻辑单测路径，Win32 GDI + `CreateDIBSection` 显示，零第三方渲染依赖（唯一第三方文件 `third_party/stb_image.h`，M4 才引入）。

**Spec:** `docs/superpowers/specs/2026-11-21-soft-raster-design.md`（已批准，v2）。本计划实现其 §8 的 M0–M3，并给出 M4–M10 的概要。所有 G/C/N/A 编号指回该文档。

---

## Global Constraints

（逐条抄自 spec，每个任务的验收都隐含包含这一节）

- C++20；主构建路径 CMake + Visual Studio 2022。`cl` 不在普通 shell PATH：用 VS 生成器，或先 call `C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat`。
- C2/C3：构建期不联网、无包管理器、无 FetchContent/find_package 外部包。唯一第三方文件是 `third_party/stb_image.h`，只被 `src/loader/image_stb.*` 包含，`soft_raster_core` 不得感知它（M4 引入）。
- C4：`soft_raster_core` 内不得出现 `<windows.h>` 或任何 Win32 符号。
- C5：热路径不用异常、不逐像素虚函数派发（虚调用按 draw call 一次，内层用函数指针）、不逐像素分配内存、不加锁。
- C6：Debug 与 Release 都可调试可剖析；Release 不剥符号。
- C8：core 的颜色与深度输出**一律 top-down**（第 0 行是图像上边缘）；DIB 的 bottom-up 翻转只发生在 `src/app/dib.*`；BMP 写盘时再做一次翻转（BMP 存储自底向上）。
- C1′：golden image 只在 MSVC 主路径判定。g++ 侧要求 = 能配置、能编译、纯逻辑单测全绿。所有数值断言一律**容差比较**（默认 1e-5），不写"精确等于"。GCC 构建加 `-ffp-contract=off`。三角函数/`pow`/`log2` 用标准库。
- §7.1 光栅化规则（两后端必须一致）：fill rule 用 **top-left**；深度比较用 `<=`；深度 clear 到 `1.0`；深度缓冲 `float32`；非 reverse-Z。
- 像素打包：DIB 32bpp 内存字节序 B,G,R,A ⇒ 小端 `uint32_t` 值 = `0xAARRGGBB`。
- 代码注释用中文（解释数学推导），标识符用英文。
- 每个里程碑一篇 `docs/notes/Mn-*.md`（G8），属验收条件。
- 每个任务结束时提交一次；提交信息用 conventional commits（`feat:`/`test:`/`docs:`/`chore:`/`fix:`），并按本仓库约定结尾加 `Co-Authored-By: Claude Code <noreply@anthropic.com>`。
- 首次 push 前必须核对提交内容（不含 build/、不含 secrets）；`git push` 到 `main`。

### 构建与验证命令（全程复用）

```bash
# MSVC 主路径
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
# 期望输出末尾：  100% tests passed, 0 tests failed out of N

# 运行（窗口）
./build/Debug/soft-raster.exe --width 640 --height 360
# 离屏跑 N 帧并存 BMP（不需要窗口，golden/性能测量走这条路）
./build/Debug/soft-raster.exe --offscreen --frames 1 --out shot.bmp

# g++ 侧（C1′：只要求配置+编译+纯逻辑单测）
cmake -S . -B build-gcc -DSR_BUILD_APP=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build-gcc
ctest --test-dir build-gcc --output-on-failure
```

---

## File Structure（本计划建立的布局，抄自 spec §7.7 并落到文件级）

| 文件 | 单一职责 | 建立于 |
|---|---|---|
| `CMakeLists.txt` | 三个目标：`soft_raster_core`(静态库)、`soft-raster`(app)、`soft_raster_tests` | 0.2 |
| `.gitignore` | 排除 `build*/`、`*.bmp` 输出（`assets/golden/` 例外）、IDE 缓存 | 0.1 |
| `tests/harness.h` `tests/harness.cpp` | 自注册测试 + `SR_ASSERT` / `SR_ASSERT_NEAR` + `runAll()` | 0.2 |
| `tests/main.cpp` | `int main(){ return srtest::runAll(); }` | 0.2 |
| `src/math/vec.h` | `Vec2/Vec3/Vec4` + 运算 + `LinearColor` | 1.1 |
| `src/math/mat.h` | `Mat4`：乘、转置、逆、`upper3x3` | 1.2 |
| `src/math/transform.h` | `makePerspective/makeLookAt/makeTranslate/makeRotate*` | 1.3 |
| `src/math/bbox.h` | `BBox`（min/max + `fromPoints` + `center/radius`）、`Rect`（int 像素矩形 + `clamp/intersect/empty`） | 2.1 |
| `src/render/color.h` | `LinearColor` + `packBGRA` + 线性↔sRGB 编解码（256 项 LUT + 解析式） | **0.3** |
| `src/render/render_target.h/.cpp` | `ColorTarget`（含 pitch 的 top-down 视图）、`DepthTarget`（含 `view()` 子矩形）、`RenderTarget` | **0.4** |
| `src/render/patterns.h/.cpp` | `fillLinearGradient/fillChecker` 测试图案（core 内，可单测；依赖 0.3 的 `encodeSRGB`） | **0.4** |
| `src/loader/bmp_writer.h/.cpp` | 写 24bpp BMP（自底向上），从 top-down `ColorTarget` 取行 | 0.5 |
| `src/app/dib.h/.cpp` | `CreateDIBSection` + 行号取反（唯一的翻转点，C8） | 0.6 |
| `src/app/win32_window.h/.cpp` | 窗口类、消息循环、帧循环计时、输入状态 | 0.6 |
| `src/app/surface.h/.cpp` | `Surface` 抽象：`WindowSurface`（DIB）/ `OffscreenSurface`（heap） | 0.6 |
| `src/app/overlay_font.h/.cpp` | 8×8 位图字体，`drawText(ColorTarget&, x,y, text, packed)` | 0.7 |
| `src/app/cmdline.h/.cpp` | `--obj/--width/--height/--offscreen/--frames/--out/--backend/--threads/--bench/--no-fit` | 0.6 |
| `src/app/main.cpp` | 组装：解析 → surface → 帧循环 → 每帧绘制 | 0.6 |
| `src/render/vertex.h` | `VSIn/VSOut/Varyings/ClipVertex/ScreenVertex` | 2.1 |
| `src/render/pipeline.h` | `VertexUniforms/FragmentUniforms/DrawState`、`VertexShaderFn`、`FragmentShaderFn`、`ShadingMode` | 2.1 |
| `src/render/clipping.h/.cpp` | clip space 六面 Sutherland–Hodgman → 凸多边形（≤8 顶点） | 2.2 |
| `src/render/viewport.h/.cpp` | 透视除法 + NDC→屏幕（top-down）+ depth [0,1] | 2.3 |
| `src/render/triangle.h` | `TriangleSetup`（edge funcs、plane eqs、bbox、area2、flags） | 2.4 |
| `src/render/triangle_setup.h/.cpp` | 屏幕三角形 → `TriangleSetup`；朝向/背面剔除/零面积/scissor 夹取 | 2.4 |
| `src/render/plane_eq.h/.cpp` | `PlaneEq{n,c}` + `sample(x,y)`；`EdgeFunc` | 1.4 / 3.1 |
| `src/render/raster_scalar.h/.cpp` | **Backend A**：包围盒内逐像素 → 覆盖 → depth → FS → 写 | 1.4 |
| `src/render/render_backend.h` | `RenderBackend` 接口（每 draw call 一次虚调用，C5） | 2.1 |
| `src/scene/mesh.h` | `Mesh`（SoA pos/normal/uv + index）+ `makeCube/makeUVSphere/makeTorus` | 2.5 |
| `src/render/stats.h` | `FrameStats` 计数与分阶段计时 | 2.1 |
| `tests/unit/*.cpp` | 每个组件一套用例 | 各处 |
| `tests/golden/golden.cpp` | `renderFixedSceneToTarget()` + 与 `assets/golden/*.bmp` 比对 ±1/255/通道 | 3.5 |
| `docs/notes/M0-*.md … M3-*.md` | 每里程碑的"为什么" | 各处 |

**为什么这样切**：`render/` 里 clipping/viewport/setup/raster 各自是一个阶段，改一个阶段不会牵动另一个（G2 的直接体现）。`patterns` 与 `ColorTarget` 分开放，是因为测试图案要在 core 里可单测，而 `fillGradient` 属于"生成像素"而非"承载像素"。所有 `app/` 文件不参与单测目标——它们是唯一的不可单测层，因此必须薄。

---

## Review Focus

spec 隐含、但单个任务的测试不天然覆盖、且最容易咬人的 5 类。每行后面"钉在"= 把该断言加进哪个任务的测试里。

1. **y 方向与绕向耦合**：core 输出 top-down（C8）会把屏幕空间的三角形绕向翻转。如果背面剔除按 NDC 的 CCW=正面来判，翻 y 之后所有立方体的面会被错误剔除，症状是"半个立方体消失、转起来面在闪"。**钉在 2.4**（给定 top-down 屏幕坐标下 CW/CCW 两个三角形，断言被剔除的恰好是设计文档定义的那一侧）与 **2.5**（立方体六面全部可见且深度不冲突）。
2. **透视除法后的线性量存成了什么**：裁剪阶段必须对 `clip` 与 `varying/w`、`1/w` 做**同一组线性插值**，如果对未除 w 的 varying 插值，症状是纹理/颜色在四边形上被"拉回"到三角形中心，且随视角变化。**钉在 3.1**（用一个深度差很大的四边形，pin 两个已知像素的插值结果，与手工透视正确值比对）。
3. **零面积 / 退化 / `w<=0` 的后半空间三角形**：裁剪输出可能含重复顶点或共线三角形；`planeInvW.sample()` 在极端情形可趋零；不拒绝就会写 NaN 深度或除零，症状是黑斑随镜头移动。**钉在 2.2**（裁剪后顶点数与重合顶点）与 **2.4**（`|area2| < eps` 与非有限值必须整体丢弃并计入 `trianglesDegenerate`）。
4. **两后端的边界像素归属**：M8/M9 的硬约束是"正确性不因优化而改变"（spec §7.8）。若 Backend A 用 `< 0` 判边、Backend B 的 SIMD 掩码用 `>= 0`，相邻三角形之间会出现缝隙或双线 overdraw，症状是网格状暗线。**钉在 1.4**（一个正方形沿对角线劈成两个三角形，断言覆盖像素数恰好 = 像素网格面积，且**没有任何像素被写两次**——这条不需要手算 mask 就能把 fill rule 唯一化）；M8 再加 A/B 一致性。
5. **线性空间 ↔ sRGB 的两处翻转点**：光照必须在线性空间算、写颜色前编码。搞反的两种症状：画面整体发灰（未编码）或暗部死黑（重复编码）。另外 BMP 写盘和 DIB 显示各有一次行翻转，两次叠加会让 golden 与窗口上下颠倒。**钉在 0.3**（`linearToSrgb8(srgb8ToLinear(v)) == v`，且 `linearToSrgb8(0.5)` ∈ [186,188]）与 **0.5**（BMP 第一行数据对应图像**最下**行）。

---

## M0：仓库与可运行骨架

工作成果：一个窗口，里面有程序生成的渐变动画 + 自绘 FPS 文字；能离屏渲染并存 BMP。此时**还没有任何 3D**。

### Task 0.1: 仓库初始化

**Files:**
- Create: `.gitignore`
- Create: `README.md`（构建命令 + 里程碑进度表骨架）
- Modify: 无（`docs/superpowers/specs/2026-11-21-soft-raster-design.md` 已存在，纳入首次提交）

**Interfaces:**
- Consumes: 无
- Produces: git 仓库，`origin` = `https://github.com/foXerw/soft-raster.git`

- [ ] **Step 1: 初始化并接上远端**

```bash
cd /d/code/soft-raster
git init -b main
git remote add origin https://github.com/foXerw/soft-raster.git
git remote -v        # 期望：origin 已列出
```

- [ ] **Step 2: 写 `.gitignore`**

内容必须覆盖：`build/`、`build-*/`、`out/`、`.vs/`、`*.obj`（**注意与 3D 模型扩展名冲突**：用 `/`前缀的行锚定并把 `assets/**/*.obj` 设为例外）、`*.exp`、`*.lib`（同理，`third_party` 无 lib）、`*.user`、`shot*.bmp`、`docs/notes/img/` 之外的临时输出。

- [ ] **Step 3: 验证 ignore 生效**

Run: `mkdir -p build && touch build/x.txt && git status --porcelain`
Expected: 输出里**没有** `build/x.txt`，且没有 `*.user`。

- [ ] **Step 4: 首次提交**

```bash
git add .gitignore README.md docs/superpowers/specs/2026-11-21-soft-raster-design.md
git commit -m "chore: init repo with design spec v2"
```

- [ ] **Step 5: 核对首次提交内容后推送**

Run: `git show --stat HEAD`
Expected: 只有 3 个文本文件，无二进制、无 build 产物、无凭据。确认后才：`git push -u origin main`

### Task 0.2: CMake 三目标 + 测试 harness

**Files:**
- Create: `CMakeLists.txt`
- Create: `tests/harness.h`, `tests/harness.cpp`, `tests/main.cpp`
- Create: `tests/unit/test_smoke.cpp`, `tests/unit/test_version.cpp`
- Create: `src/version.h`, `src/version.cpp`（给 core 一个真实的翻译单元；`add_library(STATIC)` 不接受零源文件）

**Interfaces:**
- Produces: `SR_TEST(name)`（函数体自动注册）、`SR_ASSERT(cond)`、`SR_ASSERT_NEAR(a,b,tol)`、**`SR_ASSERT_EQ(a,b)`**（整型/枚举的 `==`，失败时打印两个值）、`SR_ASSERT_FALSE(cond)`；`srtest::runAll() -> int`（返回失败数，0 = 全绿）。这四个宏**在本任务一次给齐**——后面每个测试文件都直接依赖它们，不要在别的任务里"顺手补"。CMake 目标名 `soft_raster_core` / `soft-raster` / `soft_raster_tests`，CTest 测试名 `unit`。

- [ ] **Step 1: 写 `CMakeLists.txt`**

要求：`cmake_minimum_required(VERSION 3.24)`；`project(soft_raster LANGUAGES CXX)`；`CMAKE_CXX_STANDARD 20` 且 `EXTENSIONS OFF`；`option(SR_BUILD_APP "build the Win32 app target" ON)`（g++ 侧传 `-DSR_BUILD_APP=OFF`，这是 C1′ 的落地方式）；core 用 `add_library(soft_raster_core STATIC ...)` 且 `target_include_directories(soft_raster_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)`；MSVC 时 `target_compile_options(soft_raster_core PRIVATE /W4 /utf-8 /MP)`，GCC 时 `-Wall -Wextra -ffp-contract=off -Wno-unknown-pragmas`；`add_executable(soft_raster_tests ...)` 链接 core，`enable_testing()` + `add_test(NAME unit COMMAND soft_raster_tests)`；app 目标只在 `SR_BUILD_APP AND WIN32` 时创建，链 `user32 gdi32`。**不出现任何 `find_package`（C3）。**

- [ ] **Step 2: 写 harness**

`tests/harness.h` 的形状（这是唯一一处需要固定的宏接口，后面所有测试文件都依赖它）：

```cpp
namespace srtest {
using Fn = void (*)();
std::vector<std::pair<const char*, Fn>>& suite();
struct Register { Register(const char* name, Fn fn); };   // 追加进 suite()
extern int currentFailures;                                // 每个用例开始时清零
int runAll();                                              // 逐个调用，打印 "FAIL <name>: file:line expr"，返回失败用例数
}
#define SR_TEST(name)  static void name(); static srtest::Register reg_##name(#name, name); static void name()
#define SR_ASSERT(cond)            /* 记录 file/line/cond，currentFailures++ */
#define SR_ASSERT_NEAR(a, b, tol)  /* fabs((a)-(b)) <= (tol)，否则连同两个值一起打印 */
```

自注册靠静态初始化，所以测试 `.cpp` 必须以**静态库或强制符号引用**的方式进入可执行文件——用 `add_executable(soft_raster_tests ...)` 直接把每个 `tests/unit/*.cpp` 列入源文件（**不要**先做成静态库，否则链接器会丢掉没人引用的 TU）。计划里每个新测试文件都要同步加进 CMake 的源文件列表。

- [ ] **Step 3: 写 `src/version.h/.cpp` + 测试**

`const char* sr::buildInfoString();` 返回一行含项目名、`CMake` 传入的 `SR_VERSION`、`DEBUG/RELEASE`、编译器（`_MSC_VER` / `__GNUC__`）的字符串。用途不是装饰：HUD 与 notes 里的每张性能表、每个 golden 都要记下这一行，否则 M8/M9 的数字无法归因到某个构建配置。

```cpp
SR_TEST(harness_detects_a_passing_assert) { SR_ASSERT(1 + 1 == 2); }
SR_TEST(harness_near_tolerance_is_inclusive) { SR_ASSERT_NEAR(1.0f, 1.000001f, 1e-5f); }
SR_TEST(build_info_string_is_nonempty) { SR_ASSERT(sr::buildInfoString()[0] != '\0'); }
```

- [ ] **Step 4: 配置并构建，跑测试**

Run: `cmake -S . -B build && cmake --build build --config Debug && ctest --test-dir build -C Debug --output-on-failure`
Expected: `100% tests passed, 0 tests failed out of 1`（CTest 层面只有 `unit` 一个测试，它内部跑若干用例）

- [ ] **Step 5: 验证 harness 真能报错**（不验证失败路径的 harness 等于没有 harness）

临时把 `1 + 1 == 2` 改成 `1 + 1 == 3`，重建并跑，Expected: `The following tests FAILED: 1 - unit`，且输出里有一行含 `FAIL harness_detects_a_passing_assert` 与文件名行号。改回来再跑，Expected: 全绿。

- [ ] **Step 6: g++ 侧配置+编译（C1′）**

Run: `cmake -S . -B build-gcc -DSR_BUILD_APP=OFF -DCMAKE_BUILD_TYPE=Debug && cmake --build build-gcc && ctest --test-dir build-gcc --output-on-failure`
Expected: 编译通过、`100% tests passed`。若 `make`/`msys-2.0.dll` 缺失导致 CTest 起不来，记录到 `docs/notes/M0.md` 并把 g++ 侧验证降级为"编译通过"，不要为此加 PATH 黑魔法。

- [ ] **Step 7: Commit** `git commit -am "chore: cmake targets + minimal test harness"`

### Task 0.3: 线性↔sRGB 编解码 + 像素打包（Review Focus 5 的前半）

放在 `ColorTarget` 之前，因为测试图案（0.4）需要 `encodeSRGB`；反过来 `ColorTarget` 不需要颜色概念。

**Files:**
- Create: `src/render/color.h`, `src/render/color.cpp`
- Test: `tests/unit/test_color.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace sr {
  struct LinearColor { float r{}, g{}, b{}; };                   // 线性空间，分量 ∈ [0,1]
  uint32_t packBGRA(uint8_t r, uint8_t g, uint8_t b, uint8_t a);  // → 0xAARRGGBB（内存字节序 B,G,R,A）
  uint8_t  linearToSrgb8(float v);                                // clamp [0,1] 后编码
  float    srgb8ToLinear(uint8_t v);
  uint32_t encodeSRGB(const LinearColor&, float alpha = 1.0f);    // → 0xAARRGGBB，走 LUT
  LinearColor decodeSRGB(uint32_t packed);
  }
  ```
- Consumes: 无（本任务是第一个"有值语义"的单元）

- [ ] **Step 1: 写失败测试**

```cpp
SR_TEST(srgb_encode_endpoints_and_midpoint) {
    SR_ASSERT_EQ(linearToSrgb8(0.0f), 0);
    SR_ASSERT_EQ(linearToSrgb8(1.0f), 255);
    SR_ASSERT(linearToSrgb8(0.5f) >= 186 && linearToSrgb8(0.5f) <= 188);   // Review Focus 5
    SR_ASSERT(linearToSrgb8(0.0031308f) <= 16);   // 线性段与幂段接缝附近不跳变
}
SR_TEST(srgb_is_monotonic_and_invertible) {
    uint8_t prev = 0;
    for (int i = 0; i <= 255; ++i) { uint8_t e = linearToSrgb8(i/255.f); SR_ASSERT(e >= prev); prev = e; }
    for (int i = 0; i <= 255; ++i) SR_ASSERT_EQ(linearToSrgb8(srgb8ToLinear((uint8_t)i)), (uint8_t)i);
}
SR_TEST(encodeSRGB_packs_as_0AARRGGBB) {
    SR_ASSERT_EQ(encodeSRGB({1,0,0}, 1.0f), 0xFFFF0000u);   // 纯红 = A:FF R:FF G:00 B:00
    SR_ASSERT_EQ(encodeSRGB({0,0,1}, 1.0f), 0xFF0000FFu);   // 纯蓝 = A:FF R:00 G:00 B:FF
    SR_ASSERT_EQ(packBGRA(0x11,0x22,0x33,0xFF), 0xFF112233u);  // 名字是内存序，值是 little-endian 序
}
```
（`packBGRA` 的这条断言容易被写反，所以单独 pin：参数按 r,g,b 给，返回值按 `0xAARRGGBB` 读。
**计划初稿在这里把红与蓝的期望值写反了**——`0xFF0000FF` 是蓝不是红；实施时更正，理由见 ledger Ruling 7。）

- [ ] **Step 2: 跑，确认失败** Expected: 编译错误 `color.h: no member 'encodeSRGB'`（或文件不存在）

- [ ] **Step 3: 实现**

采用 IEC 61966-2-1 分段定义（`v <= 0.0031308 ? 12.92v : 1.055*pow(v,1/2.4)-0.055`），并建 **4096 项（12bit 输入域）LUT** 供热路径使用。
**为什么不是 256 项**：输出本来就是 8bit，但把 LUT 索引也量化到 8bit 会做两次量化，暗部会出现 2~3 LSB 偏差，平滑渐变上看得到带状条纹；索引到 12bit 之后误差锁在 1 LSB 内。
因此对解析式的断言必须是"≤1 LSB"而不是"逐索引相等"（生产用 float pow、参照用 double pow，落在 .5 边界的值必然差一格）。gamma 的**语义**由端点、单调、往返三条测试守住，不靠这一条。
越界输入 clamp 到 [0,1]；NaN 也要返回一个合法字节（定死为 0，用 `!(v > 0.0f)` 而不是 `v < 0.0f` 来写这个 clamp）。

- [ ] **Step 4: 跑，全绿** Expected: `100% tests passed`

- [ ] **Step 5: Commit** `feat: linear<->sRGB encode/decode with LUT, 0xAARRGGBB packing`

### Task 0.4: `ColorTarget` / `DepthTarget` + 测试图案

**Files:**
- Create: `src/render/render_target.h`, `src/render/render_target.cpp`
- Create: `src/render/patterns.h`, `src/render/patterns.cpp`
- Test: `tests/unit/test_render_target.cpp`, `tests/unit/test_patterns.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace sr {
  struct ColorTarget {                 // 非拥有视图；top-down（C8）；pitch 是任意正字节数
      uint32_t* pixels = nullptr; int width = 0, height = 0, pitch = 0;
      void clear(uint32_t packed);
      void writePacked(int x, int y, uint32_t packed);        // 越界静默丢弃
      uint32_t readPacked(int x, int y) const;                // 越界返回 0
      bool contains(int x, int y) const;
  };
  struct DepthTarget {                 // 同一份 float 缓冲，支持子矩形 view（Backend B 的无锁基础，§7.2）
      float* values = nullptr; int width = 0, height = 0;
      void clear(float v = 1.0f);
      void write(int x, int y, float d);
      float  read(int x, int y) const;                        // 越界返回 +Infinity
      DepthTarget view(int x0, int y0, int w, int h) const;
  };
  struct RenderTarget { ColorTarget color; DepthTarget depth; };
  void fillLinearGradient(ColorTarget&, const LinearColor& a, const LinearColor& b, float phase);
  void fillChecker(ColorTarget&, int cellPx);
  }
  ```
  `writePacked` 之后 `readPacked(x,y)` 必须返回同一个值（这一条把 pitch/行序定死）。`DepthTarget::read` 越界返回 `+inf`：这样 tile 边界外的深度测试必然失败，而不是碰巧通过。
- Consumes: `LinearColor`、`encodeSRGB`（0.3）

- [ ] **Step 1: 写失败测试 `test_render_target.cpp`**

```cpp
SR_TEST(color_target_roundtrip_with_nontrivial_pitch) {
    // 64x32，pitch = 64*4 + 16（故意不连续）
    std::vector<uint8_t> mem((64*4+16)*32);
    ColorTarget t{ reinterpret_cast<uint32_t*>(mem.data()), 64, 32, 64*4+16 };
    t.clear(0xFF112233u);
    SR_ASSERT_EQ(t.readPacked(0,0), 0xFF112233u);
    SR_ASSERT_EQ(t.readPacked(63,31), 0xFF112233u);
    t.writePacked(7, 5, 0xDEADBEEFu);
    SR_ASSERT_EQ(t.readPacked(7,5), 0xDEADBEEFu);
    SR_ASSERT_EQ(t.readPacked(8,5), 0xFF112233u);      // 邻居没被踩
    t.writePacked(-1, 0, 0u); t.writePacked(0, 32, 0u); // 越界不崩、不改内存
    SR_ASSERT_EQ(t.readPacked(0,0), 0xFF112233u);
}
SR_TEST(depth_target_view_aliases_the_parent_buffer) {
    std::vector<float> buf(16*16, 1.0f);
    DepthTarget d{ buf.data(), 16, 16 };
    auto sub = d.view(4, 4, 8, 8);
    sub.write(0, 0, 0.25f);                      // 子矩形的 (0,0) == 全局 (4,4)
    SR_ASSERT_NEAR(d.read(4,4), 0.25f, 1e-6f);
    SR_ASSERT_NEAR(d.read(5,4), 1.0f, 1e-6f);
    SR_ASSERT_EQ(sub.width, 8); SR_ASSERT_EQ(sub.height, 8);
}
SR_TEST(top_down_row_zero_is_the_top_edge) {
    // C8：第 0 行是上边缘 ⇒ 行 1 的地址比行 0 大一个 pitch（不是小）
    ColorTarget t{ mem.data(), 8, 2, 8*4 };
    t.writePacked(0, 0, 0xFF000000u);                 // 只有"上边缘"被写
    t.writePacked(0, 1, 0xFF0000FFu);                 // 下边缘写成蓝，便于反向验证
    auto* p0 = t.pixels;  auto* p1 = t.pixels + t.pitch/4;
    SR_ASSERT_EQ(p0[0], 0xFF000000u);                 // pixels[0] 就是第 0 行 ⇒ 内存起点即上边缘
    SR_ASSERT_EQ(p1[0], 0xFF0000FFu);
}
```
（`SR_ASSERT_EQ` 已在 0.2 提供，本任务不再动 harness。）

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build --config Debug`
Expected: **编译失败**，`ColorTarget` 未定义（这是预期的"红"，不是 harness 坏了）

- [ ] **Step 3: 实现 `render_target.cpp`**

要点：`pixels[(size_t)y * (pitch/4) + x]`；`view()` 靠偏移指针 + 收窄宽高实现，绝不分配内存；`view()` 的 `x0+w > width || y0+h > height` 时断言失败（Debug）并把范围夹住（Release），因为 M8 的 tile 网格除法一旦越界会静默写坏邻居 tile。

- [ ] **Step 4: 写并实现 `patterns.cpp` + `test_patterns.cpp`**

断言：`fillChecker(…, 2)` 在 4×4 目标上，`(0,0)` 与 `(2,2)` 同色、`(2,0)` 与另一种；`fillLinearGradient(a,b,phase=0)` 在 `x=0` 处（容差内）等于 `encodeSRGB(a)`、在 `x=width-1` 处等于 `encodeSRGB(b)`；`phase` 让渐变沿 x 平移且保持连续（相邻列差值的绝对值 ≤ 2×单列步长，防止出现台阶）。

- [ ] **Step 5: 跑测试全绿** Expected: `100% tests passed`

- [ ] **Step 6: 确认 core 不含 Win32（C4）**

Run: `grep -rn "windows.h\|Windows.h\|__stdcall\|HWND" src/render src/math || echo CLEAN`
Expected: `CLEAN`。把这条命令写进 `docs/notes/M0.md` 的"每次加完代码都跑一遍"清单。

- [ ] **Step 7: Commit** `git commit -am "feat: ColorTarget/DepthTarget with sub-rect views + test patterns"`

### Task 0.5: BMP 写出（截图 + golden 的地基）

**Files:**
- Create: `src/loader/bmp_writer.h`, `src/loader/bmp_writer.cpp`
- Test: `tests/unit/test_bmp.cpp`

**Interfaces:**
- Consumes: `ColorTarget`（0.4）；`decodeSRGB` 不需要（BMP 存 sRGB 编码后的 8bit，直接拆 `ColorTarget` 的字节）
- Produces: `bool writeBmp(const std::string& path, const ColorTarget&, std::string& err);`（24bpp，无压缩，行 4 字节对齐，自底向上存储）

- [ ] **Step 1: 写失败测试**

```cpp
SR_TEST(bmp_header_fields) {
    // 5x3 目标 → 写入临时文件，读回字节
    // 期望：'B''M'；fileSize == 14+40+ (5*3+pad)*3 其中每行 pad = (4 - (15)%4)%4 = 1
    SR_ASSERT_EQ(buf[0],'B'); SR_ASSERT_EQ(buf[1],'M');
    SR_ASSERT_EQ(le32(buf+2), expectedFileSize);
    SR_ASSERT_EQ(le32(buf+10), 54);              // 像素数据起始
    SR_ASSERT_EQ(le32(buf+18), 5);               // width
    SR_ASSERT_EQ(le32(buf+22), 3);               // height，正值 = 自底向上
    SR_ASSERT_EQ(le16(buf+28), 24); SR_ASSERT_EQ(le32(buf+30), 0);  // bpp, BI_RGB
}
SR_TEST(bmp_first_data_row_is_the_bottom_edge) {   // Review Focus 5 的第二处翻转
    // 2x2 目标，top-down 行内容分别为红/绿；BMP 数据的第 0 行必须是绿那一行
}
SR_TEST(bmp_rejects_a_target_with_unsupported_pitch) {
    // pitch != width*4 时返回 false 且 err 非空（截图路径先不支持带 stride 的目标；
    // DIB 的 pitch 恰为 width*4，见 0.6）
}
```

- [ ] **Step 2: 跑，确认失败** Expected: 编译错误

- [ ] **Step 3: 实现**（顺序：`BITMAPFILEHEADER` 14 字节 + `INFOHEADER` 40 字节，全部小端手写，不 `#pragma pack`）

- [ ] **Step 4: 跑，全绿** Expected: `100% tests passed`

- [ ] **Step 5: 肉眼验证一次** Run: 在 `test_bmp.cpp` 里临时 `fillChecker(t,4)` + `writeBmp("tmp.bmp", t, err)`，用画图/VS 打开。Expected: 棋盘格，**正立**（不是上下颠倒）。验证完删掉临时写出（或留成 `SR_TMP_BMP` 环境变量开关）。

- [ ] **Step 6: Commit** `git commit -am "feat: 24bpp BMP writer for screenshots and golden"`

### Task 0.6: Win32 窗口 + DIB + 帧循环 + 离屏路径

**Files:**
- Create: `src/app/surface.h`, `src/app/surface.cpp`
- Create: `src/app/dib.h`, `src/app/dib.cpp`
- Create: `src/app/win32_window.h`, `src/app/win32_window.cpp`
- Create: `src/app/cmdline.h`, `src/app/cmdline.cpp`
- Create: `src/app/main.cpp`
- Modify: `CMakeLists.txt`（app 源文件）

**Interfaces:**
- Consumes: `ColorTarget`, `DepthTarget`, `RenderTarget`, `patterns`, `writeBmp`, `packBGRA`
- Produces:
  ```cpp
  namespace app {
  struct AppOptions { std::string obj, out; int width=1280, height=720;
                      bool offscreen=false; int frames=1; bool noFit=false;
                      std::string backend="a"; int threads=0; int bench=0; };
  bool parseCmdline(int argc, char** argv, AppOptions&, std::string& err);

  class Surface {              // 唯一的像素出口；拥有像素内存
  public:  virtual ~Surface();
           sr::RenderTarget& target() { return m_target; }
           virtual bool present() = 0;            // WindowSurface: BitBlt；Offscreen: no-op
  protected:
           sr::RenderTarget m_target{};
  };
  Surface* createWindowSurface(int w, int h, const wchar_t* title);
  Surface* createOffscreenSurface(int w, int h);

  struct InputState { float dx, dy; float wheel; bool lmb, rmb; bool paused;
                      int frameStep; std::vector<std::function<void()>> actions; };
  class Window { public: bool pump(InputState&); bool shouldClose() const; ... };
  }}
  ```

- [ ] **Step 1: `cmdline` 先写测试再实现**（`parseCmdline` 是纯函数，放进 core 依赖之外但**测试它**）

Test: `tests/unit/test_cmdline.cpp` — 默认值（1280×720、backend "a"、offscreen false）；`--width 640 --height 360` 生效；`--offscreen --frames 3 --out x.bmp`；未知 flag 返回 false 且 err 含该 flag 名；`--width` 缺值返回 false；`--threads -1` 返回 false。**注意**：`src/app/cmdline.cpp` 要能被测试目标链接——把它的实现放进 `cmdline.cpp` 且**不 include 任何 Win32 头**，并在 CMake 里把 `src/app/cmdline.cpp` 单独列入 `soft_raster_tests` 的源文件（这是 C4 的例外处理，`cmdline` 本身平台无关）。断言：`grep windows.h src/app/cmdline.*` 为空。

- [ ] **Step 2: 实现 `dib.cpp`（唯一的 y 翻转点）**

```
CreateDIBSection(hdc, &bi /* biHeight 为负 ⇒ top-down */, DIB_RGB_COLORS, &bits, nullptr, 0)
```
**决定：用负 `biHeight` 拿到 top-down DIB**，于是 `ColorTarget::pitch == width*4` 且行序与 core 约定一致，`dib.cpp` 里**不需要**行翻转（比"正 `biHeight` + 手动翻行"少一个错误来源，也更简单）。这不影响 0.5 的 BMP 翻转测试：BMP 文件本身自底向上，那是 `bmp_writer` 的职责，与 DIB 无关。把这个选择与理由写进 `docs/notes/M0.md`——"翻转点只有一个"是 C8 的实质。同时 `biAlpha` 留 0、`bmiColors` 清空（`BITMAPINFO` 定长 256 项的坑：用 `char[sizeof(BITMAPINFOHEADER)+3*sizeof(RGBQUAD)]` 的栈缓冲），并确认 DIB 的字节是 B,G,R + 未用字节 A（GDI 忽略 A，但我们仍然每帧写 `0xFF` 以便 BMP/golden 稳定）。

- [ ] **Step 3: 实现 `Window` + 消息循环**

`WndProc`：`WM_DESTROY` → PostQuitMessage；`WM_LBUTTONDOWN/UP`、`WM_MOUSEMOVE`（`SetCapture` 拖拽）、`WM_MOUSEWHEEL`、`WM_KEYDOWN`（先只处理 `Esc` 关闭 + `P` 暂停 + `S` 截图，其余键在 M7 补）。帧循环用 `PeekMessageW` + 空队列时渲染一帧；计时用 `QueryPerformanceCounter`。窗口不可缩放（无 `WS_THICKFRAME`，`WM_GETMINMAXINFO` 不处理）——对应 spec §7.6。

- [ ] **Step 4: 实现 `main.cpp` 的两条路径**

```
offscreen:  分配 w*h*4 + w*h*float → RenderTarget → 循环 frames 次 { fillLinearGradient; } → writeBmp(out) → exit 0
window:     createWindowSurface → while(!shouldClose) { pump; if(!paused||frameStep){ phase += dt; fillLinearGradient } present; 画 FPS 文字 }
```
Expected（手动验证，不可单测）：窗口 640×360 出现平滑移动的渐变，左上角文字显示 `FPS 59.8`（数字会动）；`S` 键存 BMP；`P` 键停住画面；`Esc` 退出。**这一条是 M0 的真正验收，必须亲眼看过再提交。**

- [ ] **Step 5: 验证离屏路径**

Run: `./build/Debug/soft-raster.exe --offscreen --frames 1 --width 200 --height 100 --out shot.bmp`
Expected: 退出码 0，`shot.bmp` 存在且 200×100、正立（用画图看）。

- [ ] **Step 6: Commit** `git commit -am "feat: Win32 DIB surface, frame loop, offscreen render path"`

### Task 0.7: 自绘位图字体（统计文字，不用 GDI 字体 API）

**Files:**
- Create: `src/app/overlay_font.h`, `src/app/overlay_font.cpp`
- Test: 无独立单测目标（app 层）；改为把 glyph 数据表放 `src/render/overlay_glyphs.h`（core 内，可单测）

**Interfaces:**
- Produces: `void drawText(sr::ColorTarget&, int x, int y, const char* text, uint32_t packedFg, uint32_t packedBg /* 0 = 透明 */);` 与 `int textWidthPx(const char* text, int scale=1);`
- Consumes: `ColorTarget::writePacked`

- [ ] **Step 1: 写失败测试 `tests/unit/test_overlay_glyphs.cpp`**

断言：`glyphAt('0').rows` 共 8 行、每行 `uint8_t`；`'A'..'Z'`、`'0'..'9'`、`" .:-=|%/\\FPSms(),<>"` 全部非空且 `glyphAt(c).rows[0..7]` 至少一行 != 0；`glyphAt('a')` == `glyphAt('A')`（只有大写，M0 够用）；`glyphAt('\0')` 返回空 glyph（不崩）。

- [ ] **Step 2: 跑，确认失败** → **Step 3: 实现** 8×8 表，ASCII 32..126 手填（或用 `glyphAt(c)` 查 `static constexpr std::array`）。

- [ ] **Step 4: 跑，全绿**

- [ ] **Step 5: 接到 M0 的 HUD 上并亲眼验证** Expected: 窗口左上角文字清晰、无锯齿错位；`FPS` 数字变化时不残留鬼影（每帧重画同一片区域即可）。

- [ ] **Step 6: Commit** `git commit -am "feat: 8x8 bitmap overlay font drawn through ColorTarget"`

### Task 0.8: M0 收尾（notes + README）

- [ ] `docs/notes/M0.md`：为什么唯一依赖是 `ColorTarget::pitch`；为什么选负 `biHeight`（翻转点归一）；`BITMAPINFO` 栈缓冲的坑；自绘字体的决定（像素全由我们算）；本里程碑实际遇到的 bug 与定位过程。
- [ ] `README.md` 里程碑表 M0 打勾 + 附截图（把 `shot.bmp` 转成 PNG 或直接把 BMP 提交进 `docs/notes/img/`）。
- [ ] 跑全套验证：`ctest` 全绿（MSVC 与 g++ 两侧）、`grep` C4 检查为 CLEAN。
- [ ] Commit + push：`docs: M0 notes + README progress`

---

## M1：数学库 + Backend A 的标量光栅器

工作成果：屏幕上有一个**纯色三角形**，顶点是手工给定的屏幕坐标。还没有相机、没有深度、没有插值。

### Task 1.1: `Vec2/Vec3/Vec4` + 运算

**Files:** Create `src/math/vec.h`；Modify `src/render/color.h`（`LinearColor` 保持独立 struct，不改成 `Vec3` 别名——理由：`encodeSRGB(LinearColor)` 的签名可读性优先，写进 notes）
**Test:** `tests/unit/test_vec.cpp`
**Produces:**
```cpp
namespace sr {
struct Vec2 { float x{}, y{}; };
struct Vec3 { float x{}, y{}, z{}; };
struct Vec4 { float x{}, y{}, z{}, w{}; };
// 自由函数：operator+ - * /（分量与标量）、dot/cross/length/lengthSquared
Vec3 normalize(Vec3);          // 约定：长度 < 1e-8 时返回 {0,0,0}（不返回 NaN）
Vec3 lerp(Vec3 a, Vec3 b, float t);
Vec2 lerp(Vec2, Vec2, float);  Vec4 lerp(Vec4, Vec4, float);
Vec4 makeVec4(Vec3, float w);
}
```
- [ ] **Step 1: 写失败测试** — 断言：`cross({1,0,0},{0,1,0}) == {0,0,1}`；`dot` 已知值；`normalize({3,0,0}) == {1,0,0}`；`normalize({0,0,0}) == {0,0,0}`（**pin 零向量约定**，否则热路径会出现静默 NaN）；`lerp(a,b,0)==a`、`lerp(a,b,1)==b`、`lerp(a,b,0.5)` 中点；`length({3,4,0}) == 5`（容差 1e-6）。
- [ ] **Step 2: 跑，确认失败** Expected: 编译错误 `vec.h: no member 'cross'`
- [ ] **Step 3: 实现**（全 `inline`，头文件 only）
- [ ] **Step 4: 跑，全绿**
- [ ] **Step 5: Commit** `feat: vector math with a documented zero-length convention`

### Task 1.2: `Mat4`

**Files:** Create `src/math/mat.h`
**Test:** `tests/unit/test_mat.cpp`
**Produces:**
```cpp
namespace sr {
struct Mat4 { float m[4][4]{};               // row-major；m[row][col]
    static Mat4 identity();
    static bool tryInvert(const Mat4& in, Mat4& out);   // 一般 4x4，余子式法；行列式 |det|<1e-8 → false
    Mat4 operator*(const Mat4&) const;        // 矩阵乘
    Vec4 operator*(const Vec4&) const;        // 列向量右乘
    Mat4 transposed() const;
    Mat4 upper3x3AsMat4() const;              // 法线矩阵的基底
}}
```
- [ ] **Step 1: 写失败测试** — 断言：`identity()*identity()==identity()`；一个手工 4×4 乘积的全部 16 项；`m*v` 对第 4 个基向量的映射（`m*{0,0,0,1}` 等于最后一列）；`tryInvert(A, inv)` 后 `A*inv ≈ I`（容差 1e-4，用平移+旋转+非均匀缩放矩阵）；**奇异矩阵**（第三行 = 第一行 ×2）`tryInvert` 返回 false 且 `out` 不被写成垃圾（保持被调用前的值或写 identity，二选一并 pin 进注释）；`transposed()` 是元素转置。
- [ ] **Step 2: 跑，确认失败** → **Step 3: 实现** → **Step 4: 全绿**
- [ ] **Step 5: Commit** `feat: 4x4 matrix with invertible-check`

### Task 1.3: 相机/投影构造

**Files:** Create `src/math/transform.h`（含实现，header-only）
**Test:** `tests/unit/test_transform.cpp`
**Produces:**
```cpp
namespace sr {
// 约定（写进注释，测试据此 pin）：右手系、OpenGL 风格；
// makePerspective 产出 clip.z ∈ [-w, w]（NDC z ∈ [-1,1]），Viewport 阶段再映到 [0,1]（§7.1）
Mat4 makePerspective(float fovYRadians, float aspect, float nearZ, float farZ);
Mat4 makeLookAt(Vec3 eye, Vec3 target, Vec3 up);
Mat4 makeTranslate(Vec3); Mat4 makeRotateX/Y/Z(float rad); Mat4 makeScale(Vec3);
}
```
- [ ] **Step 1: 写失败测试** — 断言：`makeLookAt({0,0,5},{0,0,0},{0,1,0})` 把 `{0,0,5}` 映到 `{0,0,0}`（eye→原点）、把 `{0,0,0}` 映到 `{0,0,-5}`（沿 -z 看）；`{0,1,0}` 仍在 `{0,1,0}`（up 保 y）；`makePerspective(90°,1,1,10)` 把 NDC 边界点 `{0,0,-1}`（即 z=-1 近平面中心）映后满足 `clip.z == -clip.w`、远平面中心 `z=-10` 满足 `clip.z == +clip.w`（**这就是 NDC z∈[-1,1]**）；`aspect=2` 时 x 方向范围是 y 的一半；`fovY → 大` 时 `m[1][1]` 单调变小；`makeRotateZ(90°)` 把 `{1,0,0}` 映到 `{0,1,0}`。
- [ ] **Step 2–5:** 同上（红 → 实现 → 绿 → commit）
- [ ] **Step 6: Commit** `feat: perspective/lookAt/rigid transforms with NDC-z[-1,1] pinned by tests`

### Task 1.4: `PlaneEq` / `EdgeFunc` + Backend A 光栅器（flat color）

**Files:** Create `src/render/plane_eq.h`, `src/render/plane_eq.cpp`, `src/render/raster_scalar.h`, `src/render/raster_scalar.cpp`, `src/render/triangle.h`
**Test:** `tests/unit/test_raster_scalar.cpp`
**Consumes:** `ColorTarget`（0.4）、`Vec*`（1.1）
**Produces:**
```cpp
namespace sr {
struct EdgeFunc { Vec2 p0; Vec2 n; float c;   // p0 = 边的起点，n = 边向量的"顺时针法线"，c 使内部为正
    float evaluate(const Vec2& p) const;                       // n·(p - p0) + c
    bool  covers(const Vec2& p, bool edgeIsTopOrLeft) const;   // evaluate>0，或 ==0 且 edgeIsTopOrLeft
    bool  edgeIsTopOrLeft() const;                             // 由 n 的分量符号推出（见 Step 3 说明）
};
struct PlaneEq  { float n0{}, n1{}, c{};      // f(x,y) = n0*x + n1*y + c
    float sample(float x, float y) const; };
struct FlatTriangle { Vec2 v[3]; uint32_t packedColor; };
struct RasterStats  { int64_t pixelsCovered = 0; int64_t pixelsWritten = 0; int64_t trianglesRejected = 0; };
void rasterizeFlatTriangle(ColorTarget&, const FlatTriangle&, RasterStats* = nullptr);
}
```
**为什么把 `PlaneEq` 与 `EdgeFunc` 放一个头**：两者都由三角形的三个屏幕点一次算出，共享同一个"顺时针法线"推导；分两个文件会让推导在注释里重复一遍（A1）。

- [ ] **Step 1: 写失败测试**（这一任务的关键测试，覆盖 Review Focus 4）

```cpp
SR_TEST(triangle_covers_its_three_vertices)      // 在三个顶点像素中心采样，必须都被写
SR_TEST(a_point_outside_is_not_written)          // 重心明显为负的点
SR_TEST(degenerate_triangle_writes_nothing)      // 共线三点 → pixelsWritten == 0，trianglesRejected == 1
// 4x4 目标；两个三角形铺满 4x4 像素（顶点在像素中心 0.5/4.5），共享一条对角线。
// 期望：16 个像素中心全部被写，且**每个恰好被写一次**。
// 两个劈分方向都测：如果只有一方向失败，是 top/left 规则的符号错（Review Focus 4）。
SR_TEST(square_split_on_diagonal_has_no_gap_and_no_overlap) {
    // 三角形 = (0.5,0.5)-(4.5,0.5)-(4.5,4.5)  与  (0.5,0.5)-(4.5,4.5)-(0.5,4.5)
    for (int y=0;y<4;++y) for (int x=0;x<4;++x) SR_ASSERT_EQ(writes[x][y], 1);
}
SR_TEST(square_split_on_the_other_diagonal_has_no_gap_and_no_overlap) {
    // 三角形 = (0.5,0.5)-(4.5,0.5)-(0.5,4.5)  与  (4.5,0.5)-(4.5,4.5)-(0.5,4.5)
    for (int y=0;y<4;++y) for (int x=0;x<4;++x) SR_ASSERT_EQ(writes[x][y], 1);
}
SR_TEST(triangle_fully_outside_target_is_clipped_by_bbox)  // extent 夹到 target 外 → 0 写入，不越界内存
SR_TEST(plane_eq_reproduces_vertex_values) {
    // 属性 (10, 20, 30) 在三顶点，PlaneEq 由本任务先实现"三值定面"，
    // 在三个顶点坐标处 sample() 应（容差 1e-4）返回 10/20/30
}
```

- [ ] **Step 2: 跑，确认失败** Expected: 编译错误

- [ ] **Step 3: 实现光栅器**

算法（这是签名与测试都未唯一决定的部分）：包围盒 → 逐像素中心 `(x+0.5, y+0.5)` → 三个 `EdgeFunc` 求值 → 全为正则覆盖 → 写 `packedColor`。`edgeIsTopOrLeft()` 用"第三个顶点相对这条边的位置"表达，**不要**用 `e.x/e.y` 的符号直接判——那会把规则与绕向绑死，而 top-down（C8）已经让绕向与教科书相反，这是最容易写错并且症状正好是 Review Focus 4 的裂缝/双写的地方：
- **顶边**：边水平（`e.y == 0`）**且**第三顶点在它下方（top-down 里即 `p2.y > p0.y`）。这条与绕向无关，可以放心写。
- **左边**：边不水平（`p0.y != p1.y`）**且** `p2.x < p0.x || p2.x < p1.x`（第三顶点比边的至少一个端点更靠左 ⇒ 这条边构成三角形的右轮廓… **注意**：这条规则的标准写法来自 y 朝上的 CCW 约定，我们的空间是 y 朝下、正面为 CW。实现时**两条对角线方向都要测**，见 Step 1 的 `square_split_on_diagonal` 与 `square_split_on_the_other_diagonal`；如果只有其中一个方向失败，问题一定在这条规则的符号上，而不是容差）。**必须**在 `raster_scalar.cpp` 顶部用中文写清 fill rule 的推导与"为什么共用边恰好被一个三角形拥有"（G1/A1）。`RasterStats::pixelsWritten` 用于 M8 与 backend 一致性对比。

- [ ] **Step 4: 跑，全绿** Expected: `100% tests passed`（含那个 4×4 的 gap/overlap 测试）

- [ ] **Step 5: 手工扩到 M0 的 app**：`main.cpp` 里每帧 `clear` + `rasterizeFlatTriangle`（一个随 `phase` 变形的三角形），按键改顶点。**注意**：app 每帧重建三角形是临时的，M2 会换成 Mesh。Expected: 窗口里有一个实心三角形，拖动/按键能改变形状，边缘无锯齿状裂缝。

- [ ] **Step 6: Commit** `feat: scalar barycentric/edge-function rasterizer (Backend A, flat color)`

### Task 1.5: M1 收尾

- [ ] `docs/notes/M1.md`：`EdgeFunc` 顺时针法线推导；top-left fill rule 为什么必须存在（否则共享边双写 → overdraw 统计失真 + 半透明处可见裂缝）；`PlaneEq` 从三顶点解两元一次方程的推导与"分母为零=退化"的关系；`square_split_on_diagonal` 这条测试为什么比"手算 mask"更好。
- [ ] README 进度更新 + 图。Commit + push。

---

## M2：完整几何阶段 —— 从 Mesh 到屏幕

工作成果：一个 MVP 变换的**旋转立方体**，面纯色，**故意不启用深度**（穿帮，用来讲 painter's algorithm 的不足）。六面裁剪、背面剔除、`depthTest` 标志位在此落地。

### Task 2.1: 顶点/管线类型与后端接口

**Files:** Create `src/render/vertex.h`, `src/render/pipeline.h`, `src/render/render_backend.h`, `src/render/stats.h`, `src/math/bbox.h`, `src/scene/mesh.h`, `src/scene/mesh.cpp`
**Test:** `tests/unit/test_mesh.cpp`, `tests/unit/test_bbox.cpp`
**Produces:**
```cpp
namespace sr {
// bbox.h —— BBox 与 Rect 都在这里，因为它们没有依赖且被 3 个以上模块用到
struct BBox { Vec3 min{+1e30f}, max{-1e30f};
    void grow(const Vec3&); void grow(const BBox&); bool empty() const;
    Vec3 center() const; float radius() const; static BBox fromPoints(const std::vector<Vec3>&); };
struct Rect { int x0{}, y0{}, x1{}, y1{};                   // 闭区间 [x0,x1]
    static Rect clamp(const Rect&, int width, int height);  // 夹到 [0,w-1]×[0,h-1]；空时 x1<x0
    bool empty() const { return x1 < x0 || y1 < y0; }
    int width() const { return x1 - x0 + 1; } int height() const { return y1 - y0 + 1; } };

struct Varyings {                    // 固定 4 槽的 varying 集合（刻意不做通用 attribute 表；理由进 notes）
    Vec4 color{};                    // 顶点色（线性空间）
    Vec3 worldNormal{}; Vec2 uv{}; Vec3 worldPos{};
};
inline constexpr int kVaryingFloatCount = 12;   // color(4)+worldNormal(3)+uv(2)+worldPos(3)
static_assert(sizeof(Varyings) == kVaryingFloatCount * sizeof(float));  // Varyings 必须全 float 无填充
const float* varyingFloats(const Varyings&);    // 逐 float 访问 PlaneEq 数组的入口（不做 union/ memcpy 技巧）
const char*  varyingFieldName(int i);           // "color.x"/"uv.y"…，报错信息用

// FrameStats 的字段名在此定死；后续任务的测试按这些名字断言
struct FrameStats {
    int64_t trianglesSubmitted = 0, trianglesClippedOut = 0, trianglesDegenerate = 0,
            trianglesBackfaceCulled = 0, trianglesRasterized = 0, trianglesOffscreen = 0;
    // fragmentsCovered = 通过覆盖判定的像素数；fragmentsWritten = 实际落到颜色的像素数。
    // 两者的差 = 被 depth 拒掉的像素。1.4 的局部 RasterStats 在本任务折进来（同名换前缀），
    // M8 的后端一致性测试就靠这两个数：同场景下 A 与 B 必须给出完全相同的四个计数。
    int64_t fragmentsCovered = 0, fragmentsWritten = 0, fragmentsDepthRejected = 0, fragmentsShaded = 0;
    int64_t interpClampedCount = 0;
    double  msVertex = 0, msClip = 0, msSetup = 0, msRaster = 0, msTotal = 0;
    void reset(); void merge(const FrameStats& other);   // merge 给 M8 的多线程聚合用（禁止原子，§9）
};
struct VSIn  { Vec3 position; Vec3 normal; Vec2 uv; };
struct VSOut { Vec4 clip; Varyings v; };
struct ClipVertex { Vec4 clip; Varyings vOverW; };   // vOverW == v / clip.w  —— 见下方"必须解释"注释
struct ScreenVertex { Vec2 pos; float depth; float w; float invW; Varyings v; };
struct Mesh {                        // SoA + 索引
    std::vector<Vec3> positions, normals; std::vector<Vec2> uvs;
    std::vector<uint32_t> indices;    // 每 3 个一个三角形
    BBox bounds;  void recomputeBounds(); float boundRadius() const; Vec3 boundCenter() const;
    int triangleCount() const { return (int)indices.size()/3; }
};
Mesh makeCube(float size, bool perFaceNormals);
Mesh makeUVSphere(float radius, int stacks, int slices, bool duplicatedPoleVerts = true);
Mesh makeTorus(float R, float r, int ringSegments, int sideSegments);

struct VertexUniforms { Mat4 model, view, proj; Mat4 normalMatrix; };
struct FragmentUniforms { LinearColor baseColor; /* M4/M6 再加 texture/lights */ };
struct DrawState {
    RenderTarget* target;
    VertexShaderFn vertexShader; FragmentShaderFn fragmentShader;
    VertexUniforms vu; FragmentUniforms fu;
    bool depthTest = true;           // M2 恒通过（不判、不写），M3 填实现 —— 见 spec §7.1 矛盾7 的裁决
    bool depthWrite = true;
    bool backfaceCull = true;
    ShadingMode mode = ShadingMode::FlatColor;
    FrameStats* stats = nullptr;
};
class RenderBackend { public: virtual ~RenderBackend() = default;
    virtual const char* name() const = 0;
    virtual void draw(const Mesh&, const DrawState&) = 0; };   // 每 draw call 一次虚调用，内层不再有（C5）
class BackendA : public RenderBackend { ... };                 // 标量，M1 的实现挪进来
}
```

**为什么 `PlaneEq` 按 float 分量而不是按 struct 建**：`planeAttr` 的长度因此是编译期常量 12，M9 的 SIMD 可以一次处理 4 个分量而不必管结构体边界；代价是 `Varyings` 不能出现非 float 成员或填充，所以那条 `static_assert` 是接口的一部分，不是可删的装饰（`varyingFloats()` 是唯一的合法访问方式）。

**`ClipVertex::vOverW` 的注释必须解释**（A1、Review Focus 2）：为什么裁剪阶段对 `v/w` 与 `1/w` 做与 `clip` 同参数的线性插值是**正确的**，而对未除 w 的 `v` 插值是错的。这段推导是本项目的学习目标之一，写进 `vertex.h` 顶部而不是 notes 里（因为它是接口的一部分）。

- [ ] **Step 1: 写失败测试 `test_mesh.cpp`** — 断言：`makeCube(2)` 有 8 或 24 个顶点（按 `perFaceNormals` 两种都测）、12 个三角形、`bounds` 为 `[-1,1]³`、`boundRadius ≈ √3`（容差 1e-5）；`makeUVSphere(1, 8, 12)` 的**每个顶点长度 ≈ 1**（这是最容易写错的一条：法线未归一或经纬度索引错位会立刻暴露）、三角形数 == `2*slices + 2*(stacks-2)*slices`；`makeTorus` 无退化三角形（遍历所有三角形断言 `|area2| > 1e-6` 在世界坐标投影面上）。
- [ ] **Step 2: 跑，确认失败** → **Step 3: 实现** → **Step 4: 全绿**
- [ ] **Step 5: 把 M1 的 `rasterizeFlatTriangle` 挪进 `BackendA::draw`** 并让 `main.cpp` 改用 `Mesh makeCube` + 一个 `vertexShader`（`transformVS`：`clip = proj*view*model*{pos,1}`）+ 恒返回 `baseColor` 的 `fragmentShader`。此时裁剪/视口还没实现，立方体是"未裁剪的投影结果"，会出现越界/翻转——**预期行为**，在 notes 里记录症状并说明它将由 2.2 修复。
- [ ] **Step 6: 全绿 + 手动验证 + Commit** `feat: pipeline types, Mesh generators, RenderBackend interface`

### Task 2.2: clip space 六面裁剪（Review Focus 3）

**Files:** Create `src/render/clipping.h`, `src/render/clipping.cpp`
**Test:** `tests/unit/test_clipping.cpp`
**Produces:**
```cpp
namespace sr {
enum ClipPlane : uint8_t { NegW=0, PosX, NegX, PosY, NegY, PosZ, Count };   // -w<=x<=w, -w<=z<=w
int clipPolygonToFrustum(const ClipVertex in[3], ClipVertex out[8]);   // 返回 0..6 个顶点；0 = 完全剔除
bool vertexInsideFrustum(const Vec4& clip);
}}
```
- [ ] **Step 1: 写失败测试**（断言按"哪一侧被剔、哪一侧保留"具体到数字，不留 TBD）

- 完全在内 → 返回 3，且输出 == 输入（逐字段容差比较）。
- 完全在外（整个三角形 `x > w`）→ 返回 0，`stats.trianglesClippedOut` +1。
- 后半球（所有顶点 `w < 0`）→ 返回 0，不崩，`readPacked` 全未变。
- **总不变式（每条用例都跑）**：输出的每个顶点满足 `w > 0` 且 `-w-1e-6 <= x,y,z <= w+1e-6`。裁剪器的正确性主要靠它，因为它对任意输入都成立、不需要手算参考值。
- **一条精确参考用例**（跨 `PosX` 平面 `x <= w`，其余 5 个平面自然满足）：
  ```
  A = clip(1.5,  0.0, 0, 1)   外部   uOverW = 3      // uOverW 是 Varyings 里借用的一个测试分量
  B = clip(0.0,  1.0, 0, 1)   内部   uOverW = 0
  C = clip(0.0, -1.0, 0, 1)   内部   uOverW = 0
  ```
  手工推导（写进测试注释，这是"实现前就算好"的那份）：边 `A→B` 上 `d = w - x` 从 `-0.5` 到 `1`，`t = d_A/(d_A-d_B) = 1/3` ⇒ 交点 `(1.0, 1/3)`，`uOverW = lerp(3, 0, 1/3) = 2`；边 `C→A` 上 `t = 1/(1+0.5) = 2/3` ⇒ 交点 `(1.0, -1/3)`，`uOverW = lerp(0, 3, 2/3) = 2`。
  于是断言：返回 **4** 个顶点；顺序为 `[B, (1,1/3), C, (1,-1/3)]`（起点由实现约定，测试用"多重集相等 + 相邻性"而非硬索引，避免为顺序写死脆弱断言）；两个交点的 `clip.x == clip.w`（容差 1e-6，即**正好贴在平面上**）；两个交点的 `uOverW == 2`（容差 1e-5，**Review Focus 2 的裁剪侧**：证明 `vOverW` 与 `clip` 用的是同一个 `t`）；输出多边形在 clip 空间 x-y 上的 shoelace 面积 == **4/3**，原三角形 == **3/2**，比值 == **8/9 ≈ 0.888888**（容差 1e-5）。
- **幂等性**：把上一条的输出重组成三角形再裁一次，结果不变（顶点数与面积同容差）。这条比任何单点参考值都更能抓住"漏了一个平面"。
- 一个三角形的一条边**正好落在** `x == w` 上 → 不产生重复顶点（相邻输出顶点距离 > 1e-6），且输出顶点数 ≤ 4。
- 退化输入（三点相同且在外部）→ 返回 0，不崩。
- 近平面把三角形切成窄带 → 顶点数 ∈ [3,5] + 总不变式（这种几何的参考值容易算错，靠不变式与幂等性守门，不硬写数字）。

- [ ] **Step 2: 跑，确认失败** → **Step 3: 实现**

算法：对 6 个平面依次做 Sutherland–Hodgman（输入多边形 ≤ 8 顶点，`clipSegment` 解 `t = (planeDist(a)) / (planeDist(a) - planeDist(b))`）。`ClipVertex` 与 `Varyings` 用同一 `lerp` 参数一起插（这就是"对 `v/w` 插值"的落地点）。**不做**逐平面重三角化——只在最后一步按扇形 `out[0], out[i], out[i+1]` 出三角形。

- [ ] **Step 4: 跑，全绿** → **Step 5: 接进 `BackendA::draw`**（顺序：VS → 装配 → 裁剪 → 视口）→ **Step 6: Commit** `feat: clip-space 6-plane Sutherland-Hodgman polygon clipper`

### Task 2.3: 透视除法 + Viewport（top-down，C8）

**Files:** Create `src/render/viewport.h`, `src/render/viewport.cpp`
**Test:** `tests/unit/test_viewport.cpp`
**Produces:**
```cpp
namespace sr {
struct Viewport { int x=0, y=0, width=1, height=1;
    ScreenVertex project(const ClipVertex&) const;                 // clip → 屏幕（top-down），含 depth∈[0,1]
    static ScreenVertex fromNdc(const Vec3& ndc, const Varyings&, const Viewport&);
}}
```
- [ ] **Step 1: 写失败测试** — NDC 角点映射（**逐条 pin，含 y 翻转**）：

| NDC | 期望屏幕坐标 | 期望 depth |
|---|---|---|
| `(-1,-1, -1)` | `(x + 0.0, y + height)` | `0.0` |
| `( 1, 1,  1)` | `(x + width, y + 0.0)` | `1.0` |
| `( 0, 0,  0)` | `(x + width/2, y + height/2)` | `0.5` |
| `( 0, 1, -1)` | `(x + width/2, y + 0.0)`（NDC 上边缘 → top-down 的第 0 行） | `0.0` |

另加：`w != 1` 时 `project` 先除 w（用一个 `clip = {2,2,0,2}` 断言与 `{1,1,0,1}` 同结果）；`v` 输出 = `vOverW / invW`，用 `vOverW = v*invW` 构造断言能还原 `v`（容差 1e-6）；`depth` 的 `0.5` 在中点（非线性但中点值确定）。
- [ ] **Step 2–4:** 红 → 实现（`pos.x = x + (ndc.x*0.5+0.5)*width`；`pos.y = y + (1.0 - (ndc.y*0.5+0.5))*height`；`depth = ndc.z*0.5+0.5`）→ 绿。
- [ ] **Step 5: Commit** `feat: perspective divide + top-down viewport`

### Task 2.4: Primitive Setup —— 面积/朝向/背面剔除/scissor（Review Focus 1、3）

**Files:** Create `src/render/triangle_setup.h`, `src/render/triangle_setup.cpp`；Modify `src/render/triangle.h`
**Test:** `tests/unit/test_triangle_setup.cpp`
**Produces:**
```cpp
namespace sr {
struct SetupResult { bool rejected; uint8_t rejectReason; };   // 0 none, 1 degenerate, 2 backface, 3 nonfinite, 4 offscreen
// 绕向约定（本任务把它定死，不要留给实现去"试"）：
// NDC 里 CCW 为正面 + Viewport 的 y 翻转（C8，Task 2.3）⇒ 屏幕 top-down 空间里正面是 **CW**，
// 且此时有 area2 < 0（因为 y 轴朝下使叉积符号翻转）。默认 FrontFace::CW 配 rejectOnArea2Positive。
enum class FrontFace : uint8_t { CW, CCW };                     // 在 top-down 屏幕空间中定义
struct TriangleSetup {
    ScreenVertex v[3];
    EdgeFunc edges[3]; PlaneEq planeInvW; PlaneEq planeAttr[kVaryingFloatCount];
    float area2 = 0;           // 有符号 2 倍屏幕面积（正面为负，见上）
    float dudx, dudy, dvdx, dvdy;   // 屏幕空间 uv 梯度（M4 用），此处先算好
    Rect extent;               // 已夹到 ColorTarget 的像素 bbox
    Rect tileRange;            // 已夹到整张 tile 网格（Backend B 预留，此任务只算不消费）
    bool backFacing = false;
};
// 唯一入口：几何 + 剔除决策 + 梯度准备
bool setupTriangle(const ScreenVertex in[3], const Viewport&, const DrawState&, TriangleSetup&, SetupResult&);
}
```
- [ ] **Step 1: 写失败测试** — 断言：

- **绕向与剔除（Review Focus 1）**，三条断言一起 pin：
  1. 屏幕 top-down 空间中 `p0={0,0}, p1={10,0}, p2={0,10}`（视觉上顺时针）→ `area2 < 0`，且 `FrontFace::CW` 时 `backFacing == false`。
  2. 同一三角形顶点顺序换成 `p0,p2,p1` → `area2 > 0` → `backFacing == true`，`backfaceCull==true` 时 `rejected && rejectReason == backface`，**且不写入任何像素**。
  3. **凸体面数不变式**：`makeCube(2, perFaceNormals=true)`（24 顶点 / 12 三角形），相机 `{0,0,5}` 看原点，model 旋转 `(25°, 35°, 0)` 的一般姿态 → `trianglesBackfaceCulled == 6`、`trianglesRasterized == 6`（恰好一半）；把 `FrontFace` 从 `CW` 换成 `CCW`，被剔的必须换成**另外那 6 个**（断言：两次通过的三角形索引集合互不相交且并集为全部 12 个）——这是比"数量相等"强得多的判据，能把绕向错误和"剔错了面"区分开。
  4. **正对姿态的退化面**：同一相机下 `model = identity` → 4 个侧面投影成零面积线段，断言 `trianglesDegenerate == 8`、`trianglesBackfaceCulled == 2`、`trianglesRasterized == 2`。**判定顺序在此定死**：先测零面积再测朝向（面积≈0 时朝向无意义，若先判朝向会得到随 `eps` 抖动的结果）。这条同时是"退化三角形不进入光栅"的前置保证（Review Focus 3）。
- 零面积（共线）→ `rejected && rejectReason == degenerate`。
- 含 `NaN`/`inf` 的输入 → `rejected && rejectReason == nonfinite`（Review Focus 3），且**不写入任何像素**（用一个被污染的目标断言每个字节未变）。
- bbox 被 `ColorTarget` 边界夹住：一个跨出目标右侧的大三角形，`extent` 不超过 `width-1`，且 `pixelsWritten` 只落在合法范围。
- `tileRange` 覆盖 `extent` 且不超过 tile 网格；一个"覆盖整屏"的三角形，`tileRange` == 整个网格（**不是巨大的负数/正数**——这正是 spec §7.1 选六面裁剪想换来的性质，测试要守住它）。
- `planeInvW.sample` 在三个顶点处 ≈ `1/v[i].w`（容差 1e-4）。
- `dxdx/dxdy/...`：对一个 uv 沿 x 每像素 +1 的三角形，`dudx ≈ 1`、`dudy ≈ 0`（M4 依赖，先 pin）。
- [ ] **Step 2: 跑，确认失败** → **Step 3: 实现**（`area2` 用 `edge0` 法线点乘；背面判定 = `area2` 符号 vs `FrontFace`；**`depthTest` 标志在此读取但 M2 恒通过**，代码里留 `// M3: 真正实现` 注释，避免 M3 重构光栅阶段——spec 矛盾 7 的裁决）→ **Step 4: 全绿**
- [ ] **Step 5: Commit** `feat: primitive setup, orientation-based backface culling, bbox/tile clamping`

### Task 2.5: M2 集成 —— 旋转立方体（无深度）

**Files:** Modify `src/render/raster_scalar.cpp`（`BackendA::draw` 走完 VS→clip→project→setup→raster 全链）、`src/app/main.cpp`、`src/scene/mesh.cpp`（`makeCube` 的面色）
**Test:** `tests/unit/test_pipeline_integration.cpp`
- [ ] **Step 1: 写失败测试**（离屏 `ColorTarget` + `Viewport` + `DrawState`，不打窗口）

**先说清一件事**：凸立方体在**开启背面剔除**时，三个可见面的投影只在棱上相接、互不覆盖，所以"没有深度"根本不会穿帮。要看到 painter's 的缺陷必须 `backfaceCull = false`。spec §8 的 M2 行"穿帮是好事"据此实现为"关掉剔除"，并在 notes 里把这件事讲透（**剔除本身就是一种遮挡求解**，这是个好问题）。立方体面序固定为 `+X,-X,+Y,-Y,+Z,-Z`，每面用可区分的纯色。

- 相机 `{0,0,5}` 看原点、`makeCube(2)`、model 旋转 `(25°,35°,0)`：
  - `backfaceCull=true` → `trianglesSubmitted == 12`、`trianglesBackfaceCulled == 6`、`trianglesRasterized == 6`；再旋转一个任意角度重跑，三个数不变（凸体不变式）。
  - `backfaceCull=false` → `trianglesRasterized == 12`（裁剪/退化之外都进来）。
- **`no_depth_no_cull_shows_the_painter_artifact`**：`depthTest=false, backfaceCull=false`，断言立方体剪影中心某个像素的颜色 == 最后绘制的那个面（`-Z`，即背面）的颜色，而**不是** `+Z` 前面的颜色。这条把缺陷 pin 住；M3 里同一场景 + `depthTest=true` 必须翻转成 `+Z` 的颜色（写成同名测试的第二条断言，用同一份场景构造函数，只差一个标志）。
- `backfaceCull=true, depthTest=false` → 断言**没有**穿帮（中心像素 == `+Z` 面颜色），因为剔除已经解决了凸体的遮挡。这一条防止有人误把"M2 有穿帮"当成深度测试的前提。
- 相机推入立方体内部（`eye = {0,0,0}`）：断言没有任何写入像素落在 `extent` 之外、`readPacked` 全是合法值（无 NaN 位模式）、`trianglesClippedOut == 0`（面都在近旁，全部要通过裁剪而不是被丢弃）——这是 Review Focus 3 + 六面裁剪的联合回归，"面弹出"症状在这里被抓。

- [ ] **Step 2: 跑，确认失败** → **Step 3: 打通管线** → **Step 4: 全绿**
- [ ] **Step 5: 手动验证窗口**（`main.cpp` 里加一个临时键切换 `backfaceCull`，M7 才正式做成视图选项）
  Expected: 剔除开启时——干净的旋转纯色立方体，六个面里始终只看到三个，没有裂缝、没有"半个立方体消失"（若消失 → 绕向错，回 2.4）；剔除关闭时——背面透出来覆盖正面（这就是 painter's 缺陷的肉眼版本，也是 M3 的动机）。
- [ ] **Step 6: Commit** `feat: end-to-end geometry pipeline, spinning cube without depth`

### Task 2.6: M2 收尾

- [ ] `docs/notes/M2.md`：为什么六面裁剪而不是近平面裁剪（附"只裁近平面时 extent/tileRange 炸开"的实测数字，跑一次相机推入模型的场景打印 extent 得到，这是最有说服力的一张表）；`vOverW` 的推导；绕向与 y 翻转的耦合为什么让半个立方体消失；凸体 `culled == 6` 这条不变式的适用边界（非凸模型会失效，记下来，M5 载入真实模型时别指望它）。
- [ ] README + 图 + Commit + push。

---

## M3：Z-buffer + 属性插值 + 顶点色 + golden 框架

工作成果：遮挡正确的平滑顶点色立方体 + depth 调试视图 + 第一份 golden BMP。到 M3，`docs/superpowers/specs/…#8` 的 M0–M3 全部达成。

### Task 3.1: 透视正确插值（Review Focus 2）

**Files:** Modify `src/render/triangle.h`（`planeAttr` 已存在，补采样端）、Create `src/render/interpolate.h`
**Test:** `tests/unit/test_interpolate.cpp`
**Produces:**
```cpp
namespace sr {
struct Interpolated { Varyings v; float depth; float invW; };
Interpolated interpolateAt(const TriangleSetup&, float px, float py);
float  interpolateInvW(const TriangleSetup&, float px, float py);   // = planeInvW.sample
}
```
- [ ] **Step 1: 写失败测试** — 断言：

- 三顶点处（在顶点坐标 `v[i].pos` 采样）各 varying 分量 ≈ 输入值（容差 1e-4）——即"重心坐标在顶点为 1"的直接验证。
- 三角形几何重心处 == 三个输入值的算术平均（容差 1e-4）。注意这只对**线性**属性成立；对 `attr/w` 建立的面在投影后同样成立（因为 `attr` 在顶点处被还原），所以这条不会误报。
- 一条边上取中点，插值结果 == 该边上 `1/w` 与 `attr/w` 的算术平均之商（用数值验证，不靠推理）。
- **`perspective_correct_u_at_edge_midpoint`（本项目最重要的一条数值测试；Review Focus 2）**
  构造屏幕三角形（`Varyings::uv.x` 当被测属性 u）：
  ```
  v0: pos(0, 0)   w = 1   u = 0     → uvOverW = 0,   invW = 1
  v1: pos(10, 0)  w = 10  u = 1     → uvOverW = 0.1, invW = 0.1     // v1 远 10 倍
  v2: pos(0, 10)  w = 1   u = 0     → uvOverW = 0,   invW = 1
  ```
  手工推导（写进测试注释，实现前算好）：`planeInvW = 1 - 0.09x`（`c=1`，`n0=(0.1-1)/10=-0.09`，`n1=(1-1)/10=0`）；`planeU = 0.01x`。在边 `v0v1` 的屏幕中点 `x=5`：`invW = 0.55`，`uOverW = 0.05`，**`u = 0.05/0.55 = 0.0909091`**。
  ```cpp
  SR_ASSERT_NEAR(u, 0.0909091f, 1e-5f);
  SR_ASSERT(std::fabs(u - 0.5f) > 0.4f);   // 证明这条测试能区分"透视正确"与"线性插值"
  ```
  线性（不除 w）实现会给出 `0.5`，所以第二条断言就是"区分力证明"。若结果落在两者之外，说明 `planeAttr` 建的是 `attr` 而不是 `attr/w`，或者 `invW` 面建错。
- `depth` 分量在三个顶点 ≈ 输入 depth（`1e-6`）；`invW` 分量在三顶点 ≈ `1/w`（`1e-6`）。
- **`invW_near_zero_is_clamped_not_nan`**：把 `v1` 的 `w` 设为 `1e9`（`invW≈1e-9`），采样点靠近 `v1`，断言返回值是**有限**的（`std::isfinite`）且 `stats.interpClampedCount == 1`。热路径的 NaN 会顺着 blend 污染整块区域，这条守住它。

- [ ] **Step 2: 跑，确认失败** → **Step 3: 实现**（`invW = planeInvW.sample`；`v.* = planeAttr[k].sample / invW`，`invW` 接近 0 时 clamp 并计入 `stats.interpClamped`）→ **Step 4: 全绿**
- [ ] **Step 5: Commit** `feat: perspective-correct plane-equation interpolation`

### Task 3.2: 深度测试与写入

**Files:** Modify `src/render/raster_scalar.cpp`（把 2.4 预留的 `depthTest/depthWrite` 填实）；`DepthTarget::read` 越界返回 `+inf` 已在 0.4 定下，本任务只消费
**Test:** `tests/unit/test_depth.cpp`
- [ ] **Step 1: 写失败测试**
- clear 到 1.0（约定）：新 `DepthTarget` clear 后 `read == 1.0f`。
- 两个三角形重叠、一个近一个远：重叠区颜色来自近的、`stats.fragmentsDepthRejected > 0`。
- **共面 `<=` 语义**：把第二个三角形的 depth 设为与第一个完全相同 → 后者**写入**（断言颜色 = 后者、depth 值不变）。这一条 pin 了 spec §7.1 的 `<=`，也解释了 Z-fighting 的取舍。
- `depthWrite=false` + `depthTest=true`：不遮挡、也不污染深度缓冲（断言之后一个更远的三角形仍画得出来）。
- **深度翻转了 2.5 的那条缺陷断言**：同一场景构造函数、同一像素坐标，只改一个标志 →
  `depthTest=false, backfaceCull=false` ⇒ 中心像素 = `-Z` 背面颜色（沿用 2.5 的断言，必须仍然成立）；
  `depthTest=true, backfaceCull=false` ⇒ 同一像素 = `+Z` 前面颜色。两条写在一个测试里并排断言，这是"深度测试做了什么"最直接的证明，也是 spec §8 的 M2→M3 过渡的验收。
- 深度写入的独立性：`depthTest=false, depthWrite=true` 时，后面的三角形仍会被它挡住（症状分离：如果这条失败，说明 `depthWrite` 被错误地绑在 `depthTest` 上）。
- [ ] **Step 2: 跑，确认失败** → **Step 3: 实现** → **Step 4: 全绿**
- [ ] **Step 5: 手动验证** Expected: 旋转立方体不再穿帮；转到棱边时没有裂缝。
- [ ] **Step 6: Commit** `feat: depth test/write with top-left-consistent <= semantics`

### Task 3.3: 顶点色端到端

**Files:** Modify `src/render/triangle_setup.cpp`（`planeAttr` 用 `vOverW` 建）、`src/scene/mesh.cpp`（立方体顶点色）、`src/render/pipeline.h`（`transformVS` + `vertexColorFS` 正式化）
**Test:** `tests/unit/test_vertex_color.cpp`
- [ ] **Step 1: 写失败测试** — 断言：一个三角形三个顶点 (红,绿,蓝) 线性色，中心像素的线性颜色 ≈ 三者的算术平均（容差 1e-3，**在线性空间比较**，写进 notes：在 sRGB 编码后比较会得到不同的平均值，这本身是个有用的事实）；沿一条边取两点，颜色是单调的（无台阶）；`makeUVSphere` 顶点色 = 位置归一化时，赤道上相邻两个像素颜色差 < 阈值（无 banding，说明插值连续）。
- [ ] **Step 2: 跑，确认失败** → **Step 3: 实现** → **Step 4: 全绿**
- [ ] **Step 5: 手动验证** Expected: 平滑着色的立方体。
- [ ] **Step 6: Commit** `feat: vertex-color varyings through the whole pipeline`

### Task 3.4: Output sink 抽象 + depth 调试视图

**Files:** Create `src/render/sink.h`, `src/render/sink.cpp`
**Test:** `tests/unit/test_sink.cpp`
**Produces:**
```cpp
namespace sr {
class OutputSink { public: virtual void present(const RenderTarget&) = 0; virtual ~OutputSink() = default; };
class DebugViewSink : public OutputSink {   // depth / 之后的 normal/uv/overdraw 都加在这里
public: enum class View { Color, Depth, Gray };  View view = View::Color; float depthNear = 0, depthFar = 1;
    void present(const RenderTarget&) override;   // 就地改写 color（RenderTarget 是这一帧的，可覆盖）
};
}}
```
- [ ] **Step 1: 写失败测试** — 断言：一个 depth 全 0 的目标 → `View::Depth` 后所有像素等于 `encodeSRGB(blackOrWhite per spec)`（**方向必须显式决定并 pin**：近=亮/远=暗，写进注释，症状判读要靠它）；depth 全 1 → 另一端；`depthNear/depthFar` 设为 [0.25,0.75] → 0.25 以下全部饱和到同一端（证明做了归一化而不是直接把 depth 当灰度）；`View::Color` 下 present 不改动任何像素（恒等）。
- [ ] **Step 2: 跑，确认失败** → **Step 3: 实现** → **Step 4: 全绿**
- [ ] **Step 5: 按键接上**（`D` 切 color/depth）+ 手动验证 Expected: 立方体正面亮、远处暗，背景是纯的一端。
- [ ] **Step 6: Commit** `feat: OutputSink + depth debug view`

### Task 3.5: golden image 框架（C1′）

**Files:** Create `tests/golden/golden_scene.h`, `tests/golden/golden_scene.cpp`, `tests/golden/golden.cpp`；Create `assets/golden/README.md`
**Test:** CTest 测试名 `golden`（只在 MSVC 配置里注册）
**Produces:**
```cpp
namespace sr {
// 完全确定：640x360、Backend A、单线程、depthTest on、固定 model/view/proj、固定顶点色立方体
void renderGoldenScene(RenderTarget&, FrameStats* = nullptr);
int  compareBmp(const ColorTarget&, const std::string& goldenPath, int tolerancePerChannel, std::string& diffReport);
}
```
- [ ] **Step 1: CMake 里注册 golden 测试**（条件：`MSVC AND EXISTS ${CMAKE_SOURCE_DIR}/assets/golden/m3_cube.bmp`）；golden 输出目录不存在时跳过并打印 `SKIP`（用 `set_tests_properties(... PASS_REGULAR_EXPRESSION "SKIP|OK")`，**不要**静默通过）
- [ ] **Step 2: 写测试** — `SR_TEST(golden_m3_cube_matches_reference)`：渲染 → 与 `assets/golden/m3_cube.bmp` 比 ±1/255/通道 → 不一致时打印**第一个差异像素坐标 + 前 5 个差异 + 差异像素总数**（诊断信息比"fail"有用得多）。
- [ ] **Step 3: 跑，确认失败** Expected: 参考文件不存在 → SKIP 或 fail
- [ ] **Step 4: 生成参考** `./build/Debug/soft-raster.exe --offscreen --frames 1 --width 640 --height 360 --out assets/golden/m3_cube.bmp`，**肉眼看一次**（错位的 golden 会把后面每个里程碑都锁死）→ 提交进仓库。
- [ ] **Step 5: 跑，全绿** Expected: `100% tests passed`，golden 测试为 OK
- [ ] **Step 6: 破坏性验证** — 临时把 viewport 的 y 翻转去掉，跑 golden，Expected: fail 且报出大量差异（证明 golden 真的在守门）。恢复。
- [ ] **Step 7: Commit** `test: golden-image harness + M3 reference`

### Task 3.6: M3 收尾

- [ ] `docs/notes/M3.md`：`1/w` 线性为什么导出 `attr` 的透视正确（完整推导 + 那张四边形数值）；`<=` 与共面 Z-fighting；为什么 golden 要比"手算像素期望值"更便宜且更能防回归；`depth` 视图方向如何影响你读 bug。
- [ ] README：M0–M3 打勾 + 立方体截图；`docs/notes/img/` 提交图片。
- [ ] 全套验证：MSVC 侧 `ctest`（unit + golden）全绿；g++ 侧 unit 全绿；C4 `grep` CLEAN；Release 配置也过一遍（`cmake --build build --config Release && ctest --test-dir build -C Release`，C6/C1′：Release 下浮点一致性可能让 golden 容差紧张，若失败**先调容差再考虑 `/fp:strict`**，并把结论写进 notes）。
- [ ] Commit + push。

---

## M4–M10：概要（每个里程碑到点时再展开成同格式的任务表）

到 M3 结束时会知道现在还不知道的事（`PlaneEq` 的性能特征、裁剪的实际剔除比例、golden 在 Release 下的容差余量），所以 M4 之后只给概要 + "展开时必须回答的问题"。

**M4 纹理**（`texture.h/.cpp`、`sampler.h/.cpp`、mip 链生成、`loader/image_stb.*` + `image_tga.*`）
- 任务分解要点：`generateMipChain`（纯函数，可单测：面积/像素和、盒式滤波的正确值）→ `sampleNearest`（UV 越界与 wrap）→ `sampleBilinear`（在 texel 中心取整数、跨边界的 wrap 一致性）→ `mipLevelFromDerivatives`（用 2.4 已算好的 `dudx/dvdx`，断言已知三角形的层级选择）→ trilinear → `stb_image` 接入（`third_party/stb_image.h` + `image_stb.cpp`，**唯一一处第三方**，C2 三条限制：只此文件、只解码、core 不感知）→ 手写 TGA（作为 stb 缺席的回退，且是"图像就是字节数组"的教学样本）→ 立方体贴 checker，`1` 键开关 mip 观察闪烁（A 组验收：spec §8 M4 的可见成果）。
- 展开时必须回答：`FilterMode` 放在 `FragmentUniforms` 还是 `DrawState`；mip 生成放 core 还是 loader；PNG 的 16bit/灰度/调色板输入如何归一到 RGBA8。

**M5 OBJ/MTL 导入 + 自动 fit**（`loader/obj.*`、`loader/mtl.*`、`scene/camera.*`）
- `parseObj` 做成**纯字符串/流函数**（不 IO），便于单测：负索引、quad→两三角、缺 `vn`→面法线、`vt` 缺失→uv=0、`usemtl` 分组出多个 `SubMesh`、`mtllib` 解析、`map_Kd` 的相对路径拼接（**外部模型最容易咬人的地方**：贴图路径相对 .mtl 而非 cwd）。
- 相机包围球 fit：`near = r/1000, far = r*10`，`F` 键重 fit，`--no-fit` 关闭（spec §7.5）。
- 展开时必须回答：一个 OBJ 里 `f` 混用 `v/vt/vn` 组合时索引怎么统一（`v//vn`、`v/vt//`）；超过 24bit 索引/超大模型要不要报错。

**M6 光照**（`render/lighting.h/.cpp`，`shaders/*`）
- `lambertPerVertex`（Gouraud）与 `blinnPhongPerPixel` **两个都要**，为的是能切换着看差别（spec §7.3 明示保留动机）；`LightUniforms`（方向光 ≤4 + 环境项）；线性空间着色 → `encodeSRGB` 写出（0.3 的 LUT 在此成为热路径）。
- 展开时必须回答：法线矩阵用 `transpose(inverse(model))` 的 3×3（1.2 已备 `upper3x3AsMat4`），非均匀缩放的球体是它的回归场景；要不要把 `Varyings` 的 `worldNormal` 归一放 FS（插值后长度 < 1，影响高光）。

**M7 交互与可视化 —— 终态演示版**
- 轨道相机（拖拽/滚轮/右键平移）、HUD（FPS + 分阶段 ms + 三角形/像素计数 + overdraw 比）、视图循环（wireframe / normal / uv / overdraw / binning）、暂停与单步（C7）、`--obj` 运行时重载。
- 展开时必须回答：wireframe 用 Bresenham 画线（新单元 `raster_lines`）还是"退化三角形"技巧；overdraw 热图用 `uint16` 计数缓冲还是把 `DepthTarget` 泛化成 `Attachment`。

**M8 tile 分箱 + 多线程（Backend B）—— G3 的 15 fps 在此达成**
- 顺序：tile 网格与 bin 数据结构（`TileBin`，几何阶段填 triangle reference）→ `DepthTarget::view()`（0.4 已备）消费 → 单线程 tiled 版跑通并与 Backend A **逐像素一致 ±1/255**（§7.8 的硬约束，写成 CTest 用例）→ 线程池（约 150 行，§9 builder's call）→ 线程数按键切换 → `--bench` 输出对比表。
- 展开时必须回答：bin 的重建频率（每帧全清重灌 vs 静态几何缓存）；三角形极大时的 tileRange 上限保护；`FrameStats` 在多线下怎么聚合（每线程私有 + 合并，禁止原子）。
- **如果 15 fps 达不到**：按 N6 缩小验收模型并如实记录，不引入 shading 向量化。

**M9 SIMD inner loop（范围已在 §7.3 划死）**
- SSE2 起步：8×8 微 tile 的 8 路 x 偏移一次求 edge function、一次算 planeInvW/planeAttr、一次 depth 比较与掩码写入；`_mm_movemask` 得覆盖位图。**不做** shading 向量化。
- 与标量结果一致性测试是主验收（±1/255/通道）；`docs/notes/M9.md` 的核心内容是**收益为什么随场景而变**（覆盖/插值主导 vs shading 主导），以及"这就是 GPU 必须 SIMT 的原因"。
- 展开时必须回答：定点 edge function（`_mm_cmpgt_epi32`）的溢出边界（六面裁剪保证的有界 extent 在这里兑现价值）；要不要 AVX2（16 路）。

**M10 收尾**
- SSAA（超采样式，spec §8 已定语义）：2×/4× 内部 `RenderTarget` + box filter resolve sink；golden 全量脚本化（一个 `scripts/regenerate_golden.ps1` + `scripts/bench.ps1`）；`docs/notes/` 十篇成文并加索引；README 补效果图、按键表、性能表、"这个项目教会我什么"的收束段。
