# M5 OBJ/MTL 导入 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 `--obj` 真的生效：载入外部 OBJ + MTL + 贴图，按材质分多次 draw call，自动 fit 相机，并且三处"方向约定"各自被独立的锚钉死。

**Architecture:** 解析在 `soft_raster_core`（纯文本 → 纯数据，零 IO、零 stb 感知），装配在 `soft_raster_loader`（唯一能同时看见 OBJ 数据与像素解码的层），场景持有在 `src/app/`。分层理由见设计文档 §6 的方案 A：`tests` 链 core + loader、不链 app，所以装配落在 loader 是"端到端可测"与"C2 守得住"的唯一交集。

**Tech Stack:** C++20；CMake + MSVC（主路径）/ MSYS2 g++（次路径）；自建测试 harness（`SR_TEST` / `SR_ASSERT` / `SR_ASSERT_NEAR` / `SR_ASSERT_EQ`）；CTest；无第三方渲染依赖（唯一第三方仍是 `third_party/stb_image.h`，只被 `src/loader/image_stb.*` 包含）。

**Spec:** [`docs/superpowers/specs/2026-11-22-soft-raster-m5-obj-design.md`](../specs/2026-11-22-soft-raster-m5-obj-design.md) —— 19 条裁决（**D1–D18 + D14 续**）全部权威，本计划只翻译不重开。上游是 [`2026-11-21-soft-raster-design.md`](2026-11-21-soft-raster-design.md)。

## Global Constraints

逐字抄自设计文档与上游 spec，每个任务的隐含要求都包含这一节：

- **C2**：`stb_image` 只许出现在 `src/loader/image_stb.*`。机器检查：`grep -rn "stb_image" src/` 只命中那一个文件。
- **C4**：`soft_raster_core` 内不得出现 `<windows.h>` 或任何 Win32 符号。平台代码全在 `src/app/`。
- **C5**：热路径（逐像素/逐三角形内层）不分配、不异常、不逐像素虚派发。虚调用**按 draw call 一次**。
- **C1′**：单元测试的数值断言一律容差比较，默认 `1e-5f`；golden 只在 MSVC 侧判定，g++ 侧只要求"能配置、能编译、纯逻辑单测全绿"。
- **C8**：core 的颜色与深度输出一律 **top-down**；`ImageRGBA8` 第 0 行是图像上边缘，采样器 `v=0` = 第 0 行。
- **D2**：所有解析函数收**文本缓冲**（`std::string_view`），**不做 IO**；IO 只在 `loadModel` 一处发生。
- **CMake 警告地板**：MSVC `/W4 /utf-8`、GNU `-Wall -Wextra -ffp-contract=off`，三侧**零警告**。新增的 .cpp 必须手工列进 `CMakeLists.txt` 对应 target（测试文件尤甚：harness 靠静态初始化自注册，做成静态库会被链接器整个丢掉 ⇒ 那个文件的用例**静默地一条都不跑**）。
- **验证纪律四条**（产品方定，2026-11-21）：① 新测试必须过**变异验证**（把被测代码故意写坏，确认断言变红；红不出来 = 假测试）；② **构建失败时绝不读测试结果**（先 `grep -E "warning|error"`，非空则本轮结论作废）；③ Release 验证一律 `--clean-first`；④ **生成 golden 前必须产品方亲眼在窗口里验收**。
- 每个任务结束提交一次；提交信息用项目现有风格（`feat(loader): …（M5.n）`），中文正文说明**为什么**而不是**改了什么**。

## Review Focus

设计文档隐含、但没有任何一条"快乐路径"测试会抓到的五类输入 —— 每行在本计划里都有归属任务，加粗处写明落在哪个 Step：

1. **同一张贴图被翻了两次行**（OBJ 的 `vt` 与 TGA 的 origin bit 各翻一次，两处同时写错则画面看着完全正确，golden 也不红）。⇒ M5.2 Step 6、M5.3 Step 5、M5.7 Step 5 三条**各自独立**的锚。
2. **中文路径与带空格的贴图文件名**（产品方实际用 Git Bash，argv 是 UTF-8 而 MSVC 的 `path(std::string)` 按 ANSI(936) 解释；Blender 写 `map_Kd -s 1 1 1 my brick.png`）。⇒ M5.1 Step 7、M5.4 Step 8、M5.6 Step 9。
3. **一个 group 的索引数不是 3 的倍数 / 尾部残缺**（材质切换切在面中间、文件被截断）。⇒ M5.0 Step 3、M5.2 Step 9。
4. **外部文件的 uv 越界**（`vt 1.5 -0.2` 在真实 OBJ 里很常见，而我们的兜底是"画成边缘颜色/全 0"，静默）。⇒ M5.2 Step 8、M5.4 Step 7。
5. **`Kd` 与贴图被 sRGB 解码两次或零次**（症状是"整体暗一档 / 亮一档"，看起来像光照问题而不是颜色管理问题）。⇒ M5.1 Step 3、M5.4 Step 6。

---

## 文件结构

**新建**

| 文件 | 属于哪个 target | 责任 |
|---|---|---|
| `src/loader/mtl.h` / `mtl.cpp` | core | MTL 文本 → `Material` 表；`Kd/Ka/Ks/Ns/d/map_Kd`；D17 的选项状态机 |
| `src/loader/obj.h` / `obj.cpp` | core | OBJ 文本 → `Model`（`Mesh` + `groups` + `materials`）；D6–D10 + 收紧 3 的材质兜底规则 |
| `src/loader/tga_writer.h` / `tga_writer.cpp` | core | `ImageRGBA8` → 未压缩 32bpp TGA，**故意 bottom-up**（让锚 2 有内容） |
| `src/loader/model.h` / `model.cpp` | **loader** | `loadModel`：读文件 + 装配贴图 + warnings；`drawModel`：逐 group 提交 |
| `src/scene/fit.h` | core | `fitCamera`：包围球 → 相机距离 / near / far（纯 header，无 .cpp） |
| `src/app/path.h` / `path.cpp` | app | `nativePath(utf8) → std::filesystem::path`。**全仓库唯一一处** UTF-8↔路径编码转换 |
| `src/app/scene.h` / `scene.cpp` | app（+ 手工列进 tests，同 `cmdline.cpp` 的先例） | 场景持有者：`LoadedModel` + 相机 + fit + 错误行 |
| `tests/unit/test_mtl.cpp`、`test_obj.cpp`、`test_tga_writer.cpp`、`test_model.cpp`、`test_fit.cpp`、`test_obj_golden.cpp`、`test_draw_range.cpp` | tests | 各任务的用例 |
| `assets/obj/cube.obj`、`cube.mtl`、`twomats.obj`、`twomats.mtl`、`assets/obj/README.md`、`assets/tga/checker.tga` | 资产 | 见 M5.3/M5.4/M5.7；README 记 provenance（Ruling 33 的先例） |
| `docs/notes/M5-OBJ导入与三次翻转.md` | 文档 | 学习笔记，验收条件 |

**修改**：`tests/main.cpp:20`、`src/render/stats.h`、`src/render/render_backend.h/.cpp`、`src/render/color.h/.cpp`、`src/render/texture.h/.cpp`、`src/loader/image_stb.h/.cpp`、`src/loader/image_tga.h/.cpp`、`src/scene/golden_scene.h/.cpp`、`src/app/cmdline.h/.cpp`、`src/app/input.h`、`src/app/win32_window.cpp`、`src/app/main.cpp`、`CMakeLists.txt`、`README.md`、`docs/superpowers/specs/2026-11-21-soft-raster-design.md`（加脚注，见 M5.8）。

---

## 对设计文档的六处收紧

写计划时发现的、比原文更好的做法。**都记进 SDD 账本**（`.superpowers/sdd/2026-11-23-soft-raster-m5-obj/progress.md`，Rulings 从 **43** 起编号；`.superpowers/` 在 `.gitignore` 里，**不许删**）：

