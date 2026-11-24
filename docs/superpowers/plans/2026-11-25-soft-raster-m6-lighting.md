# M6 光照 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给管线接上真实光照（一盏跟随相机的点光 + 一盏方向光），三种着色模式可按键切换，全部运算在线性空间；同时把 M3 留下、M6 之前没人读的三个接口叫醒并证明它们醒着。

**Architecture:** 光照本体是 `src/render/shading.h` 里两个**自由纯函数**（可解析单测），FS/VS 只负责取插值结果并调用它们；`LightRig` 与 `MaterialShade` 是每帧一份的普通结构体，由 `VertexUniforms`/`FragmentUniforms` 各持一个指针指向同一份（单一真源）；`makeNormalMatrix` 复用早已存在但从未被生产代码调用的 `tryInvert/transposed/upper3x3AsMat4`。抽象止步于阶段边界（反目标 A1）：shader 里看得见点积、半向量和 `pow`。

**Tech Stack:** C++20；CMake + MSVC（主路径）/ MSYS2 g++（次路径）；自建 harness（`SR_TEST`/`SR_ASSERT`/`SR_ASSERT_NEAR`/`SR_ASSERT_EQ`）；CTest；零第三方渲染依赖。

**Spec:** [`docs/superpowers/specs/2026-11-24-soft-raster-m6-lighting-design.md`](../specs/2026-11-24-soft-raster-m6-lighting-design.md) —— 裁决 **D3–D23** 全部权威（第二版已吸收 builder 评审的 11 处纠正，§14 是那份清单）。上游是 `2026-11-21-soft-raster-design.md`；前章是 `2026-11-22-soft-raster-m5-obj-design.md`（其 D5 要求 M6 做一次审视，落在本计划 Task M6.1 与 M6.9）。

## Global Constraints

逐字来自设计文档与上游 spec，每个任务的隐含要求都包含这一节：

- **C5**：逐像素不分配、不虚派发。`LightRig`/`MaterialShade` 以**指针**进入 uniforms；`shading.h` 的函数按 `const&` 收参；分阶段计时**每三角形至多几次读时钟，绝不逐像素**。
- **C4**：`soft_raster_core` 内不得出现 `<windows.h>` 或 Win32 符号。本里程碑全部新代码在 core（`render/`、`math/`、`scene/`）或 app（`src/app/`）。
- **C2**：`stb_image` 只许出现在 `src/loader/image_stb.*`。机器检查 `grep -rn "stb_image" src/`；**注释里出现该字面量也算**（M5.1 的实际教训）。
- **C1′**：数值断言一律容差（默认 `1e-5` 量级）；**golden 与一切含 `pow` 的比对只在 MSVC 判定**，g++ 侧打印可见 `[skip]`（`tests/golden/golden.cpp:90-92` 的先例）。
- **`Varyings` 仍是 12 个 float**，`kVaryingFloatCount == 12` 与 `vertex.h:23` 的 `static_assert` 不许动（D6）。
- **spec §7.3 的命名照抄**：`unlit` / `lambert_pervertex` / `blinn_phong_perpixel` / `debug_*`。
- **两份既有 golden 必须逐字节不变**（M3 与 M4）：M4 用 md5 `1f767a114a49d38c454448eec4465d51` 判；**M3 不做 md5 门禁**（参考图停在 `3f8cc2a`，`src/` 之后变了 12 个提交，今天生成会差 2 字节 ±1，属既有事实，见设计 Ruling 50 与 `assets/golden/README.md`）。
- **绝不重新生成任何参考图**（纪律 #4，需产品方授权）。
- **零警告地板**：MSVC `/W4 /utf-8`、GNU `-Wall -Wextra -ffp-contract=off`。新增 .cpp 必须手工列进 `CMakeLists.txt` 对应 target；**测试文件尤其**——harness 靠静态初始化自注册，链接器看不见的 TU 会**静默贡献 0 条用例**。
- **验证纪律四条**：① 每个新测试做变异验证，且**一次 abort 的变异运行不算证据**（错代码必须给断言级红）；② 构建失败时绝不读测试结果，用 `out=$(cmake --build ... 2>&1 | grep -E "warning|error"); if [ -n "$out" ]; then 结论作废; else 跑测试; fi`；③ Release 一律 `--clean-first` 且逐条读告警；④ **生成 golden 前必须产品方亲眼验收**。
- **Ruling 67 的 M6 形态**：一个 shader 读 K 个字段（`N`/`L`/`V`/`H`/`Ns`/`Ks`/`Ka`/光强/albedo），**每一个都要有它自己能红的断言，用两个不同取值互换**，置空/置零只证明"读了"，不证明"读对了哪一个"。
- **`SR_ASSERT` 记录后继续**（不中止）：断言之后的解引用必须先判空；反过来，判空**不许**把断言一起吞掉（M5.2/M5.3 各中过一次）。

## Review Focus

设计文档隐含、但没有任何一条快乐路径测试会抓到的五类输入。每行在本计划里有归属任务与具名测试：

1. **饱和**：光强或材质系数一旦让线性值超过 1.0，`encodeSRGB` 会 clamp ⇒ **Gouraud 与逐像素两张图在峰值处都变成 255**，本章核心对比在像素层面消失，而人眼只会说"太亮了"。合理期待：默认参数下高光**不饱和且看得见**。⇒ **M6.4 Step 6（`lit_golden_has_a_highlight_that_does_not_saturate`）+ M6.8**。
2. **退化法线 / 零面积面**（真实 OBJ 里必然出现：重合顶点、只导出顶点的文件）：`normalize` 对零向量返回零向量（`vec.h:67-72`），于是 `N·L = 0` ⇒ 该像素**只剩 ambient**。合理期待：不崩、不 NaN、看起来是暗面而不是黑洞，且**有可见的 ambient 项**。⇒ **M6.2 Step 7（`degenerate_normal_falls_back_to_ambient_not_black_or_nan`）**。
3. **`Ns` 的两个极端与 `Ks` 全黑**：`powf(x, 0) == 1` 会给整个表面镀一层均匀白；`Ns` 极大（MTL 允许 1000+）会让高光缩到不足一个像素。合理期待：`Ns=0` 等价于"无高光"，`Ns` 超范围被夹住而不是变黑或变白饼。⇒ **M6.2 Step 5/8（`Ns_zero_disables_specular_and_is_equivalent_to_Ks_zero`、`shininess_is_clamped_before_the_pow`）**。
4. **无材质几何与有材质几何同屏**：`Material` 的默认是 `Ks=0/Ka=0/Ns=0`（`mtl.h:24-27`），对"文件没给材质"是对的，对"我要演示高光"的程序球是错的。合理期待：程序几何看得见高光，而 OBJ 的材质**不被默认值覆盖**。⇒ **M6.1 Step 6（`procedural_geometry_gets_a_lit_default_and_obj_keeps_its_own_material`）**。
5. **切到 Gouraud 时贴图消失**（VS 不采样，D17）：用户会以为"贴图坏了"。合理期待：HUD 上说清当前着色模式，且这个差别是**被断言的**而不是被隐藏的。⇒ **M6.3 Step 8（`gouraud_ignores_the_texture_and_says_so_in_the_shader_choice`）+ M6.6 的 HUD 行**。

