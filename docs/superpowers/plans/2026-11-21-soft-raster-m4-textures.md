# soft-raster M4 纹理与采样 —— 实施计划

**Spec:** `docs/superpowers/specs/2026-11-21-soft-raster-design.md` §7.4（权威）
**上位计划:** `2026-11-21-soft-raster-m0-m3.md` 的「M4–M10 概要」按约定在此展开。
**已完成的 M4.1:** mip 链生成 + mip 层级选择，见 ledger 的《M4.1 完成》与 Ruling 23–26。

**Goal:** 到 M4 结束：立方体贴一张程序生成的 checker，旋转时贴图不扭曲；`1` 键关掉 mipmap 能看到闪烁、打开就不闪（spec §8 M4 行的可见成果）。

**Global Constraints:** 沿用上位计划那一节（C++20 / 不联网 / C4 core 无 Win32 / C5 热路径不分配不派发 / C1′ 容差断言 / 中文注释 / 每任务一提交）。只补 M4 特有的一条：
`third_party/stb_image.h` 是**唯一**第三方文件，只被 `src/loader/image_stb.*` 包含，`soft_raster_core` 不得感知它（C2）。

## 接口裁决（展开时必须先定的两件事）

**Ruling A — `FilterMode`/`WrapMode` 放 `FragmentUniforms`，不放 `PipelineState`。**
理由：spec §7.1 把「纹理」列在 Uniforms 里；`pipeline.h:36` 的注释本来就写着 M4 往
`FragmentUniforms` 加 texture 指针；而 `PipelineState` 的其余字段（cull / depthTest / target）全是
**固定功能阶段**读的，采样是 FS 的事，混进去会让光栅阶段依赖采样概念。
代价：若 M8 的 Backend B 想按 filter 特化代码路径，得把这三个字段再搬一次——但 FS 是逐像素函数指针，
后端本来不关心它内部读什么，所以这个代价大概率不会兑现。

**Ruling B — `FragmentInput::uvDerivX/uvDerivY` 改名成 `gradU/gradV`。**
现状：`render_backend.cpp:80` 填的是 `{dudx, dudy}` 与 `{dvdx, dvdy}`，也就是**每个标量场自己的梯度**
（∇u 与 ∇v），而字段名读起来像「沿 X 的导数 / 沿 Y 的导数」。`mipLevelFromDerivatives` 的参数顺序是
`(dU/dx, dV/dx, dU/dy, dV/dy)` —— 两者相差一次**转置**，填错的后果不是崩溃也不是黑屏，
只是「mip 选错、画面糊或者闪」，是最难归因的一类 bug。所以：改名 + 提供一个只收 `Vec2 gradU, Vec2 gradV`
的入口，让调用方没有机会按位置传错四个 float。

---

### Task 4.2: `Texture::fromImage` + wrap + `sampleNearest`

**Files:** Create `src/render/sampler.h`, `src/render/sampler.cpp`；Modify `src/render/texture.h/.cpp`
**Test:** `tests/unit/test_sampler.cpp`

**Interfaces:**
- Produces:
  ```cpp
  namespace sr {
  enum class WrapMode : uint8_t { Repeat, ClampEdge };

  // 把一张图变成可采样的纹理（生成整条 mip 链）。
  Texture makeTexture(ImageRGBA8 base, bool colorDataAsSrgb = true);

  // 纹素的浮点值。rgb 在 colorData 为真时**已解码到线性空间**，a 永远是 value/255。
  struct TexelF { float r{}, g{}, b{}, a{}; };

  // UV→纹素索引的唯一真相来源，nearest 与 bilinear 共用，避免两套 off-by-half。
  // u=0 落在第 0 个纹素的**中心**还是**左边缘**？本实现：floored 索引 = floor(u * W)，
  // 即 u=0 是第 0 个纹素的左边缘、u=0.5/W 才是它的中心（Direct3D/OpenGL 的 uv 地址约定一致）。
  int wrapCoord(int i, int count, WrapMode wrap);

  TexelF sampleNearest(const Texture&, WrapMode wrapU, WrapMode wrapV, float u, float v);
  }
  ```
- `Texture` 补一个 `bool srgb = true;`（对应 GPU 的 `SRGB8_ALPHA8` vs `RGBA8`：checker/照片是颜色数据要解码，
  uv 调试图是**数据**，解码会把 0.5 变成 0.73，那种图必须能声明成线性）。

**Steps:**
- [x] **Step 1: 写失败测试** — 断言：
  - `sampleNearest` 在 `u=0.5/W, v=0.5/H`（第 0 个纹素中心）取回的正是那个纹素；在 `u=(W-0.5)/W` 取回最后一个。
  - **off-by-half 的判别**：`u = 0.5/W - epsilon` 与 `u = 0.5/W + epsilon` 必须落在**相邻两个不同**纹素上（证明分界线在纹素边界而不是纹素中心）。
  - `Repeat`：`u = 1.25` 与 `u = 0.25` 取回同一个纹素；`u = -0.25` 与 `u = 0.75` 同一个（**负数取模**是 C++ `%` 的坑，必须单独 pin）。
  - `ClampEdge`：`u = -1.0` → 第 0 个，`u = 2.0` → 最后一个；且不越界读内存。
  - 非方形纹理（8x2）：u 用宽、v 用高，两者不串（把 `x` 与 `y` 写成同一个尺寸是个真会犯的错）。
  - sRGB 解码：一张存着 `0x80` 的颜色纹理，`sampleNearest().r ≈ srgb8ToLinear(0x80)`（≈0.2158）而**不是** 0x80/255（0.5019）；`srgb=false` 的纹理则必须是 0.5019。alpha 两种情况都等于 `a/255`。
  - `makeTexture` 之后 `levels` 与 `generateMipChain` 一致（链尾 1x1，见 Ruling 24）。