1. **9 个任务不是 8 个**（M5.0–M5.8；设计文档 §15 那行数错了，M5.8 顺手改）。
2. **不新增 `assets/golden/m5_obj_cube.bmp`**：锚 3 直接把 OBJ 场景**在进程内**渲染后与已有的 `m4_textured_cube.bmp` 比对（排除 HUD 条带）。少一张参考图 = 少一个"重新生成之后一切正常"的失效面，而交叉检查一点没少。`--golden-write-obj` 仍保留（给人看图与写 notes 用）。
3. **材质缺失一律降级为警告，从 D6 的致命清单里拿掉两条**：`usemtl` 指向未知材质名、以及 **`mtllib` 指向的文件读不到** ⇒ 警告 + 用 `defaultMaterial()`，几何照常显示。
   理由同 G-M5.1（"随便一个 OBJ 都立刻可见"）：现实中大量 OBJ 是**只发了模型没发材质**的，
   而这两种情形的几何都完全无损 —— 毙掉整个模型换不来任何正确性，只换来一句"加载失败"。
   **D6 的致命清单仍然保留**：索引越界（内存安全）、`f` 少于 3 个顶点、数字解析失败、贴图文件读不到。
   最后一条为什么不一起降级：贴图缺失会让画面**看起来正常但内容是纯色**，那是最难被发现的一类错，
   宁可让人去补文件。（`map_Kd` 指向的文件不存在 ⇒ 报错；材质表本身缺失 ⇒ 警告。这条分界写进 `model.h` 注释。）
4. **锚 1 拆成两条**：内联小 OBJ 版在 M5.2（守翻转这一行本身），`cube.obj` vs `makeCube` 的资产版在 M5.7（守这个文件真的忠实）。
5. **`goldenSampler()` 与 `goldenBackground()` 从 `golden_scene.cpp` 的文件内 `static` 提到头文件** —— M4 的教训（"两份循环会在某次改动里只被改到一处，而没人会读那个一直绿的测试"）适用于采样状态：OBJ golden 若自己再写一份 ClampEdge/Trilinear，改坏一边不会让另一边变红。
6. **`Texture` 的 uv 越界计数是 debug-only 的 `mutable` 成员，不是 `FrameStats` 字段** —— sampler 拿不到 stats（`sample()` 只收 `Texture`/`SamplerState`），而往 `FragmentUniforms` 塞 stats 指针会把生命周期问题带进热路径。按 Texture 归属还能说出"是哪张贴图"。M8 多线程前它是一个已知的良性竞争（注释里写明）。

---

## Task M5.0: 地基两件 —— 退出码夹到 1 + `drawRange`

纯重构 + 测试基础设施。**这个任务的验收标准是"什么都没变"**：两份 golden 逐字节不变、约 28 处 `draw` 调用点源码不动。

**Files:**
- Modify: `tests/main.cpp:18-20`
- Modify: `src/render/stats.h`（`FrameStats` 加字段 + `merge`）
- Modify: `src/render/render_backend.h:9-23`（`RenderBackend` / `BackendA`）
- Modify: `src/render/render_backend.cpp`（`BackendA::draw` 的循环头在 `:95`）
- Create: `tests/unit/test_draw_range.cpp`
- Modify: `CMakeLists.txt:22-40`（无新 core 文件）、`:87-126`（加 test 文件）

**Interfaces:**
- Consumes: `Mesh`（`positions/normals/uvs/colors/indices` + `indices` 每 3 个一组）、`PipelineState`、`bump(stats, &FrameStats::字段)` 这个现有自由函数（定义在 `render_backend.cpp` 内，`#ifndef NDEBUG` 之外恒跑）。
- Produces:
  ```cpp
  class RenderBackend {
    public:
    virtual void drawRange(const Mesh& mesh, const PipelineState& state,
                           uint32_t firstIndex, uint32_t indexCount) = 0;
    void draw(const Mesh& mesh, const PipelineState& state);   // 非虚：= drawRange(m, s, 0, indices.size())
  };
  struct FrameStats { /* ... */ int64_t drawCalls = 0; };      // merge() 里也要加
  ```

- [ ] **Step 1: 退出码夹到 1**

`tests/main.cpp` 的 `return srtest::runAll(opts);` 改成把失败数夹到 1（`>1 ? 1 : failed`），并在上面写一行注释：**POSIX 把退出码截断到 0–255，"退出码 = 失败用例数"意味着正好 256 条失败时 rc 是 0** —— 一条会让后面所有任务的红/绿判读整体失效的陷阱（终审 deferred #8）。

- [ ] **Step 2: 验证退出码这件事（一次性改动，不许提交）**

Run: `cmake --build build --config Debug && ./build/Debug/soft_raster_tests.exe --only harness_; echo "rc=$?"` → 期望 `rc=0`。
（**原文写的是 `--only test_smoke`，那是个不存在的用例名** —— 文件名是 `test_smoke.cpp`，里面的用例名全是 `harness_*`。
零匹配会让 rc=1，所以照原文跑会把"过滤器敲错"误读成"夹钳没生效"。这条错由 M5.0 的实现者抓出，见账本 Ruling 51。）
然后**临时**把 `tests/unit/test_smoke.cpp` 里任意一条断言改成 `SR_ASSERT(false)`，重跑，期望 `rc=1`（不是 2、不是 255），再 `git checkout tests/unit/test_smoke.cpp` 撤销。

- [ ] **Step 3: 写 `drawRange` 的失败测试**

`tests/unit/test_draw_range.cpp`，用 `tests/unit/test_util.h` 的 `Canvas`。三条：
```cpp
SR_TEST(draw_range_submits_only_the_index_slice)      // 6 索引的 mesh，drawRange(…,0,3) 与 (…,3,3)
                                                      // 各得到 trianglesSubmitted == 1
SR_TEST(draw_calls_are_counted_per_submission)        // 两次 drawRange ⇒ drawCalls == 2；一次 draw ⇒ 1
SR_TEST(draw_range_with_a_truncated_tail_drops_it_and_counts_degenerate)
                                                      // indexCount == 5 ⇒ 只画 1 个三角形，
                                                      // trianglesDegenerate == 1（**不崩、不越界**）
```
`trianglesSubmitted` 与 `trianglesDegenerate` 是 `FrameStats` 的现有字段（`stats.h:12-14`）。

- [ ] **Step 4: 跑测试确认失败**

Run: `cmake --build build --config Debug` → 期望**编译失败**（`drawRange` 不存在）。这是预期的红。若出现的是别的错误，先解决它再继续。

- [ ] **Step 5: 实现**

`render_backend.h` 按上面的 Interfaces 改；`render_backend.cpp` 把 `BackendA::draw` 改名 `drawRange`，循环头从
`for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)` 改成对 `[firstIndex, firstIndex+indexCount)` 区间迭代（**注意**：`firstIndex` 是**索引数组的下标**，不是顶点下标）。
`drawCalls` 在函数入口自增一次。`draw` 作为非虚包装放头文件里 inline。
尾部残缺走 `trianglesDegenerate` 而不是断言 —— 一个坏 group 不该带走整帧（Review Focus 3）。

- [ ] **Step 6: 全绿 + 证明"重构没改任何东西"**

```bash
cmake --build build --config Debug 2>&1 | grep -E "warning|error"   # 必须为空，否则本轮结论作废
ctest --test-dir build -C Debug --timeout 60                        # 全绿（M3 那张由 ±1 门禁判）
./build/Debug/soft-raster.exe --golden-write-textured build/chk-m4.bmp >/dev/null
md5sum build/chk-m4.bmp assets/golden/m4_textured_cube.bmp          # **这对 md5 必须相同**
```
**M3 那张不做 md5 门禁**（账本 Ruling 50）：`assets/golden/m3_cube.bmp` 最后写于 `3f8cc2a`，
之后 `src/render`/`src/scene` 又变了 11 个提交而参考图没重生成 ⇒ 今天生成出来与参考图差 2 个字节（±1），
**这在 `main` 上就成立**，不是任何重构造成的。M3 的一致性由 ctest 的 ±1/255 门禁判（C1′ 的原意）。
M4 那张实测**逐字节可复现**，而锚 3 正是依赖这个性质（见 M5.7 Step 5）。
是否要重新生成 M3 参考图 = 纪律 #4 的授权事项，留给产品方，见 M5.8 Step 5。

- [ ] **Step 7: 变异验证（Step 3 的三条必须有牙齿）**

依次单独制造这三个变异，每次期望**至少一条**断言变红，然后撤销：① `drawRange` 里把 `firstIndex` 当顶点下标用；② `drawCalls++` 挪到循环里面；③ 循环条件写成 `i + 2 <= end`（多吃一个索引）。