---

## 文件结构

**新建**

| 文件 | target | 责任 |
|---|---|---|
| `src/render/lights.h` | core | `LightRig`、`MaterialShade`、`litDefaultMaterialShade()`（纯数据，无行为） |
| `src/render/shading.h` | core | **光照本体**：`shadeDiffuseOnly` / `shadeBlinnPhong`，自由函数，可解析单测（D20） |
| `tests/unit/test_shading.cpp` | tests | 纯函数的解析断言（衰减、`Ns`、两灯叠加、退化法线） |
| `tests/unit/test_lit_shaders.cpp` | tests | `lambertVS`/两个 FS/`debugNormalFS` 的行为断言 |
| `tests/unit/test_view_mode.cpp` | tests | `planView` 的映射（FS vs sink） |
| `src/app/view_mode.h` / `.cpp` | app（+ 手工列进 tests，`cmdline.cpp` 先例） | `ViewMode` → `ViewPlan{fragmentShader, sinkView, name}`，**不含 Win32** |
| `docs/notes/M6-光照与休眠接口.md` | 文档 | 学习笔记（G8，验收条件） |

**修改**：`src/math/transform.h`（`makeNormalMatrix`）、`src/render/pipeline.h`（两组 uniforms 各加两个指针）、`src/render/builtin_shaders.h/.cpp`（新增 VS/FS，`transformVS` 只改 `worldPos` 那一行）、`src/render/render_backend.cpp`（分阶段计时）、`src/render/stats.h`（注释：字段现在真的被写了）、`src/render/sink.h/.cpp`（视图枚举与分界说明）、`src/scene/golden_scene.h/.cpp`（`renderGoldenSceneLit` + 长标签条）、`src/loader/model.cpp`（逐组填 `MaterialShade`）、`src/app/cmdline.h/.cpp`（11 个 flag + `--bench` 修）、`src/app/main.cpp`（着色模式、光参数、HUD 第三行、`makeNormalMatrix` 取代 identity）、`src/app/scene.h/.cpp`（`buildLightRig` + last-good 法线矩阵）、`src/app/input.h`、`src/app/win32_window.cpp`（`3`/`[`/`]`/`-`/`=`/`A`/`0`）、`CMakeLists.txt`、`README.md`、`docs/superpowers/specs/2026-11-22-soft-raster-m5-obj-design.md`（回写 D5）、`docs/superpowers/specs/2026-11-21-soft-raster-design.md`（§7.6 按键表加新键）。

**新建测试文件**：`test_shading.cpp`、`test_lit_shaders.cpp`、`test_view_mode.cpp`（都要列进 `CMakeLists.txt` 的 tests 源列表）。

---

## Task M6.0: 叫醒休眠接口 —— `makeNormalMatrix` + `worldPos` 的真世界空间

本章的地板。**这个任务的验收标准是"两份既有 golden 逐字节不变"**：字段改了语义，但没有任何 shader 读它们。

**Files:**
- Modify: `src/math/transform.h`（追加 `makeNormalMatrix`；`mat.h:26-31` 的 `tryInvert/transposed/upper3x3AsMat4` 已在）
- Modify: `src/render/builtin_shaders.cpp:13`（`worldPos` 那一行）与 `:11`（变换后归一化）
- Modify: `src/app/main.cpp:150`、`src/scene/golden_scene.cpp:83` 与 `:111`、`src/loader/obj_scene.cpp:24`（生产侧 identity 全部换成 `makeNormalMatrix` 调用）
- Test: `tests/unit/test_transform.cpp`（追加）、`tests/unit/test_mesh.cpp` 不动

**Interfaces:**
- Consumes: `Mat4::tryInvert(const Mat4&, Mat4&) -> bool`（奇异 ⇒ false 且**不写 out**）、`Mat4::transposed()`、`Mat4::upper3x3AsMat4()`、`normalize(const Vec3&)`（零向量 ⇒ 零向量，`vec.h:67`）。
- Produces:
  ```cpp
  // src/math/transform.h
  // 法线矩阵 = transpose(inverse(model 的 3x3))。奇异 ⇒ false 且不写 out（与 tryInvert 同约定）。
  bool makeNormalMatrix(const Mat4& model, Mat4& out);
  ```

- [ ] **Step 1: 写失败测试**（`tests/unit/test_transform.cpp` 追加；命名照设计 §8.1）

```cpp
SR_TEST(worldPos_is_world_space_not_object_space)      // 今天必须红：它就是"没人读"的证据
SR_TEST(normal_matrix_undoes_non_uniform_scale_for_normals)
SR_TEST(normal_is_normalized_after_the_transform_not_before)
SR_TEST(makeNormalMatrix_rejects_a_singular_model_and_does_not_write_out)
SR_TEST(makeNormalMatrix_of_a_pure_rotation_equals_that_rotation)   // transpose(inverse(R)) = R
```

`worldPos` 那条：`model = Mat4::translation({5,0,0})`，`VSIn.position = {1,2,3}`，
断言 `v.worldPos.x ≈ 6.0f`、`.y ≈ 2.0f`、`.z ≈ 3.0f`（容差 `1e-5f`）。