- [x] **Step 2: 跑，确认失败**（编译失败 = 红，因为新 API 不存在）
- [x] **Step 3: 实现**（`wrapCoord` 用 `i - floor2(i/count)*count` 风格而不是 `%`，负数安全；`sampleNearest` 走 `Texture::texel`，越界返回 nullptr 时**报告而不是静默取 0**——那意味着 wrap 算错了）
- [x] **Step 4: 跑，全绿** + MSVC/g++ 两侧零警告 + C4 grep CLEAN
- [x] **Step 5: 区分力验证** — 把 `floor(u*W)` 改成 `int(u*W)`（对负 UV 行为不同）与把 `%` 当成 wrap 用（负数）两种变异，各自必须让某条断言变红
- [x] **Step 6: Commit** `feat(render): texture wrap + nearest sampling with the texel-center convention pinned`

### Task 4.3: `sampleBilinear`（跨边界的一致性）

- [x] 测试：四个纹素分别是 0/64/128/192 时，纹素中心取回原值，纹素边界取回两两平均；
  **跨 Repeat 边界**（u 接近 1.0 时第二个 taps 回到索引 0）与 `ClampEdge`（同一位置钳在最后一个）必须分别断言——
  这两个在边界处的行为差异是 bilinear 最容易"看起来对"的地方。
- [x] 测试：线性插值发生在**线性空间**（一张左黑右白的 sRGB 图，中点值应接近 `srgb8ToLinear` 后的平均，而不是 0.5）
- [x] 实现 → 全绿 → 变异验证（先把权重 `1-f` 与 `f` 写反，必须有断言红）→ Commit

### Task 4.4: `mipLevelFromGradients` + `sampleTrilinear`

- [x] Ruling B 落地：`FragmentInput` 改名 `gradU/gradV`；新增
  `int mipLevelFromGradients(float texelsU, float texelsV, int levelCount, Vec2 gradU, Vec2 gradV)`
- [x] 测试：gradU/gradV 传反（转置）必须让某条断言变红 —— 构造一个 u 沿 x 变、v 沿 y 变的非对称用例
- [x] 测试：trilinear 在 `rho` 恰好跨级时 = 相邻两级的线性混合（取 1.5 级处 = 两级各半）
- [x] 测试：`useMip=false` 时永远走 level 0（`1` 键的语义）
- [x] 实现 → 全绿 → Commit

### Task 4.5: 贴图进管线（端到端）

- [x] `FragmentUniforms` 加 `const Texture* texture; WrapMode; FilterMode; bool useMip;`；`builtin_shaders` 加 `texturedFS`（读 `in.v.uv` → 采样 → 乘 `baseColor`）
- [x] 测试：`tests/unit/test_texture_pipeline.cpp` —— 离屏画一个 uv 已知的三角形/四边形，断言中心像素 == 期望纹素、四个角分别落在四个角的纹素上；**旋转 45° 后贴图跟着转**（断言某条对角线上的采样顺序变了，而不是只断言"没崩"）
- [x] golden：`assets/golden/` 新增 M4 参考图（320x180，同 M3 约定），并肉眼看过一次
- [x] Commit

### Task 4.6: stb_image 接入 + 手写 TGA

- [x] `third_party/stb_image.h`（版本与许可证写在文件头注释，C2）+ `src/loader/image_stb.{h,cpp}`（core 之外）
- [x] 测试 TGA（纯函数，可单测：24bpp/32bpp、自底向上、非法输入返回 false）；stb 侧只测"给了路径不存在时返回 false 且 err 非空"
- [x] `grep -rn "#include <windows" src/ | grep -v '^src/app'` 仍 CLEAN；`grep -rn "stb_image" src/` 只命中 `src/loader/image_stb.*`（C2 的"唯一一处"要能被机器检查）
- [x] Commit

### Task 4.7: M4 可见成果 + 收尾

- [x] 立方体贴程序生成的 checker（uv 覆盖 [0,1]，每面同一张）；`1` 键切 mip、`2` 键切 nearest/bilinear/trilinear
      **计划缺陷（已修）**：`makeCube` 的 uv 从 M2 起就是错的 —— 按下标位模式赋值给出一个自交的
      蝴蝶结参数化，而且同一张表无法表达"v=0 在顶边"（±X 面的角序起点不同 ⇒ 那两面转了 90°）。
      另外"纯棋盘"作为夹具在转置下恒等，测不出 uv 接反，所以贴图带三个角标记。见 notes §1 与 §7。
- [x] 手动验证（这是 spec §8 的验收，必须亲眼看过）：旋转时贴图不扭曲；关掉 mip 在斜面/远处闪烁，打开不闪
      —— 产品方 2026-11-21 在窗口里跑过并确认。
- [x] `docs/notes/M4-纹理与采样.md`（仓库的命名约定是 `Mn-中文标题.md`，不是计划写的 `M4.md`）：
      texel 中心约定与 off-by-half；sRGB 解码为什么在采样时做而不是在加载时做；
      为什么各向异性取 max 而不是平均；mip 链在 2x1 处的那一级不是"真实平均"而是边缘复制；
      外加本里程碑真正的产出——"夹具在某个对称操作下不变，那个操作就测不出来"这一族（踩了五次）。
- [x] README M4 打勾 + 图；全套验证（Debug/Release/g++/C4/C2）；Commit + push