- [ ] **Step 8: 提交**

```bash
git add tests/main.cpp src/render/stats.h src/render/render_backend.h src/render/render_backend.cpp tests/unit/test_draw_range.cpp CMakeLists.txt
git commit -m "test+render(M5.0): 退出码夹到 1，并给后端一个索引区间入口 drawRange"
```

---

## Task M5.1: `color.h` 的浮点 sRGB 解码 + MTL 解析器

**Files:**
- Modify: `src/render/color.h`、`src/render/color.cpp`
- Create: `src/loader/mtl.h`、`src/loader/mtl.cpp`（**进 core**：`CMakeLists.txt:22-40`）
- Create: `tests/unit/test_mtl.cpp`

**Interfaces:**
- Consumes: `LinearColor`（`color.h:11`）、`srgb8ToLinear(uint8_t)`（`color.h:21`，**只吃字节**，所以浮点版是新工作）。
- Produces:
  ```cpp
  // 线性 ← sRGB 的浮点版：输入 clamp 到 [0,1]，NaN 返回 0。
  // 与 srgb8ToLinear 唯一的区别是**不经过 256 级量化**：MTL 的 Kd 本来就是浮点，
  // 走字节版会把 0.5 变成 128/255 再解码，白白吃掉一档。
  float srgbToLinear(float v);
  LinearColor srgbToLinear(const LinearColor& c);

  // src/loader/mtl.h
  struct Material {
      std::string name;
      LinearColor diffuse{1.0f, 1.0f, 1.0f};   // **默认纯白**，不是 pipeline 的 0.8 灰
      LinearColor ambient{0.0f, 0.0f, 0.0f};   // Ka：M5 存而不用
      LinearColor specular{0.0f, 0.0f, 0.0f};  // Ks：M6
      float shininess = 0.0f;                  // Ns：M6
      float opacity = 1.0f;                    // d / Tr：M5 存而不用（无 alpha 通路）
      std::string diffuseMap;                  // map_Kd 的文件名；core 不知道像素
  };
  const Material& defaultMaterial();           // 上面那个默认值，单例
  struct MtlStats { int unrecognized = 0; int optionTokensSkipped = 0; };
  bool parseMtl(std::string_view text, std::vector<Material>& out,
                std::vector<std::string>& warnings, MtlStats& stats, std::string& err);
  ```

- [ ] **Step 1: 写 `srgbToLinear(float)` 的失败测试**（放 `tests/unit/test_color.cpp` 追加）

`0.0f → 0.0f`、`1.0f → 1.0f`（**两端必须精确**，否则 golden 会整体偏一档）、`0.5f ≈ 0.21404f (±1e-4)`、
`1.5f` clamp 成 `1.0f`、`-0.3f` clamp 成 `0.0f`、`NaN → 0.0f`、单调性（采样 33 点，逐点不减）。

- [ ] **Step 2: 跑测试确认失败** → `ctest` 报未声明的 `srgbToLinear`。

- [ ] **Step 3: 实现 `srgbToLinear`**

标准 IEC 分段公式（`v <= 0.04045 ? v/12.92 : powf((v+0.055f)/1.055f, 2.4f)`），先 clamp 再算，NaN 用 `!(v > 0.0f || v < 0.0f)` 之类的形式挡住（**与 `plane_eq.h` 的 NaN 守卫同一个写法风格**，`std::isnan` 也行）。
注释里写明**为什么解码在两头都必须精确**：`Kd 1 1 1` 解成非 1 会让"无 tint"这件事不再成立，而 golden 是零容差比对。

- [ ] **Step 4: `parseMtl` 的失败测试**（`tests/unit/test_mtl.cpp`，内联字符串夹具，无 IO）

```cpp
SR_TEST(mtl_parses_newmtl_blocks_in_file_order)
SR_TEST(mtl_parses_kd_as_srgb_encoded_display_value)      // "Kd 0.5 0.5 0.5" → 每通道 ≈ 0.21404
SR_TEST(mtl_parses_ka_ks_ns_d_and_tr)                     // 存进字段，不参与着色
SR_TEST(default_material_is_white_not_gray)               // 0.8 灰会让每个无 MTL 的模型暗一档
SR_TEST(mtl_unknown_attribute_is_warning_and_count)       // "refl ..." ⇒ 不报错、unrecognized==1
SR_TEST(mtl_map_kd_skips_blender_style_options)           // "map_Kd -s 1 1 1 brick.png" → "brick.png"
SR_TEST(mtl_map_kd_skips_unknown_option_tokens)           // "map_Kd -weird 2 brick.png" → "brick.png" + warning
SR_TEST(mtl_map_kd_keeps_spaces_in_filename)              // "map_Kd -s 2 2 2 my brick.png" → "my brick.png"
SR_TEST(mtl_map_kd_without_filename_is_error)             // 光杆 "map_Kd" → false + err 带行号
SR_TEST(mtl_empty_text_yields_no_materials_not_an_error)  // 空文件合法：让 obj 侧兜 defaultMaterial
```

- [ ] **Step 5: 跑测试确认失败**

- [ ] **Step 6: 实现 `parseMtl`**

逐行切：`#` 注释、空行、`newmtl`、`Kd/Ka/Ks`（3 个 float）、`Ns/d/Tr`（1 个 float）、`map_Kd`（走 D17 状态机）、其余 → `stats.unrecognized++` + `warnings.push_back("第 N 行: 未知材质属性 'xxx'")`。
**数字解析失败是错误**（带行号 + 原文片段），与 D6 一致：已知指令的坏参数 ⇒ `return false`、`out` 清空、`err` 非空。

- [ ] **Step 7: 实现 D17 的选项状态机**

规则在设计文档 D17 里，逐条落地：arity 表只决定"少吞还是多吞"、不是白名单；未知 `-xxx` 丢 1 个 token 并顺带吞掉紧随其后的数字 token；**第一个不以 `-` 开头的 token 起，剩余用单空格重拼 = 文件名**；拼出来为空才是错误。
`map_Kd` 之后**只有**选项没有文件名时，`err` 里要写出"这是真实导出器的常见形状，但缺了文件名"（Review Focus 2）。

- [ ] **Step 8: 全绿 + 零警告 + 变异验证**

变异：① `srgbToLinear` 直接 `return v`（Step 4 的 Kd 断言必须红）；② 选项状态机把 `-s` 之后的 3 个数字当文件名起点（`mtl_map_kd_skips_blender_style_options` 必须红）；③ 文件名重拼改成"取最后一个 token"（含空格那条必须红）。

- [ ] **Step 9: 提交** `git add src/render/color.* src/loader/mtl.* tests/unit/test_color.cpp tests/unit/test_mtl.cpp CMakeLists.txt`

---

## Task M5.2: OBJ 解析器 + 七项病理标本

一个任务的量最大，因为它的所有输入都是内联字符串、所有输出都是纯数据 —— 这正是 D2 换来的东西。

**Files:**
- Create: `src/loader/obj.h`、`src/loader/obj.cpp`（进 **core**）
- Create: `tests/unit/test_obj.cpp`
- Modify: `CMakeLists.txt:22-40`、tests 源文件列表

**Interfaces:**
- Consumes: `Mesh`/`BBox`（`scene/mesh.h:14-28`，含 `recomputeBounds()`）、`Material`（M5.1）、`render/vertex.h` 的 SoA 约定。
- Produces:
  ```cpp
  struct MeshGroup { uint32_t indexOffset = 0; uint32_t indexCount = 0; int32_t material = 0; };
  struct Model {
      Mesh mesh;
      std::vector<Material> materials;     // = parseObj 入参 + 可能的 default 兜底，见下
      std::vector<MeshGroup> groups;       // indexCount 恒为 3 的倍数
  };
  struct ObjStats {
      int triangles = 0, faceCorners = 0, uniqueVertices = 0;
      int trianglesFromQuads = 0, trianglesFromNGons = 0;
      int facesWithoutNormals = 0, facesUnrolled = 0;
      int uvOutOfRange = 0, negativeIndices = 0;
      int unrecognized = 0;                // s/vp/bevel/cset/未知指令
  };
  bool parseObj(std::string_view text, std::span<const Material> materials, Model& out,
                std::vector<std::string>& warnings, ObjStats& stats, std::string& err);
  // 只扫 mtllib 名字，不解析几何：给 loadModel 决定"该读哪个 .mtl"用（D2：parse 不做 IO）
  bool scanObjMtllib(std::string_view text, std::vector<std::string>& out);
  ```
  **材质表的两条兜底规则**（收紧 3）：`materials.empty()` ⇒ `out.materials` 只含一个 `defaultMaterial()`，全部 group 指它；
  `usemtl X` 而 X 不在表里 ⇒ 警告 + 在 `out.materials` **末尾追加一次** `defaultMaterial()` 并指它（多次未知名复用同一个追加项）。