非均匀缩放那条（本章最值的一条推导）：`model = Mat4::scale({2,1,1})`，
面由 `p0=(0,0,0)`、`p1=(1,0,0)`、`p2=(0,1,0)` 张成 ⇒ 变换后表面切向 `t1=(2,0,0)`、`t2=(0,1,0)`；
取物体空间法线 `n = (1,1,0)/√2`（它垂直于原表面）：
- **朴素变换** `xyz(model * vec4(n,0)) = (2,1,0)/√2` ⇒ 与 `t1` 的点积 `≠ 0`（断言 `> 0.1`）；
- `makeNormalMatrix` 变换后 `normalize(m * n)` 与 `t1`、`t2` 的点积都 `≈ 0`（容差 `1e-5f`）。

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build --config Debug && ./build/Debug/soft_raster_tests.exe --only normal_ ; ./build/Debug/soft_raster_tests.exe --only worldPos_`
Expected: `worldPos_is_world_space_not_object_space` 与 `normal_matrix_undoes_non_uniform_scale_for_normals` 红（前者是 `makeNormalMatrix` 未定义 ⇒ 编译失败，按纪律 #2 先修到"链接得过、断言红"：先只加声明 + 一个 `return false` 的桩）。

- [ ] **Step 3: 实现 `makeNormalMatrix`**

三行组合：`Mat4 u3 = model.upper3x3AsMat4(); Mat4 inv; if (!Mat4::tryInvert(u3, inv)) return false; out = inv.transposed(); return true;`
注释里写明**为什么是 inverse-transpose 而不是"惯例"**：非均匀缩放会同时改变表面切向与朴素变换后的法线，二者不再垂直（Step 1 的那条断言就是这个几何事实本身）。

- [ ] **Step 4: 改 `transformVS` 的两行**

`v.worldPos = xyz(u.model * makeVec4(in.position, 1.0f));`
`v.worldNormal = normalize(xyz(u.normalMatrix * makeVec4(in.normal, 0.0f)));`
**归一化在乘法之后**（D4）：`scale(2,1,1)` 下变换前归一化等于没归一化。
注释里注明"`transformVS` 不加光照（范围），但这两行必须改（D4）"——设计 §7 与 D4 的措辞冲突已修正，别退回旧的读法。

- [ ] **Step 5: 生产侧的 identity 全部换成真调用**

四处：`main.cpp:150`、`golden_scene.cpp:83`、`golden_scene.cpp:111`、`obj_scene.cpp:24`。
写法统一为：
```cpp
Mat4 nm;
if (!makeNormalMatrix(<model 表达式>, nm)) { nm = <last-good 或 identity 变量>; /* 记一条诊断 */ }
st.vu.normalMatrix = nm;
```
golden 两处传入 `goldenModel()`，因此 `normalMatrix` 从此**等于那个旋转**而不是 identity（§8.1 #4）。
`main.cpp` 那处的 model 真是 identity，也要走 `makeNormalMatrix`（守卫故意要的代价）。
**不许出现字面量赋值 `normalMatrix = Mat4::identity()`**（M6.9 的 grep 守卫会查）。

- [ ] **Step 6: 全绿 + "改了字段没改画面"的证明**

```bash
out=$(cmake --build build --config Debug 2>&1 | grep -E "warning|error"); [ -z "$out" ] || echo 作废
ctest --test-dir build -C Debug --timeout 60
# 两个 golden 入口是互斥的（Ruling 38 的计数守卫），所以必须分开跑，不能一行给两个 flag
./build/Debug/soft-raster.exe --golden-write-textured build/m4.bmp
md5sum build/m4.bmp assets/golden/m4_textured_cube.bmp     # 必须相同
./build/Debug/soft-raster.exe --golden-write build/m3.bmp
cmp -l build/m3.bmp assets/golden/m3_cube.bmp | head       # 预期：≤ 若干个字节、每个差 ±1（既有事实，Ruling 50）
# M3 的一致性由 ctest 的 ±1 门禁判定，md5 不是它的门禁 —— 别因为这对不上就重生成参考图
cmake --build build-gcc && ctest --test-dir build-gcc --timeout 60
```
两份 golden 不变本身就是证据：字段语义改了，但没有任何 shader 读它们。

- [ ] **Step 7: 变异验证（五个，逐个记录红在哪条断言）**
① `makeNormalMatrix` 直接返回 `model.upper3x3AsMat4()`（不逆不转置）；② 把 `normalize` 移到乘法之前；
③ `worldPos` 改回 `in.position`；④ 奇异检测改成 `det != 0`（固定阈值）；⑤ 返回 `inv` 不转置。
**每个都必须是断言级红**（不是 abort）；若某个变异让进程崩，说明测试自己越界了，先修测试。

- [ ] **Step 8: 提交**

```bash
git add src/math/transform.h src/render/builtin_shaders.cpp src/app/main.cpp src/scene/golden_scene.cpp src/loader/obj_scene.cpp tests/unit/test_transform.cpp
git commit -m "fix(math+render): 叫醒 M3 留下的休眠接口 —— makeNormalMatrix 与真世界空间 worldPos（M6.0）"
```

---

## Task M6.1: `LightRig` + `MaterialShade` 进 uniforms

**Files:**
- Create: `src/render/lights.h`
- Modify: `src/render/pipeline.h:30-46`（两组 uniforms 各加 `const LightRig*` 与 `const MaterialShade*`）
- Modify: `src/loader/model.cpp:229-250`（逐组填材质）
- Test: `tests/unit/test_lit_shaders.cpp`（本任务先建，只放 plumbing 用例）

**Interfaces:**
- Consumes: `LinearColor`（`color.h:11`）、`srgbToLinear(float)`（`color.h:27`）、`Material`（`mtl.h:20-31`）、`BackendA::drawRange`。
- Produces:
  ```cpp
  struct LightRig {
      Vec3 keyDirWorld{}; LinearColor keyColor{}; float keyIntensity = 1.0f;
      Vec3 pointPosWorld{}; LinearColor pointColor{}; float pointIntensity = 1.0f;
      bool attenuate = false; float attenuateRadius = 1.0f;
      LinearColor ambient{}; Vec3 eyeWorld{};
  };
  struct MaterialShade { LinearColor diffuse{1,1,1}, ambient{0,0,0}, specular{0,0,0}; float shininess = 0.0f; };
  const MaterialShade& litDefaultMaterialShade();   // diffuse 1 / specular .35 / shininess 64 / ambient .02
  // 两个 uniforms 都变成： const LightRig* lights = nullptr;  const MaterialShade* mat = nullptr;
  ```

- [ ] **Step 1: 写失败测试**

```cpp
SR_TEST(lights_and_material_reach_both_stages_from_one_source)   // §8.1 #6
SR_TEST(procedural_geometry_gets_a_lit_default_and_obj_keeps_its_own_material)  // Review Focus 4
SR_TEST(null_lights_means_unlit_and_writes_no_nan)               // 早退语义：D15 的 nullptr = unlit
```
第一条的形状：同一个 `LightRig` 实例地址同时挂进 `vu.lights` 与 `fu.lights`，
改 `rig.keyColor.r` 后**分别**跑 `lambertVS` 与（M6.4 之前的）占位 FS……
**注意**：本任务还没有 lit shader，所以这条先只断言"两处指针相等 + `MaterialShade` 被 `drawModel` 逐组填对"，
两阶段同步那半句留给 M6.3/M6.4 补全（在测试注释里写明这条是**分阶段完成**的，避免读者以为已经测全）。

- [ ] **Step 2: 跑测试确认失败**（`lights.h` 不存在 ⇒ 编译失败；先加空声明做到"链接得过、断言红"）

- [ ] **Step 3: 实现 `lights.h`**

`litDefaultMaterialShade()` 用函数内 `static const`（`defaultMaterial()` 的先例，`mtl.cpp:64-68`），
注释写明**为什么程序几何不能沿用 `Material` 的默认**：`Ks=0/Ns=0` 会让 lit golden 烘进一张零高光的球，
G-M6.2 与肉眼门一起静默失败（设计 §14 第 1 条）。

- [ ] **Step 4: uniforms 加指针 + `drawModel` 逐组填**

`model.cpp` 的每组循环里，把 `materials[g.material]` 的 `diffuse/ambient/specular/shininess`
抄进一个**每组一个的 `MaterialShade`**（栈上，循环外复用同一块存储即可，别 push 进容器），
`fu.mat = &thatShade`。
**为什么不是 `const MaterialShade*` 指向 `Material`**：`Material` 还带着 `diffuseMap`/`opacity`/`name`
等 FS 不该读的东西，且 `MaterialShade` 是"着色需要的四个数"这一层抽象（A1：抽象止步于阶段边界）。

- [ ] **Step 5: 光色的 sRGB 解码（Review Focus 5 的一半）**

`buildLightRig`（`src/app/scene.cpp`，本任务先放最小版本）里，
命令行给的 `--key-color 0.5 0.5 0.5` 必须经 `srgbToLinear(float)` ⇒ rig 里 `≈0.2140`。
```cpp
SR_TEST(light_and_material_colors_are_srgb_decoded_once)   // §8.2 #11：不是 0.5，也不是它的平方
```

- [ ] **Step 6: `Ns` 的夹取与 0 语义**

`MaterialShade::shininess` 在**填入时**夹到 `[1, 4096]`，但保留"0 = 无高光"的语义：
用一个 `bool hasSpecular = (raw > 0)` 或 `shininess = 0` 直传 + `shading.h` 里判 0。
**裁定：`shininess` 原样存（含 0），夹取与 0 判断都放在 `shading.h`**（一处规则，两个消费者）。
```cpp
SR_TEST(shininess_is_clamped_before_the_pow)               // Review Focus 3
SR_TEST(Ns_zero_disables_specular_and_is_equivalent_to_Ks_zero)   // §8.2 #10（注意：不是与 keyIntensity=0 等价）
```

- [ ] **Step 7: 全绿 + 变异验证 + 提交**
变异：① `drawModel` 永远用 `materials[0]` 的 `MaterialShade`（Ruling 67：两材质两种 `Ks` 的夹具必须红）；
② 光色不解码直接用；③ `shininess` 夹取下界写成 0（于是 `pow(x,0)==1` 镀白回来）。

---

## Task M6.2: `shading.h` —— 可解析单测的光照本体

设计 D20：**先有可测的数学，再有 shader**。FS/VS 只是薄封装，所以这一步能把所有
"在整帧里找一个满足条件的像素"类不可能夹具（§14 第 7 条）换成解析断言。

**Files:**
- Create: `src/render/shading.h`
- Create: `tests/unit/test_shading.cpp`
- Modify: `CMakeLists.txt`（tests 源列表）

**Interfaces:**
- Consumes: `LightRig`、`MaterialShade`（M6.1）、`Vec3` 自由函数（`dot/cross/normalize/length`）、`srgbToLinear`。
- Produces:
  ```cpp
  // 漫反射 + ambient（Gouraud 用这一支；不含高光）
  LinearColor shadeDiffuseOnly(const Vec3& N, const Vec3& P, const LightRig&, const MaterialShade&,
                               const LinearColor& albedo);
  // 漫反射 + ambient + Blinn-Phong 高光（逐像素用这一支）
  LinearColor shadeBlinnPhong(const Vec3& N, const Vec3& P, const LightRig&, const MaterialShade&,
                              const LinearColor& albedo);
  ```
  `N` 必须是**已归一化**的世界空间法线（函数不替调用方归一化，归一化是 M6.0 的责任 —— 边界写清）。
  两盏灯的贡献相加；`P` 只用于 `L_point = normalize(pointPosWorld - P)` 与衰减距离 `d`。

- [ ] **Step 1: 方向光的漫反射项**

`SR_TEST(directional_lambert_is_albedo_times_NdotL_times_intensity)`：
`N=(0,1,0)`、`L` 取 `(0,1,0)` ⇒ 系数 1；`L=(0,0,1)` ⇒ 系数 0；`L=(0,-1,0)` ⇒ **0 而不是负数**（背光钳到 0）。
断言 `shadeDiffuseOnly` 的每个通道 = `albedo × Kd × intensity × max(0,N·L)`（容差 `1e-6f`，纯乘法可更紧）。

- [ ] **Step 2: ambient 项（D9）**

`SR_TEST(ambient_term_uses_Ka_and_is_the_only_light_on_a_backfacing_surface)`：
`N·L` 全为 0 时输出 `== ambient × Ka`（不是 0）⇒ **Review Focus 2 的"暗面不是黑洞"**。

- [ ] **Step 3: 点光的 `L` 随位置变化**

`SR_TEST(point_light_direction_depends_on_the_surface_position)`：
同一 `N`、两个不同 `P` ⇒ 输出不同；且 `P == pointPosWorld + N*dist` 时 `N·L == 1`。

- [ ] **Step 4: 衰减（§8.2 #9）**

`SR_TEST(attenuation_matches_the_formula_analytically)`：`attenuate=true`、`radius=10`，
`d = 0 / 10 / 30` 三点 ⇒ 系数 `1 / (1+(d/r)²)` = `1 / 0.5 / 0.1`（容差 `1e-5f`）。
`attenuate=false` ⇒ 三点同值。

- [ ] **Step 5: 高光：半向量与 `Ns`**

`SR_TEST(specular_uses_the_half_vector_and_peaks_when_N_equals_H)`：
`L=(0,1,0)`、`V=(0,1,0)` ⇒ `H=(0,1,0)`，`N=(0,1,0)` ⇒ `N·H=1` ⇒ 高光 `= Ks × intensity × 1`；
`N` 转 30° ⇒ 高光按 `cos^Ns` 衰减，断言实测比值（容差按 C1′，`pow` 两侧可能差 `1e-4`，**实测后写死并注释**）。

- [ ] **Step 6: `Ns == 0` 与夹取（Review Focus 3）**

`SR_TEST(Ns_zero_disables_specular_and_is_equivalent_to_Ks_zero)`：
`Ns=0` 与 `Ks=0`（`Ns=64`）输出**逐通道相等**；`Ns=1e6` 被夹到 4096（与 `Ns=4096` 相等）。
**别写成"与 `keyIntensity=0` 相等"** —— 两灯 rig 下那两个差的项不同（§14 第 8 条）。

- [ ] **Step 7: 退化法线（Review Focus 2）**

`SR_TEST(degenerate_normal_falls_back_to_ambient_not_black_or_nan)`：
`N=(0,0,0)`（`normalize` 的零向量约定）⇒ 输出有限、等于 ambient 项、**不含 NaN**。
用 `SR_ASSERT(!(v > 0.0f || v < 0.0f))` 形式的 NaN 检测（`plane_eq.h` 的既有风格）。

- [ ] **Step 8: 实现 `shading.h`**

两个函数；`shadeDiffuseOnly` 是 `shadeBlinnPhong` 去掉高光支的同一套代码 ⇒
**共享一个内部 `struct LightContribution` 或一个 `static` 求和函数**，
不许出现两份逐字重复的 `N·L` 计算（M5 的 `lex.h` 判例：逐字重复的逻辑块必须唯一一份）。
`powf` 只在 `shininess > 0` 时调用（D3）。

- [ ] **Step 9: 全绿 + 变异验证（Ruling 67 的完整一次）**

逐个把 `N`、`L_key`、`L_point`、`V`、`H`、`Ns`、`keyIntensity`、`pointIntensity`、`ambient`、`albedo`
**各改成一个不同的合法错误值**（例如 `V` 用 light 而不是 eye、`H` 忘了归一化、
点光的 `d` 用了 `length(P)` 而不是 `length(P - lightPos)`），记录每个变异红在哪条断言。
**任何字段"改了没人红"就是本任务未完成**（那正是本章的论点）。

- [ ] **Step 10: 提交** `feat(render): 光照本体做成可解析单测的自由函数（M6.2）`

---

## Task M6.3: Gouraud —— `lambertVS` + `lambertPerVertexFS`

**Files:**
- Modify: `src/render/builtin_shaders.h/.cpp`
- Test: `tests/unit/test_lit_shaders.cpp`

**Interfaces:**
- Consumes: `shadeDiffuseOnly`（M6.2）、`Varyings::color` 槽（D6：**不新增槽位**）、`VertexUniforms::lights/mat`。
- Produces:
  ```cpp
  ClipVertex lambertVS(const VSIn&, const VertexUniforms&);
  bool lambertPerVertexFS(const FragmentInput&, const FragmentUniforms&, FragmentOutput&);
  ```

- [ ] **Step 1: 写失败测试**

```cpp
SR_TEST(lambertVS_writes_lit_color_into_the_existing_color_slot)   // 断言 kVaryingFloatCount 仍是 12
SR_TEST(gouraud_ignores_the_texture_and_says_so_in_the_shader_choice)  // Review Focus 5
SR_TEST(gouraud_albedo_is_vertex_color_times_diffuse)              // D17
```
`gouraud_albedo_is_vertex_color_times_diffuse`：顶点色 `(0.5,0.5,0.5)`、`Kd (1,1,1)`、
`N·L = 1`、`intensity = 1`、`ambient 0` ⇒ `v.color.rgb ≈ (0.5,0.5,0.5)`；
再把 `Kd` 改成 `(0.5,0.5,0.5)` ⇒ `(0.25,0.25,0.25)`。**两次都断言**（只改一个系数无法区分"谁被乘了"）。

- [ ] **Step 2: 跑测试确认失败 → Step 3: 实现**

`lambertVS`：MVP 三行与 `transformVS` **共用一个内部函数**（`clipFromPosition` 之类，
放 `builtin_shaders.cpp` 匿名 namespace），不许逐字复制；
然后 `albedo = xyz(in.color) * mat->diffuse`（`LinearColor` 层面），
`v.color = makeVec4(shadeDiffuseOnly(N_world, P_world, *lights, *mat, albedo), 1.0f)`。
`lights == nullptr` ⇒ 退化成 `v.color = in.color`（unlit 语义，别写死白）。
`lambertPerVertexFS`：`out.color = LinearColor{in.v.color.x, .y, .z}`（直读插值结果，**一行**）。

- [ ] **Step 4: 插值正确性**

`SR_TEST(gouraud_interpolates_the_result_not_the_normal)`：
两个顶点法线相差 90° 的三角形，中心像素的亮度**等于两顶点亮度的线性插值**（容差 `1e-5f`），
而不是"插值法线后再算光照"。这条把 Gouraud 的定义钉成代码事实。

- [ ] **Step 5: 全绿 + 变异验证 + 提交**
变异：`lambertPerVertexFS` 改成重算光照（⇒ Step 4 必须红）；albedo 忘乘 `Kd`；`v.color` 写成线性→sRGB 双编码。

---

## Task M6.4: `blinnPhongPerPixelFS`（含贴图 albedo）

**Files:**
- Modify: `src/render/builtin_shaders.h/.cpp`
- Test: `tests/unit/test_lit_shaders.cpp`

**Interfaces:**
- Consumes: `shadeBlinnPhong`（M6.2）、`sample()`（`sampler.h:81`）、`FragmentUniforms::texture/sampler/baseColor`。
- Produces: `bool blinnPhongPerPixelFS(const FragmentInput&, const FragmentUniforms&, FragmentOutput&);`

- [ ] **Step 1: 写失败测试**

```cpp
SR_TEST(textured_model_keeps_the_texture_under_phong)      // §8.2 #12 / D16
SR_TEST(phong_highlight_is_tighter_than_gouraud_on_the_same_rig)  // 为 M6.8 的 #7b 铺路（小夹具，两模式各渲一次比半高宽度）
SR_TEST(phong_reads_the_interpolated_normal_and_position_not_the_vertex_ones) // 逐像素的证据
```
第三条的形状：一个大三角形（覆盖几百像素），三个顶点法线相同但**位置不同**，
点光放在只照亮三角形**一侧**的位置 ⇒ 左半与右半亮度必须不同；
"逐顶点"实现下整片一样亮。（这条就是"逐像素"三个字的断言，而不是注释。）

- [ ] **Step 2: 跑测试确认失败 → Step 3: 实现**

`albedo = (fu.texture ? texel.rgb : white) * mat->diffuse`（线性空间相乘，D16）；
`N = normalize(in.v.worldNormal)`（插值后长度会缩，**必须再归一化** —— 注释写明为什么不能省）；
`out.color = shadeBlinnPhong(N, in.v.worldPos, *fu.lights, *fu.mat, albedo)`。

- [ ] **Step 4: 与 M4 的贴图通路共存**

`SR_TEST(phong_with_null_lights_falls_back_to_texturedFS_output)` ⇒ 无光时不改变既有画面
（保护 M4 golden 的语义边界，也保护 app 的 `unlit` 模式）。

- [ ] **Step 5: 全绿 + 变异验证 + 提交**

---

## Task M6.5: `debugNormalFS` + `V` 循环 4 视图（FS 与 sink 的分界）

**Files:**
- Create: `src/app/view_mode.h` / `view_mode.cpp`（**无 Win32**，列进 tests 源列表）
- Modify: `src/render/builtin_shaders.h/.cpp`（`debugNormalFS`）
- Modify: `src/render/sink.h`（注释：哪些视图能走 FS、哪些必须走 sink，以及为什么）
- Modify: `src/app/main.cpp`（`V` 的处理：`% 3` 改成 4 个视图的循环）
- Create: `tests/unit/test_view_mode.cpp`

**Interfaces:**
- Consumes: `FragmentShaderFn`、`DebugViewSink::View`、`Varyings::worldNormal`。
- Produces:
  ```cpp
  enum class ViewMode : uint8_t { Color, Depth, Gray, Normal };
  struct ViewPlan { bool useFragmentShader; FragmentShaderFn fragmentShader; DebugViewSink::View sink; const char* name; };
  ViewPlan planView(ViewMode mode, FragmentShaderFn defaultFS);   // src/app/view_mode.h
  bool debugNormalFS(const FragmentInput&, const FragmentUniforms&, FragmentOutput&);
  ```

- [ ] **Step 1: 写失败测试**

```cpp
SR_TEST(normal_view_encodes_N_to_half_float_and_is_gamma_encoded_once)
SR_TEST(planView_routes_normal_to_an_FS_and_depth_gray_to_sinks)   // D23 的分界
SR_TEST(planView_falls_back_to_the_default_FS_for_color_view)
```
第一条的**精确期望**（这是本项目的一个已知 wart，必须写明而不是让人猜）：
`debugNormalFS` 输出线性 `(N+1)*0.5`，而颜色目标出口恒定做 `encodeSRGB`
（`render_backend.cpp:102`）。`encodeAnalytic` 用的是 **IEC 分段**（`color.cpp:11-14`，
接缝 0.0031308，幂段 `1.055·v^(1/2.4) − 0.055`），所以线性 `0.5` 编码成 `≈0.7354 → 188`
（**不是 128**，也不是 gamma-2.2 纯幂的 186）。
**先跑一次把实际字节打出来确认是 188，再把它写死**，并在注释里留下推导 ——
本计划第一版这里写的是 132，那是我把解码方向当成了编码方向（自查时抓到）。
注释还要写明：这是**输出侧**恒定做编码的结果，不是 bug；
`Texture::srgb = false` 管的是**输入侧**要不要解码，两件事别混（M4 notes §3 的区分）。

- [ ] **Step 2: 跑测试确认失败 → Step 3: 实现 `debugNormalFS`**

`out.color = LinearColor{(N.x+1)*0.5f, (N.y+1)*0.5f, (N.z+1)*0.5f}`；`lights == nullptr` 也要能工作
（视图不该依赖光）。

- [ ] **Step 4: 实现 `planView`**

`Color` ⇒ `{false, defaultFS, View::Color, "COLOR"}`；`Normal` ⇒ `{true, debugNormalFS, View::Color, "NORMAL"}`；
`Depth/Gray` ⇒ `{false, defaultFS, 对应 sink, "DEPTH"/"GRAY"}`。
`view_mode.h/.cpp` **不含任何 Win32**（`cmdline.cpp` 的先例），并把它列进 `CMakeLists.txt` 的 tests 源列表。

- [ ] **Step 5: 接 `V` 键**

`main.cpp` 的 `% 3` 改成 `% 4` 的 `ViewMode` 循环；`renderFrame` 读 `planView(...)` 决定
"这次提交用哪个 FS + 帧末走哪个 sink"。HUD 第二行显示 `plan.name`。

- [ ] **Step 6: `sink.h` 的分界注释**

在 `DebugViewSink` 上方写死：**能逐像素判定的视图走 FS（法线、以后的 overdraw 计数），
需要整张缓冲的走 sink（深度、灰度）**；并说明为什么 spec §7.3 那句话只适用于前者
（否则会有人花一天去"统一"两套机制 —— D23 的代价）。

- [ ] **Step 7: ⛔ 停下来，请产品方先看 `V` 法线视图**

这是 normalMatrix 修复最便宜的肉眼验证：法线图若与模型朝向不符（比如旋转后仍偏同一侧），
说明 M6.0 的修复没生效或被某个 identity 绕过了。**这一步不需要 golden，也不需要授权生成任何东西。**

- [ ] **Step 8: 全绿 + 变异验证 + 提交**

---

## Task M6.6: app 接线 —— 着色模式、光参数、HUD 第三行

**Files:**
- Modify: `src/app/cmdline.h/.cpp`（11 个 flag + `--shade`）、`src/app/input.h`、`src/app/win32_window.cpp`（`3`/`[`/`]`/`-`/`=`/`A`/`0`）、`src/app/scene.h/.cpp`（`buildLightRig` + last-good 法线矩阵）、`src/app/main.cpp`（rig 构造、模式切换、HUD）
- Test: `tests/unit/test_cmdline.cpp`（追加）、`tests/unit/test_scene.cpp`（追加）

**Interfaces:**
- Consumes: `LightRig`/`MaterialShade`/`litDefaultMaterialShade`、`makeNormalMatrix`、三个 shader、`planView`、`OrbitCamera::eye()`（`camera.h:27`）。
- Produces:
  ```cpp
  // src/app/scene.h
  LightRig buildLightRig(const AppOptions&, const sr::BBox& bounds, const sr::OrbitCamera& cam);
  // 强度步进：加法 ±0.1，夹到 [0,3] ⇒ 0 可达（D24），"只留一盏灯"真能按出来
  void stepIntensity(float& value, float delta);
  ```

- [ ] **Step 1: 命令行测试（先红）**

```cpp
SR_TEST(cmdline_accepts_the_light_flags)              // 11 个 flag 各一条断言，缺值报错带 flag 名
SR_TEST(cmdline_shade_mode_parses_and_rejects_typo)   // unlit|gouraud|phong
SR_TEST(buildLightRig_decodes_colors_and_defaults_radius_to_three_times_the_scene)
SR_TEST(stepIntensity_reaches_zero_by_repeated_presses)   // D24：乘法步进永远到不了 0
```

- [ ] **Step 2: 实现命令行与 `buildLightRig`**

`AppOptions` 存**原始输入值**（sRGB 域），解码只发生在 `buildLightRig` 一处（避免"解两次"）。
`attenuateRadius` 默认 = `bounds.radius() * 3`（bounds 空 ⇒ 1.0，与 Ruling 49 的退化哲学一致）。
`pointPosWorld = cam.eye()`，`eyeWorld = cam.eye()`（D5）；`--point-pos` 覆盖。

- [ ] **Step 3: 按键**

`3` → `cycleShadeMode`；`[`/`]` → 点光 ±0.1；`-`/`=` → 方向光 ±0.1；`A` → 衰减；`0` → 恢复默认。
`input.h` 加对应 bool，`win32_window.cpp` 的 switch 加 case（数字键与字母键同族那条注释已在那里）。

- [ ] **Step 4: HUD 第三行（D10）**

`SHADE <mode>  KEY <r,g,b>×<int>  POINT <…>  Ka <…>  ATT <on|off>`。
**输入值与线性值都显示**（光强被解码过一次，只显示一个会让人算错期望）。
`drawText` 的 y 偏移改成三行；截图与 offscreen 路径同样画（M0 的先例：字体对不对要能靠一张 BMP 判）。

- [ ] **Step 5: 默认参数必须让高光不饱和（D19 / Review Focus 1）**

`--shade` 默认 `phong`；默认 `keyIntensity 0.35`、`pointIntensity 1.0`、`Ks 0.35`、`Ns 64`。
`SR_TEST(default_rig_on_the_lit_sphere_does_not_saturate)`（M6.8 的 #7a 在这里先落一半）：
用默认 rig 渲一个球，断言全图最大通道 `< 255` **且**高光像素数 `> 0`。

- [ ] **Step 6: 全绿 + 变异验证 + 提交**

- [ ] **Step 7: ⛔ 肉眼门（纪律 #4 的第一次落点）**

请产品方跑：
```bash
./build/Debug/soft-raster.exe --shade phong          # 默认：看得见且不饱和的高光带
./build/Debug/soft-raster.exe                        # 无 --shade：应与上一张一致（默认就是 phong）
./build/Debug/soft-raster.exe --obj assets/obj/twomats.obj
```
要确认四件事：高光**位置**随拖拽移动（不是钉在屏幕中心不动）；亮度**不饱和成白饼**；
按 `3` 切到 Gouraud 时**高光是摊开的一片而不是一个点**（这就是本章要教的差别）；
`V` 的法线图与模型朝向一致。产品方说"对"之前**不进 M6.8**。

---

## Task M6.7: 叫醒分阶段计时与 `--bench`（D21）

**Files:**
- Modify: `src/render/render_backend.cpp`（四个阶段边界各一次 `lapMs()`）、`src/render/stats.h`（注释更新）
- Modify: `src/app/cmdline.cpp:185-191`（`--bench N` 设 `frames = N`）、`src/app/main.cpp:319-321`（删 M0 占位文案，改成打印分阶段表）
- Test: `tests/unit/test_pipeline_integration.cpp`（追加）

**Interfaces:**
- Consumes: `sr::Timer::lapMs()`（`bench/timer.h:29-34`，**已存在且正是为此写的**）、`FrameStats::msVertex/msClip/msSetup/msRaster`。
- Produces: 无新接口；把四个"声明了但没人写"的字段变成有值的。

- [ ] **Step 1: 写失败测试**

```cpp
SR_TEST(stage_timers_are_written)          // §8.1 #7：一次 draw 之后四个字段全部 > 0
SR_TEST(stage_timers_partition_the_total)  // sum(ms*) ∈ [0.5, 1.05] × msTotal
SR_TEST(bench_flag_runs_the_requested_number_of_frames)   // 今天 --bench 60 只跑 1 帧
```
第二条是本任务的**牙齿**：只断言 `> 0` 挡不住"四个字段都写同一个数"这种糊弄写法；
求和与 `msTotal` 的关系才证明它们**划分**了工作。区间上界 1.05 容忍计时自身开销，
下界 0.5 容忍机器抖动（**实测比值写进注释，不许只留一个魔法数**）。

- [ ] **Step 2: 跑测试确认失败**（四个字段全是 0 ⇒ 前两条红；`--bench` 那条红在帧数）

- [ ] **Step 3: 在 `drawRange` 里放 lap**

一个 `Timer stage;` 在三角形循环外；循环内按 `VS → clip → (rasterOne 内) project+setup → pixel loop`
四个边界各 `stats->msXxx += stage.lapMs()`。
**每三角形至多 4 次读时钟，绝不逐像素**（C5）。
`rasterOne` 是 lambda，lap 要放在它内部的两段之间（`sv/setupTriangle` 与 `forEachCoveredPixel`）。
扇形重三角化会让 setup/raster 段被多次进入 ⇒ 累加，不是覆盖。

- [ ] **Step 4: `--bench` 修 + 打印分阶段表**

`cmdline.cpp` 的 `--bench` 分支加 `out.frames = v;`；
`main.cpp:319-321` 那句 M0 占位文案换成打印 `msVertex/msClip/msSetup/msRaster/msTotal`
与 `trianglesSubmitted/fragmentsShaded/drawCalls` 的一行表。

- [ ] **Step 5: A/B 帧时间表（D8）**

同一场景（lit 球 + 默认 rig + 三线性贴图另测一组）跑三种着色模式各 ≥60 帧，
把 `msTotal/帧`、`fragmentsShaded`、四个阶段时间记进 `docs/notes/M6-*.md` 的表。
**notes 里必须写清计时本身的开销**（Step 1 的第二条断言给出的比值就是证据）。

- [ ] **Step 6: 全绿 + 变异验证（删掉任一 `msXxx` 的写入 ⇒ #7 与 #16 必须红）+ 提交**

---

## Task M6.8: lit golden —— 参考图、长标签条、三条破坏性验证

**⚠ 进入本任务前必须已完成 M6.6 Step 7 的肉眼门（纪律 #4）。**

**Files:**
- Modify: `src/scene/golden_scene.h/.cpp`（`renderGoldenSceneLit` + 标签 `"GOLDEN M6 LIT 320X180"`）
- Modify: `src/app/cmdline.h/.cpp`、`src/app/main.cpp`（`--golden-write-lit`，与另外三个 golden/emit 入口互斥）
- Test: `tests/unit/test_golden_lit.cpp`（新建）+ `tests/golden/golden.cpp`（注册 MSVC-only 比对）

**Interfaces:**
- Consumes: `makeUVSphere`、`litDefaultMaterialShade`、`blinnPhongPerPixelFS`、`goldenBackground/goldenSampler`（M5 已导出）、`kGoldenWidth/kGoldenHeight`、`readBmp24`。
- Produces: `void renderGoldenSceneLit(RenderTarget& target, FrameStats* stats);`
  与既有两个 golden 函数**同一个 `void(*)(RenderTarget&, FrameStats*)` 签名** ⇒
  `writeGolden` 不需要 `std::function` 加宽（M5.7 的那次加宽是因为要带 `LoadedModel`，这里不需要）。

- [ ] **Step 1: 场景函数**

无贴图球（`makeUVSphere(1.0f, stacks, slices)`，**`slices` 取奇数** ⇒ 没有顶点正对相机，
这是 §8.2 夹具约束第 2 条，注释必须写明原因）；姿态沿用 `goldenModel()/goldenView()/goldenProjection()`
（**不引入 fit**，D15 判例）；`vu.lights`/`vu.mat`/`fu.lights`/`fu.mat` 指向**同一个**函数内 `static const`
rig 与 `litDefaultMaterialShade()`；HUD 标签 `"GOLDEN M6 LIT 320X180"`（20 字符）。

- [ ] **Step 2: `--golden-write-lit` 与互斥**

第四个显式入口，无默认场景选择（Ruling 38）；与 `--golden-write`/`--golden-write-textured`/
`--golden-write-obj`/`--emit-checker-tga` 互斥的计数逻辑扩成 5 选 1（M5.3/M5.7 的既有写法）。

- [ ] **Step 3: 写参考图（需要肉眼门已过）并注册比对**

```bash
./build/Debug/soft-raster.exe --golden-write-lit assets/golden/m6_lit_sphere.bmp
```
```cpp
SR_TEST(golden_m6_lit_sphere_matches_reference)     // MSVC-only，±1/通道，g++ 打印 [skip]
SR_TEST(lit_golden_is_deterministic)                // 同一函数两次调用逐像素相同（M4 先例）
SR_TEST(lit_golden_has_a_highlight_that_does_not_saturate)   // §8.2 #7a：峰值通道 < 255 且高光像素 > 0
```

- [ ] **Step 4: 三条量化断言（§8.2 #7b/#7c/#8）**

```cpp
SR_TEST(gouraud_smears_the_specular_peak_that_per_pixel_localizes)   // 半高宽度：逐像素 < Gouraud，比值 ≥ 实测阈值
SR_TEST(the_per_pixel_highlight_center_moves_with_the_eye)           // 只改 eyeWorld ⇒ 重心移动；只改 keyDir ⇒ 也移动
SR_TEST(interpolation_kink_at_triangle_edges_is_stronger_under_gouraud)  // 指定扫描线的二阶差分比值
```
**三条都必须在 Step 1 的夹具上先实测再写死阈值**，注释里留下实测值与单位。
第三条的名字与措辞必须避开"Mach band"这个词的感知含义（§14 第 6 条：那是图要说明的，不是断言）。

- [ ] **Step 5: 三条破坏性验证（§8.3 #13/#15/#16）—— 本任务的交付物本体**

逐个做，每个记录"红在哪条断言、差多少像素、第一个在哪里"，然后撤销：
① `normalMatrix` 改回 `identity` ⇒ lit golden **必须红**（本章地板）；
② `worldPos` 改回 `in.position` ⇒ #7c 必须红；
③ 排除矩形沿用 M5 的 `x<110, y<12` ⇒ lit golden **必须红**（标签条 20 字符，墨迹到 x≈123）；
④ 删掉某个 `msXxx` 的写入 ⇒ `stage_timers_partition_the_total` 必须红。
**若 ① 不红，M6 的全部结论作废** —— 那意味着 lit golden 不依赖法线矩阵，
说明光照走的是别的路径（例如某处仍在用视图空间技巧，反目标 A1）。

- [ ] **Step 6: 全绿（含 M3/M4 两份 golden 不变）+ 提交**

---

## Task M6.9: 收尾 —— notes、README、grep 守卫、回写旧文档、三侧全量

**Files:**
- Create: `docs/notes/M6-光照与休眠接口.md`
- Modify: `README.md`、`docs/superpowers/specs/2026-11-22-soft-raster-m5-obj-design.md`（回写 D5）、`docs/superpowers/specs/2026-11-21-soft-raster-design.md`（§7.6 按键表）、`assets/golden/README.md`（第三张参考图的生成命令与门禁）
- 账本：`.superpowers/sdd/2026-11-25-soft-raster-m6-lighting/progress.md`（**不入库，不许删**）

- [ ] **Step 1: 三侧全量 + 五条 grep 守卫**

```bash
cmake --build build --config Debug 2>&1 | grep -E "warning|error"
ctest --test-dir build -C Debug --timeout 60
cmake --build build --config Release --clean-first 2>&1 | grep -E "warning|error"
ctest --test-dir build -C Release --timeout 60
cmake --build build-gcc 2>&1 | grep -E "warning|error"; ctest --test-dir build-gcc --timeout 60
grep -rn "stb_image" src/                                   # 只命中 image_stb.*
grep -rn '\.string()' src/                                   # 只允许命中注释；逐条读，不许只数数（Ruling 62）
grep -rn 'normalMatrix = Mat4::identity' src/                # 生产侧必须零命中（§8.4 新守卫）
grep -rn 'windows.h' src/render src/math src/scene src/loader # 零命中（C4）
git show HEAD:assets/golden/m6_lit_sphere.bmp | od -c | head -2   # 二进制未被 eol 归一化
```

- [ ] **Step 2: notes 的五节（写"为什么"）**

① **休眠接口**：三个字段/一个矩阵/四个计时器（§1 与 §1.1 的全表），
以及"正确的配方活在测试夹具里、生产侧一处没有"这条更尖锐的说法；
② **inverse-transpose 是几何必然**：`scale(2,1,1)` 的推导 + Step 那条点积断言；
③ **为什么 Gouraud 够不到高光峰**：凸组合论证 + **为什么第一版那条"最大亮度"断言是错的**
（饱和 clamp + 头灯几何下顶点正对相机）—— 这一节是本章最有教学价值的部分，
因为它展示了"一个看起来像数学常识的断言如何被一个具体夹具推翻"；
④ **线性空间与 sRGB**：光色/材质色/顶点色三者的解码时机表，
以及"输出侧恒定编码 ⇒ 法线视图的 0.5 变成 132 而不是 128"这个 wart 的说明；
⑤ **A/B 帧时间表**（M6.7 Step 5 的实测），并明确写"这批数字是 M9 结论的前置"。
另外把 M5 的 D5 审视结论（D15/D19）写成一段，指向 M5 设计文档的回写。

- [ ] **Step 3: README**：M6 行打勾 + 图 + 按键表加 `3`/`[`/`]`/`-`/`=`/`A`/`0` +
新 flag 示例 + `--bench` 现在真的跑 N 帧的说明。

- [ ] **Step 4: 回写两份上游文档**
M5 设计的 D5 加一句"审视结论见 M6 设计 D15/D19（已实施）"；
上游 spec §7.6 的按键表补 M6 新增键，并注明"分阶段计时在 M6.7 落地"。

- [ ] **Step 5: 向产品方提一件本里程碑发现但未自行处理的既有事实**
`FrameStats` 的四个 `ms*` 字段从 M2 起就一直是 0，而 `Timer` 的头注释写着"用于分阶段耗时"
（`bench/timer.h:3-5`）—— 也就是说**注释承诺了一件从未发生的事，持续了四个里程碑**。
是否要在 `assets/golden/README.md` 或 notes 里点名这条历史，由产品方定；本计划只保证 M6 之后它是真的。

- [ ] **Step 6: 账本写完 + 终审前自查**
`git diff main --stat` 逐文件过：残留调试打印、被注释掉的测试、`#if 0`、`sr_tmp_bmp/` 有没有误入库。

- [ ] **Step 7: 提交，走 `superpowers:requesting-code-review` → 整枝终审 → 合并**
按 M5 的节奏：fresh-context 整体评审覆盖 `main..HEAD` 全部 M6 提交，Critical/Important 修完再合并。
push 由产品方定（本机 push 常被重置）。