- [ ] **Step 1: 位置/法线/uv 的读取与三元组去重（先只测这一层）**

```cpp
SR_TEST(obj_parses_positions_normals_and_uvs_into_soa_arrays)
SR_TEST(obj_dedups_vertices_by_the_v_vt_vn_triple)     // 一个共享角被两面引用 ⇒ uniqueVertices < faceCorners
SR_TEST(obj_negative_indices_count_from_the_end)       // "f -3 -2 -1"
SR_TEST(obj_trims_each_directive_to_the_fields_it_has)// "f v/vt/vn vt/vn" 等混合形式
```
去重的键是 `(vIdx, vtIdx, vnIdx)` 三个 int64 打包（缺项用哨兵值）；map 用 `std::unordered_map`。**只发生在加载期**，不在热路径（D7）。

- [ ] **Step 2: 面类型**

```cpp
SR_TEST(obj_triangulates_quads_as_fans)                 // f a b c d ⇒ (a,b,c) (a,c,d)，D8
SR_TEST(obj_triangulates_pentagons_as_fans_and_counts)  // 5 角 ⇒ 3 三角形，trianglesFromNGons == 3
SR_TEST(obj_drops_degenerate_faces_without_failing)     // "f 1 1 1" / 两点重合 ⇒ 丢弃 + 计数（不是错误：导出器常写）
SR_TEST(obj_rejects_faces_with_fewer_than_three_vertices) // "f 1 2" ⇒ 错误 + 行号
```
**注意 `f` 少於 3 个顶点是 D6 的错误，而退化面（≥3 项但引用重复）是警告** —— 两者的区分写进注释，否则下一个读代码的人会统一处理。

- [ ] **Step 3: 缺 `vn` 的面走 unroll（D7）**

```cpp
SR_TEST(obj_unrolls_faces_that_have_no_normals)         // 该面 3 个独占顶点，normal = 归一化面法线
SR_TEST(obj_face_normal_matches_the_winding_of_the_face)// 法线方向 = cross(b-a, c-a) 的右手方向
SR_TEST(obj_normals_and_uv_stay_shared_when_vn_is_present)  // 与 Step 1 那条方向相反的断言
```
顶点"存在属性"不等于"属性正确"：`render_backend.cpp:102-105` 对缺失属性有兜底（normal→`(0,1,0)`、uv→`(0,0)`），
**但那是安全网不是功能** —— 缺 vn 必须在这里补真法线，否则 M6 一上光照，整个模型会朝 +Y 打光。这条理由写进注释。

- [ ] **Step 4: 材质分组（`usemtl`）与 group 边界**

```cpp
SR_TEST(obj_groups_coalesce_only_adjacent_runs)         // A B A ⇒ **3** 个 group，不重排（spec §7.5"最小的 batching"）
SR_TEST(obj_every_group_index_count_is_a_multiple_of_three)
SR_TEST(obj_unknown_material_name_warns_and_uses_default)  // 收紧 3：不是错误
SR_TEST(obj_material_table_gets_default_when_no_mtl)
```

- [ ] **Step 5: uv 越界统计 + 越界值原样保留**

`vt 1.5 -0.2` ⇒ 数值**照存**（翻转之后），`stats.uvOutOfRange++`。不 clamp：clamp 会把"文件写错了"这件事藏起来，而 wrap 模式本来就是设计给越界用的（Review Focus 4）。

- [ ] **Step 6: D9 的 v 翻转 + 锚 1 的内联版**

一行 `uvOut = { u, 1.0f - v }`，写在解析器的最后一站，注释引 `mesh.cpp:51-52` 那条既有契约。
```cpp
SR_TEST(obj_flips_vt_because_obj_v_zero_is_the_image_bottom)
// 夹具必须**非对称**：vt 写 0.25 而不是 0.5（0.5 在翻转下不变 ⇒ 假测试）
// 断言：输入 vt 0.25 → 输出 uv.y == 0.75f；输入 1.0 → 0.0；输入 0.0 → 1.0
```

- [ ] **Step 7: 语法层的容错**

```cpp
SR_TEST(obj_accepts_crlf_and_bare_cr)
SR_TEST(obj_strips_a_utf8_bom)                       // Blender/某些导出器会写 BOM
SR_TEST(obj_accepts_tabs_and_repeated_spaces)
SR_TEST(obj_ignores_unknown_directives_and_counts)   // s / vp / bevel / cset ⇒ 警告不是错误
SR_TEST(obj_reports_the_line_number_of_a_bad_number) // "v x y z" ⇒ err 含 "第 3 行"
SR_TEST(obj_rejects_out_of_range_index_with_a_message)  // 这条最关键：positions[idx] **没有**越界保护
```
最后一条的理由写进测试注释：`render_backend.cpp:102-104` 只给 normal/uv/color 兜底，
**position 是直接下标** ⇒ 越界索引是解析器必须挡住的**内存安全问题**，不是格式洁癖。

- [ ] **Step 8: 七项"病理标本"合成一份完整文本，一次过**

`SR_TEST(obj_parses_a_real_world_kitchen_sink_file)`：一份内联 OBJ，同时含
① 跨面共享的 `v/vt/vn` 三元组 ② 一个五边形 ③ 一个缺 vn 的面 ④ 负索引
⑤ 一条越界 `vt` ⑥ `usemtl` 指向不存在的材质 ⑦ 尾部 `f 1 2`（应报错）。
**分两次**：先把 ⑦ 拿掉断言 `parseObj == true` 且 `stats` 的每个计数等于手算出的期望值
（期望值必须**手算写在注释里**，不是"跑一遍把结果抄回来"—— 后者是给自己出题）。
⑦ 存在时断言 `false` + err 带行号。

- [ ] **Step 9: 变异验证（本任务的重点，逐条做）**

至少这五个变异，每个都要让**指定**的那条断言变红，且**只**该那条变红：
① 删掉 `1.0f - v`（Step 6）；② 把去重键退化成只用 `vIdx`（Step 1 的 dedup 断言 + Step 3 的"共享时不 unroll"必须红）；
③ 把 unroll 分支删掉（Step 3 的顶点数断言必须红）；④ quad 的扇形分解改成 `(a,b,c),(b,c,d)`（Step 2 + Step 8 的计数必须红）；
⑤ 越界索引改成"clamp 到合法范围"（Step 7 的拒绝断言必须红）。
**②③ 是一对方向相反的断言**（去重写坏 ⇒ 顶点变少；unroll 写坏 ⇒ 顶点变多），所以两条都必须存在。

- [ ] **Step 10: 全绿 + 提交** `feat(loader): 手写 OBJ 解析器 —— 三元组去重、缺 vn 就 unroll、v 翻在格式门口（M5.2）`

---

## Task M5.3: `writeTga` + `--emit-checker-tga` + `image_stb` 的路径与尺寸 —— 锚 2

**Files:**
- Create: `src/loader/tga_writer.h`、`tga_writer.cpp`（进 **core**，与 `bmp_writer.cpp` 同侧）
- Modify: `src/loader/image_stb.h:15`、`image_stb.cpp`（`path` 化 + `stbi_info` 先问 + `_wfopen`）
- Modify: `src/loader/image_tga.h:33`（`loadTgaFile` 收 `std::filesystem::path`）
- Modify: `src/app/cmdline.h`、`cmdline.cpp`、`src/app/main.cpp`
- Create: `tests/unit/test_tga_writer.cpp`；Modify: `tests/unit/test_image_stb.cpp`、`test_image_tga.cpp`
- Create: `assets/tga/checker.tga`、`assets/obj/README.md`

**Interfaces:**
- Consumes: `ImageRGBA8`（`texture.h:11-15`，top-down）、`makeCheckerImage(4,32,{})`、`kGoldenCheckerCells/CellPx`。
- Produces:
  ```cpp
  bool writeTga(const std::string& path, const ImageRGBA8& img, std::string& err);
  // 未压缩 type 2、32bpp、**origin bit = 0（bottom-up）**，alpha 存第 4 字节。
  bool loadImageFile(const std::filesystem::path& path, ImageRGBA8& out, std::string& err);  // 签名换型
  bool loadTgaFile(const std::filesystem::path& path, ImageRGBA8& out, std::string& err);    // 同上
  inline constexpr int kMaxTextureSide = 4096;   // src/render/texture.h
  ```
  `path` 化是**源码兼容**的（`std::string` 隐式转换），现有调用点与测试不用改。

- [ ] **Step 1: `writeTga` 的失败测试**

```cpp
SR_TEST(tga_writer_roundtrips_a_small_image_byte_for_byte)      // 3x2 图，write → parseTga → 相等
SR_TEST(tga_writer_marks_bottom_up_so_reading_must_flip)        // 断言 descriptor bit5 == 0，
                                                                 // 且读回的 row0 == 写的**最后**一行
SR_TEST(tga_writer_rejects_an_empty_image)                      // width/height <= 0 ⇒ false + err
```
第二条是锚 2 的一半：**故意**写 bottom-up，"读回来必须翻一次"才是我们要守的路径（设计 §10）。

- [ ] **Step 2: 跑测试确认失败 → Step 3: 实现 `writeTga`**（18 字节头 + RGBA 直写，无 RLE；BGRA/RGBA 的字节序这件事必须在测试里用**非灰度**的像素钉住）

- [ ] **Step 4: `--emit-checker-tga <path>`**

`cmdline.h` 加 `std::string emitCheckerTga;`，`cmdline.cpp` 照 `--golden-write` 的写法加解析（**独立 flag、不带默认值** —— Ruling 38 的理由在这里同样成立），`usageString()` 加一行，`main.cpp` 在 golden 分支之前加一个 `if (!o.emitCheckerTga.empty()) { ... }` 分支：`makeCheckerImage(kGoldenCheckerCells, kGoldenCheckerCellPx, {})` → `writeTga`。

- [ ] **Step 5: 生成资产并验锚 2**

```bash
cmake --build build --config Debug
./build/Debug/soft-raster.exe --emit-checker-tga assets/tga/checker.tga
```
```cpp
SR_TEST(anchor2_the_committed_tga_equals_the_program_checker)   // tests/unit/test_tga_writer.cpp
// loadTgaFile(SR_SOURCE_DIR "/assets/tga/checker.tga") 的 rgba **逐字节等于**
// makeCheckerImage(kGoldenCheckerCells, kGoldenCheckerCellPx, {}).rgba
// 尺寸与 levelCount 顺带断言：128x128、链 8 级
```
跑通后把 `checker.tga` 提交，并在 `assets/obj/README.md` 里写：由哪个 flag、哪个提交、哪个函数生成，
一条命令可复现（Ruling 33 的 provenance 纪律）。

- [ ] **Step 6: `image_stb` 改成 path + 先问后分配 + `_wfopen`**

`stbi_info` 只读头部拿宽高 ⇒ 超过 `kMaxTextureSide` 直接 `false` + 一句带常量名的错（D13）；
`stbi_load_from_file(FILE*, ...)`，FILE 由 `_wfopen` 打开（`#ifdef _MSC_VER`，否则 `std::fopen`）；
`#include <cstdio>`、`<filesystem>`，**不许** `<windows.h>`（C4）。

- [ ] **Step 7: `loadImageFile` 的两条新测试**

```cpp
SR_TEST(stb_rejects_a_file_header_larger_than_the_texture_cap)  // 造一个 8192x8192 的**头**（不写像素），
                                                                 // 断言 false 且 err 含 "4096" —— 这条证明
                                                                 // "先问再分配"问在分配之前
SR_TEST(stb_loads_a_png_from_a_chinese_named_directory)         // D12：临时目录用中文名，写一张 BMP/图进去再读
```
第二条**在 Windows 上必须真跑通**（`_wfopen` 之前它会是 ANSI 误解码 → 打不开）。若 MSYS2 侧建中文名临时目录不稳，用 `#ifdef _MSC_VER` 限定该用例并在注释里写明（C1′ 允许 g++ 侧少判几条，但要说明）。

- [ ] **Step 8: 变异验证**：① `writeTga` 把 origin bit 设成 1 ⇒ Step 1 第二条与锚 2 各红一处；② 把 `stbi_info` 的检查删掉 ⇒ Step 7 第一条红（且**这次是真的分配了**，所以顺带看一次它的耗时差）。

- [ ] **Step 9: 全绿 + 提交**（含 `assets/tga/checker.tga`）

---

## Task M5.4: `loadModel` 装配 + `drawModel` 逐组提交

**Files:**
- Create: `src/loader/model.h`、`model.cpp`（进 **loader** target，`CMakeLists.txt:65-68`）
- Modify: `src/render/texture.h`（`mutable` 越界计数）、`src/render/sampler.cpp`（`toTexelF` 的 nullptr 分支 bump）
- Create: `tests/unit/test_model.cpp`
- Create: `assets/obj/twomats.obj`、`twomats.mtl`
- Modify: `tests/unit/test_texture.cpp`（补 deferred #3 的 alpha mip 断言）

**Interfaces:**
- Consumes: `parseObj`/`scanObjMtllib`（M5.2）、`parseMtl`（M5.1）、`loadImageFile`（M5.3）、`makeTexture`（`sampler.h:21`）、`BackendA::drawRange`（M5.0）。
- Produces:
  ```cpp
  struct LoadedModel {                      // move-only 语义：贴图可能几百 MB，别按值传来传去
      Model model;
      std::vector<Texture> textures;
      std::vector<int32_t> texturePerMaterial;   // 下标进 textures；-1 = 无贴图
      ObjStats stats;
      std::filesystem::path source;
  };
  bool loadModel(const std::filesystem::path& objPath, LoadedModel& out, std::string& err);
  void drawModel(RenderBackend& backend, const LoadedModel& m, RenderTarget& target,
                 const VertexUniforms& vu, const FragmentUniforms& base, FrameStats* stats);
  ```
  **`textures` 的不变式**：`loadModel` 返回后**不得再 push**（`FragmentUniforms::texture` 是裸指针，`pipeline.h:44`，扩容 = 悬垂）。写成 `model.h` 的注释 + `drawModel` 入口的一条 `assert`-式断言（用 `stats` 计数而不是 `<cassert>`，与现有风格一致）。

- [ ] **Step 1: `loadModel` 的失败测试（用真文件，这一层测的就是 IO）**

```cpp
SR_TEST(model_loads_an_obj_with_a_sibling_mtl_and_tga)      // 临时目录里放 obj/mtl/tga 三件
SR_TEST(model_resolves_map_kd_relative_to_the_mtl_directory) // 贴图在 mtl 旁边而非 obj 旁边（Blender 的 apply 修改器常造成这种布局）
SR_TEST(model_missing_texture_file_is_an_error_naming_it)   // err 里必须有那个文件名（收紧 3：这条**不**降级）
SR_TEST(model_missing_mtl_file_is_a_warning_and_still_draws) // 收紧 3：只发了 obj 没发 mtl ⇒ 几何照常显示 + 警告
SR_TEST(model_without_mtllib_gets_one_default_material)     // 纯 OBJ：能画，纯色白
SR_TEST(model_dedups_two_materials_sharing_one_image)       // textures.size() == 1 而 materials.size() == 2
SR_TEST(model_canonicalizes_so_dot_slash_and_bare_name_share_one_texture)  // D11 修订的那条
```

- [ ] **Step 2: 跑测试确认失败 → Step 3: 实现 `loadModel`**
顺序：`ifstream(path)` 读 OBJ → `scanObjMtllib` → 逐个 mtl 相对 **OBJ 目录**读入并 `parseMtl` → `parseObj` → 每个非空 `diffuseMap`：相对 **MTL 目录** → `weakly_canonical` → 查重表 → `loadImageFile` → `makeTexture(img, /*colorDataIsSrgb=*/true)`。
**warnings 在这里打印**（含文件名），因为这是唯一同时看得见"文件"与"文本"的一层。

- [ ] **Step 4: `drawModel` 的失败测试 —— 多 draw call 是真的按材质换了状态**

```cpp
SR_TEST(draw_model_uses_one_draw_call_per_group)            // twomats: drawCalls == 3（Review Focus 里"数三角形分不出来"的那条）
SR_TEST(draw_model_paints_each_group_with_its_own_diffuse)  // 两个盒子两种 Kd：中心像素各归其色
SR_TEST(draw_model_falls_back_to_diffuse_when_no_texture)   // 无 map_Kd 的组 ⇒ 纯色，不是黑（Ruling 32 的语义）
SR_TEST(draw_model_keeps_texture_and_uv_for_textured_groups)
```
`drawModel` 里**最容易写错的一件事**是"循环填材质"（写成永远 `materials[0]`），所以第 2、3 条必须都有。

- [ ] **Step 5: sRGB 的双重解码守卫（Review Focus 5）**

```cpp
SR_TEST(srgb_file_texture_and_linear_kd_land_on_the_same_brightness_as_a_white_tint)
// 一张全 0.5 灰的 TGA + Kd 1 1 1  vs  一张全 1.0 的程序图 + Kd 1 1 1：
// 前者采出来必须 ≈ 0.2140（字节 128 解码一次），后者 ≈ 1.0。
// 谁哪天在加载时又解了一次，这条会把它拉到 0.033 而不是安静地偏暗。
```

- [ ] **Step 6: deferred #3 —— mip 链对 alpha 也做平均**

`tests/unit/test_texture.cpp` 补：一张 alpha 交替 0/255 的 2x2 图 ⇒ 下一级 alpha == 127 或 128（±1），
**rgb 与 alpha 用同一次 2x2 平均**（终审 deferred #3 说"M4 的分不出'平均'与'保持'"，靠的就是 alpha 不恒 255 的夹具）。

- [ ] **Step 7: deferred #2 —— `Texture` 的越界纹素采样计数**

`texture.h` 加 `mutable int64_t outOfRangeTexelSamples = 0;`（**`#ifndef NDEBUG` 里才 bump**，注释写明 M8 之前这是已知的良性竞争、以及为什么不用 `FrameStats`：收紧 6）。`toTexelF` 的 `px == nullptr` 分支 bump 它。
测试：故意构造一个 `levels` 少一级的 Texture，采样后断言计数 > 0 —— **这条测试本身要能在 `NDEBUG` 下跳过**（`#ifndef NDEBUG` 包住断言），否则 Release 侧会红。

- [ ] **Step 8: 中文目录 + 带空格贴图名走完整条装配**（`SR_TEST(model_loads_from_a_chinese_directory_with_a_spaced_texture_name)`，`map_Kd -s 1 1 1 my brick.png` 的形状）

- [ ] **Step 9: 变异验证 + 全绿 + 提交**

---

## Task M5.5: `fitCamera` —— 三尺度 NDC 断言（D16）

**Files:**
- Create: `src/scene/fit.h`（纯 header）
- Create: `tests/unit/test_fit.cpp`
- 无需改 math：`Mat4::operator*(const Vec4&)` 已在 `src/math/mat.h:34,96`，`makePerspective` 产出的 NDC z ∈ [-1,1] 已在 `src/math/transform.h:11` 写明（预检核实）

**Interfaces:**
- Consumes: `BBox`（`math/bbox.h:12-34`，`center()`/`radius()` 已处理空盒）、`OrbitCamera`（`scene/camera.h:17-22`，有 `target`）、`makeLookAt`/`makePerspective`。
- Produces:
  ```cpp
  struct FittedCamera { OrbitCamera camera; float nearPlane = 0.1f; float farPlane = 100.0f; };
  // 只改 distance / target / near / far；**yaw 与 pitch 从 base 继承**（D：按 F 的人不是为了被重置视角）
  FittedCamera fitCamera(const BBox& bounds, float fovYRadians, float aspect, const OrbitCamera& base);
  ```
  公式（设计 §11 + D16）：`r = bounds.radius()`，`distance = r / sinf(min(fovY,fovX)/2)`，
  `fovX = 2*atanf(tanf(fovY/2)*aspect)`，`near = r/1000`，`far = r*10`，`target = bounds.center()`。
  `model` 保持单位阵：fit 只发生在相机侧，golden 的姿态常量（`golden_scene.h:23-27`）继续有效。

- [ ] **Step 1: 数值逐字段断言**

```cpp
SR_TEST(fit_matches_the_documented_formula_on_the_m4_cube)
// 1.6 立方体：r = 0.8*sqrt(3) ⇒ distance ≈ 2.77128、near ≈ 0.001386、far ≈ 13.8564
// 这四个数字**就是** D15 里"fit 相机与 M4 姿态是两张无关的图"的证据，写进注释
SR_TEST(fit_keeps_yaw_and_pitch_of_the_base_camera)
```

- [ ] **Step 2: 三尺度 NDC 包含断言（这是 D16 的主角）**

`SR_TEST(fit_puts_the_whole_bbox_inside_ndc_at_three_scales)`：对 `0.001f`、`1.6f`、`1600.0f` 三种边长的立方体，
fit 之后把**包围盒 8 个角**用 `proj * view * vec` 投到 clip 再除 w，断言每个角的 `x/y/z ∈ [-1,1]`。
注释里写清三个数各挡什么：`near` 太大 ⇒ 近角被裁、`far` 太小 ⇒ 远角被裁、`distance` 太小 ⇒ 上下或左右溢出。

- [ ] **Step 3: 极端形状与退化输入**

```cpp
SR_TEST(fit_survives_an_empty_bbox_without_nan)              // 空盒 radius()==0 ⇒ distance 0/near 0：**特殊处理**成"退回默认 5.0/0.1/100"，别把 0 传进 proj（w==0 会让整帧变 NaN）
SR_TEST(fits_vertical_fov_wins_in_a_wide_window_and_horizontal_in_a_tall_one)  // aspect 16:9 与 9:16 各一次
```

- [ ] **Step 4: 变异验证**：把 `distance` 公式改成 `r / tanf(...)`（差 1/cos ⇒ 高瘦窗口那条会红）、`near = r/1000` 改成 `r/100`（三尺度那条会红）。

- [ ] **Step 5: 全绿 + 提交** `feat(scene): fitCamera —— 包围球 fit，yaw/pitch 归用户（M5.5）`

---

## Task M5.6: app 接线 —— 载入真模型（⚠ 本任务末尾有产品方肉眼门）

**Files:**
- Create: `src/app/scene.h`、`scene.cpp`、`src/app/path.h`、`path.cpp`
- Modify: `src/app/cmdline.h:10`（`obj` 语义）、`cmdline.cpp`、`src/app/main.cpp`（`renderFrame` 里那个 `static sr::Mesh cube` 在 `:109-113`，两处"从 M5 开始生效"的提示在 `:230`、`:246`）、`src/app/win32_window.cpp`（`R` 键已映射在 `:210`；拖放新增）、`src/app/input.h:53`（`reloadModel` 已存在，无需新增字段）
- Modify: `CMakeLists.txt`（tests 列表加 `src/app/scene.cpp` + `tests/unit/test_scene.cpp`；app 是 glob，`path.cpp` 自动进）

**Interfaces:**
- Consumes: `loadModel`/`drawModel`/`LoadedModel`（M5.4）、`fitCamera`（M5.5）、`OrbitCamera`、`makeCube`/`applyPositionColors`（兜底模型）。
- Produces:
  ```cpp
  std::filesystem::path nativePath(const std::string& utf8);   // src/app/path.h，Win32，约 8 行
  struct SceneState {                                          // src/app/scene.h，**不含任何 Win32 头**
      std::unique_ptr<sr::LoadedModel> model;   // nullptr = 画程序生成的立方体（M2–M4 的老画面）
      sr::OrbitCamera camera;
      float nearPlane = 0.1f, farPlane = 100.0f;
      bool fitEnabled = true;
      std::string errorLine;                    // 空 = 无错误；成功后清空
      std::string modelLabel;                   // HUD 用：文件名 或 "CUBE"
      bool load(const std::filesystem::path& obj);   // 失败**保留旧 model**，只写 errorLine
      void refit(int width, int height);
  };
  ```

- [ ] **Step 1: `SceneState` 的失败测试**（`tests/unit/test_scene.cpp`；`scene.cpp` 无 Win32 ⇒ 能像 `cmdline.cpp` 一样直接列进 tests）

```cpp
SR_TEST(scene_load_replaces_the_model_on_success)
SR_TEST(scene_load_keeps_the_previous_model_and_sets_error_line_on_failure)  // A1/半加载的反面：失败不许把画面清空
SR_TEST(scene_successful_load_clears_the_error_line)
SR_TEST(scene_refit_does_not_touch_yaw_or_pitch)
```

- [ ] **Step 2: 跑测试确认失败 → Step 3: 实现 `scene.cpp`**
`load()` 内部：`auto fresh = std::make_unique<sr::LoadedModel>(); std::string err; if (!sr::loadModel(p, *fresh, err)) { errorLine = ...; return false; }` —— **先建新的再换指针**，
这是"失败保留旧模型"的实现方式，别写成先 `model.reset()`。

- [ ] **Step 4: `renderFrame` 改成走 `SceneState`**

`main.cpp:107-147` 那个块：`model == nullptr` 时保持现有立方体路径（**逐字节不变的 golden 与 M4 画面是这个分支的回归证据**）；
非空时 `drawModel(backend, *model, target, vu, baseFu, &stats)`，`vu.proj` 用 `nearPlane/farPlane`（不再硬编码 0.1/100）。
`baseFu.sampler.filter = g_filter; baseFu.sampler.useMip = g_useMip;`（`1`/`2` 键对导入模型同样生效 —— 这是 M4 那两个旋钮的自然延伸，别在导入路径上重新发明）。
导入模型的 wrap 用 **Repeat**（外部模型的 uv 常越界，ClampEdge 是"每面一整张贴图"的特例语义，只属于 golden 立方体），并把它写在注释里。

- [ ] **Step 5: `--obj` 生效 + `F` + `--no-fit`**

`--obj` 空 ⇒ 老路径；非空 ⇒ `SceneState::load(nativePath(o.obj))`，失败打印 + HUD 显示，**程序继续跑**（不是退出：看不见模型比看见错误更糟）。
`F` 键（`input.h` 需要加一个 `bool refit`，`win32_window.cpp` 里照 `R` 的写法加映射）⇒ `scene.refit(w,h)`。
`--no-fit` 已被 `cmdline.cpp:95` 解析（现在没人读它）⇒ 接成 `fitEnabled = false`。

- [ ] **Step 6: `R` 重载**

`input.reloadModel` 已有；接成 `scene.load(同一个 path)`，成功后再 `refit`。

- [ ] **Step 7: 拖放 `.obj`**

`win32_window.cpp`：`DragAcceptFiles(hwnd, TRUE)` + `WM_DROPFILES` → `DragQueryFileW` → `nativePath` 走同一个 `SceneState::load`。
**非 `.obj` 后缀 ⇒ 写 `errorLine`，不静默忽略**（A4）。可测的那一半在 loader，这里只剩"shell 给了个路径字符串"。

- [ ] **Step 8: HUD 与终端**

第二行追加 `DC <n>` 与 `UNRECOGNIZED <n>`；`errorLine` 非空时**第二行整行给错误**，持续到下次成功载入。
`nativePath` 的调用点注释写明它是全仓库唯一的编码转换处（收紧 D18 第 3 条）。

- [ ] **Step 9: UTF-8 argv 桥（D18）**

`main.cpp` 开头：`GetCommandLineW()` + `CommandLineToArgvW()` → 转 UTF-8 的 `std::vector<std::string>` + `argv` 指针数组 → 喂现有 `parseCmdline`（`cmdline.cpp` 与它的测试**一行不改**）。
`AppOptions::obj` 的注释改成"UTF-8"；`main.cpp:379` 那句"由调用方决定"删掉。

- [ ] **Step 10: 三侧全绿 + 提交**

```bash
cmake --build build --config Debug 2>&1 | grep -E "warning|error"
ctest --test-dir build -C Debug --timeout 60
cmake --build build-gcc 2>&1 | grep -E "warning|error"; ctest --test-dir build-gcc --timeout 60
```

- [ ] **Step 11: ⛔ 停下来，等产品方肉眼验收（验证纪律第 4 条，不许跳过）**

请产品方跑这三条，并回报看到的东西：
```bash
./build/Debug/soft-raster.exe --obj assets/obj/twomats.obj      # 两个盒子、两种颜色、三组
./build/Debug/soft-raster.exe                                    # 老立方体画面必须与 M4 一模一样
./build/Debug/soft-raster.exe --obj <一个真的外部模型>            # 拖放也试一次；顺手按 F / R / 1 / 2 / W
```
要产品方确认的四件事：**装得下**（fit）、**朝向对**（贴图没上下颠倒 —— 只有人能一眼看出双翻转抵消之外的第三类错）、
**关掉 mip 会闪 / 打开不闪**（M4 的能力没有因为换路径而丢失）、**`W` 翻面时里外正常**（D10 的 winding 风险唯一能被肉眼排除）。
产品方说"看到了"之前，**不得进入 M5.7，不得生成任何 golden**。

---

## Task M5.7: `cube.obj` + 锚 1 的资产版 + 锚 3（与 M4 参考图零容差相同）

**Files:**
- Create: `assets/obj/cube.obj`、`assets/obj/cube.mtl`
- Create: `src/loader/obj_scene.h`、`obj_scene.cpp`（进 **loader**）
- Modify: `src/scene/golden_scene.h/.cpp`（把 `goldenSampler()` 与 `goldenBackground()` 从头文件内 `static` 提出来 —— 收紧 5）
- Modify: `src/app/cmdline.h`、`cmdline.cpp`、`main.cpp`（`--golden-write-obj <out>`，与 `--obj` 配对；缺 `--obj` ⇒ 明确报错）
- Create: `tests/unit/test_obj_golden.cpp`
- Modify: `tests/golden/golden.cpp`（若走 CTest 侧；本任务的比对是**进程内**，所以只在 golden.cpp 加一行注释指向新用例）

**Interfaces:**
- Consumes: `loadModel`（M5.4）、`drawModel`（M5.4）、`goldenModel/goldenView/goldenProjection/kGoldenWidth/Height`、`goldenSampler()/goldenBackground()`（本任务提出去）、`texturedFS`。
- Produces:
  ```cpp
  // src/loader/obj_scene.h —— 不读文件、不要路径：姿态与采样状态全部来自 core 的 golden_scene.h
  void renderObjGoldenScene(const LoadedModel& m, RenderTarget& target, FrameStats* stats);
  ```
  HUD 那行写 `"GOLDEN M5 320X180"` —— 与 M4 同长度、只差一个字（`golden_scene.cpp:120-124` 的既有做法，
  目的是让"拿错参考图"不可能静默通过）。

- [ ] **Step 1: 手写 `cube.obj` + `cube.mtl`**

规则（不抄数字，抄**推导**）：8 个 `v`（±0.8）、6 个 `vn`（±1 轴）、24 个 `vt`（每面 4 个，**按 OBJ 惯例：v=0 是贴图下边缘**）、
6 个 `f v/vt/vn` 四边形，面顺序 **+X, -X, +Y, -Y, +Z, -Z**（`mesh.cpp:15` 的既有约定，M2 的剔除测试依赖它），
每面的角序使 D8 的扇形分解 `(a,b,c),(a,c,d)` 与 `appendQuad` 的**对角线一致**。
`cube.mtl`：`newmtl checker` + **`Kd 1 1 1`**（纯白；`1.0 → 1.0` 精确，见 M5.1 Step 3）+ `map_Kd ../tga/checker.tga`。
`assets/obj/README.md` 里记这个文件是**手写**的，以及"v=0 在图片下边缘"这一句 —— 它是锚 1 有牙齿的前提。

- [ ] **Step 2: 锚 1 的资产版（失败测试）**

```cpp
SR_TEST(anchor1_cube_obj_parses_to_the_same_triangles_as_makeCube)
// 左右各 24 个三角形。规范化比较：每个三角形取 3 个 (pos,normal,uv) 角，按角序的**3 种旋转**中
// 字典序最小者作代表（吸收起点选择），全部排序后逐元素比较 ⇒ **绕向必须一致**（不许镜像）
// 先断言 positions.size() == 24（== 面角数 ⇒ 这个文件没发生跨面共享，立方体的固有形状，别误读成"去重没生效"）
```
失败信息要打印**第一个不一致的三角形下标与三对值** —— 这个测试同时是 cube.obj 的**编写工具**，作者靠它把文件调对。

- [ ] **Step 3: 跑测试确认失败**（第一次跑必然是红的，除非一次写对了 OBJ —— 正常，按报告改文件，**不要改解析器**）

- [ ] **Step 4: `renderObjGoldenScene`**

三行：`target.depth.clear(); goldenBackground(target.color);` → `vu = {identity, goldenView(), goldenProjection(kGoldenWidth,kGoldenHeight), identity}` → `drawModel(BackendA, m, target, vu, baseFu{white, ClampEdge...}, stats)` + `drawText(..., "GOLDEN M5 320X180", ...)`。
**采样状态必须用提出来的 `goldenSampler()`**，不许自己写第二份（收紧 5）。

- [ ] **Step 5: 锚 3（零容差交叉比对，无新参考图）**

```cpp
SR_TEST(anchor3_obj_golden_is_the_m4_golden_except_the_hud_strip)
#ifndef _MSC_VER → 打印 [skip] 并 return（C1′：golden 只在 MSVC 判定）
渲染 renderObjGoldenScene(loadModel(SR_SOURCE_DIR "/assets/obj/cube.obj")) 到 Canvas(320,180)
读 assets/golden/m4_textured_cube.bmp（用 tests/golden/bmp_read.h 的 readBmp24）
逐像素**精确相等**（这条不是数值回归，是**约定一致性**检查 ⇒ 容差是 0，与 kTolerancePerChannel=1 无关）
**跳过矩形 x ∈ [0,110) 且 y ∈ [0,12)**：HUD 从 (3,3) 起，17 字符 × kGlyphAdvancePx(6) = 102 宽、kGlyphHeightPx(8) 高
差异 > 0 时打印前 5 个坐标 + 两个像素值 + 差异总数，并提示"先看锚 1、锚 2 是否同时红 —— 只有这里红而它们绿，
说明差异不在翻转上"
```
**这条必须做一次破坏性验证**（M4 的先例）：临时把 `cube.mtl` 的 `Kd` 改成 `0.5 0.5 0.5` ⇒ 本条红；
改回 ⇒ 绿。若 `Kd` 改了它还不红，说明材质 tint 根本没接进 OBJ 路径（比 golden 不对更值得知道的事）。

- [ ] **Step 6: 生成 `--golden-write-obj` 的输出给人看 + 写进 notes 的图**

```bash
./build/Debug/soft-raster.exe --obj assets/obj/cube.obj --golden-write-obj docs/notes/img/m5-obj-cube.png  # BMP 就写 .bmp，按需转 png 给人看
```

- [ ] **Step 7: 变异验证 —— 三条锚必须各自独立红**

这是 M5 最重要的一次验证，逐条做并在账本里记录"哪条红了几处"：
① 删掉 `parseObj` 的 `1.0f - v` ⇒ **锚 1 红 + 锚 3 红**（锚 2 绿）；
② 把 `parseTga` 的 origin 处理改成"永远不翻" ⇒ **锚 2 红**（锚 1 绿），锚 3 **红**（因为只翻了一次）；
③ ①+② **同时做** ⇒ 双翻转抵消：锚 3 **可能绿**，而**锚 1 与锚 2 必须都红** —— 这一步是整份设计存在的理由，
   账本里必须留下"抵消发生时只有 1、2 抓得住"的实测证据。

- [ ] **Step 8: 全绿（含两份老 golden）+ 提交** `test(M5.7): 三处方向约定各有独立的锚 —— 双翻转抵消实测只有锚 1/2 抓得住`

---

## Task M5.8: 收尾 —— notes、README、三侧全量、spec 脚注、账本

**Files:**
- Create: `docs/notes/M5-OBJ导入与三次翻转.md`、`docs/notes/img/m5-*.png`
- Modify: `README.md`（M5 勾选、按键表加 `F`/拖放、`--obj`/`--emit-checker-tga`/`--golden-write-obj` 的示例、里程碑表）
- Modify: `docs/superpowers/specs/2026-11-21-soft-raster-design.md`（§7.5 两处加脚注指向 M5 设计的 D15 / D14 续）
- Modify: 设计文档 §15 的"8 个任务" → 9
- 账本：`.superpowers/sdd/2026-11-23-soft-raster-m5-obj/progress.md`（**不入库**，但必须写完）

- [ ] **Step 1: 三侧全量验证**

```bash
cmake --build build --config Debug 2>&1 | grep -E "warning|error"      # 必须为空
ctest --test-dir build -C Debug --timeout 60
cmake --build build --config Release --clean-first 2>&1 | grep -E "warning|error"   # 纪律③：--clean-first
ctest --test-dir build -C Release --timeout 60
cmake --build build-gcc 2>&1 | grep -E "warning|error"; ctest --test-dir build-gcc --timeout 60
./build/Debug/soft-raster.exe --golden-write-textured build/e4.bmp && md5sum build/e4.bmp assets/golden/m4_textured_cube.bmp
# M3 那张只由 ctest 的 ±1 门禁判，不做 md5（Ruling 50）
grep -rn "stb_image" src/          # 只命中 image_stb.*（C2）
grep -rn "windows.h" src/render src/math src/scene src/loader   # 只应命中 image_stb 的 _wfopen 分支之外**无**（C4）
```

- [ ] **Step 2: notes 的骨架（写"为什么"，不写"做了什么"）**

必须有的五节：① 这一章的主题是**抵消**（三处方向约定、两处都是翻行、同时错则画面正确），
带 §1 那张表和 Step 7 的**实测**证据（哪个变异红在哪条锚上）；
② 夹具对称性这一族的第四次出现（引 M4 notes §1 那张表，把"翻两次"补成第 6 行）；
③ `vt` 与 `map_Kd` 选项与 winding 这三件事"格式的差异在格式的门口处理掉"的具体含义；
④ 分层为什么只有一种解（C2 + tests 链接 + 装配需要 stb 的三方交集），以及 `bmp_writer.cpp` 证明"目录 ≠ target"；
⑤ **两条对 spec §7.5 的偏离**（D15 fit 相机、D14 续资产清单），各自的原句、为什么不照做、替代守卫是什么。
deferred 那四条里关掉三条、留下一条（alpha 混合，理由）。

- [ ] **Step 3: README**：M5 行打勾 + 图 + 新按键/新 flag；`R`（重载模型）那行"在 M5 落地"的旧注释改掉。

- [ ] **Step 4: 上游 spec 加脚注**（不做这一步，下一个读 spec 的人会以为 M5 没照 §7.5 做，而不是照 D15 改了它）：
在 §7.5 的 fit 那句与资产清单那句各加一行 `> 偏离见 2026-11-22-soft-raster-m5-obj-design.md D15 / D14 续（M5 实施时定）`。

- [ ] **Step 4.5: 向产品方提一件本里程碑发现的既有事实（不自行处理）**
`assets/golden/m3_cube.bmp` 与今天生成的输出差 2 个字节（±1），因为参考图停在 `3f8cc2a` 而 `src/` 之后又变了 11 个提交。
它在 ±1 门禁下是绿的，**不是缺陷**；但它意味着"M3 的 md5 可复现"这句话从某个提交起就不再成立，
而 anyone 拿它当门禁就会误判自己的重构。两个选项交给产品方：重新生成（纪律 #4 要求先肉眼验收）
或在 `assets/golden/README.md` 里写明"M3 只做 ±1 判定，md5 不是它的门禁"。**不要自己重生成。**

- [ ] **Step 5: 账本写完**：Rulings 43 起，把本计划"六处收紧"、每个任务的变异验证结果、
以及 deferred 的处置写进去。**不删工作区**（`.superpowers/` 不在 git 里，删 = 销毁记录）。

- [ ] **Step 6: 终审前的自查**：`git diff main --stat` 逐文件过一遍，找残留的调试打印、
被注释掉的测试、`#if 0`、以及 `sr_tmp_bmp/` 这类临时输出有没有误入库。

- [ ] **Step 7: 提交 + 走 `superpowers:requesting-code-review` → 合并**
按 M4 的节奏：先过一轮 fresh-context 整体评审（覆盖 `main..HEAD` 全部 M5 提交），
Critical/Important 修完再合并；push 由产品方定（GitHub push 在本机常被重置，失败了重推即可）。
