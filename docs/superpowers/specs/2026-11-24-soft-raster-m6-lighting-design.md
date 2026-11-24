# M6 设计 — 光照，以及"休眠接口"这个失效模式

产品方 2026-11-24 在三个方案里选了 **C**：一盏跟随相机的点光 + 一盏方向光，
光源参数从命令行来、按键微调，`3` 切着色模式。其余 **D-nn 都是我（builder）的裁决**，
每条附理由与代价。

权威上游：[`2026-11-21-soft-raster-design.md`](2026-11-21-soft-raster-design.md)
§2（G3/G4/G5）、§3（N2/N6）、§4（A1/A3）、§5（C1′/C4/C5）、§7.1（线性空间着色）、
§7.3（内置 shader 集合与 SIMD 边界）、§7.6（按键表）、§8 的 M6 行。
前章：[`2026-11-22-soft-raster-m5-obj-design.md`](2026-11-22-soft-raster-m5-obj-design.md)
（D5 明写"M6 换 shader 时要重新审视 `Kd` 在解析时解码这件事"—— 本文 §6.3 就是那次审视）。

---

## 1. 这一章真正的主题：休眠接口

M3 为了光照预留了三样东西，每一样都写了注释说明"M6 才用"：

| 东西 | 现在的事实 | 核过的位置 |
|---|---|---|
| `Varyings::worldNormal` | **没有任何 shader 读它** | `builtin_shaders.cpp` 四个 FS 全文 |
| `Varyings::worldPos` | **没有任何 shader 读它**，而且**它是物体空间位置**，不是世界空间 | `builtin_shaders.cpp:13` `v.worldPos = in.position;` |
| `VertexUniforms::normalMatrix` | **每个调用方都传 `identity`**，包括那个 `model = rotX(0.43)·rotY(0.61)` 的 golden | `main.cpp:150`、`golden_scene.cpp:83/111`、`obj_scene.cpp:24`、`test_draw_range.cpp:50` |

第三条是**当前就存在的错**，不是"M6 才会引入的错"：纯旋转时 `transpose(inverse(R)) = R`，
而 `transformVS` 做的是 `normalMatrix * normal`，所以 golden 场景的 `worldNormal` 现在是**未旋转的法线**。
它今天无害，**因为没人读它**——而这正是问题：一个接口可以带着错误语义活过两个里程碑，
因为没有任何测试读过它。

更说明问题的是：**`Mat4::tryInvert`、`Mat4::transposed()`、`Mat4::upper3x3AsMat4()` 全都已经写好了**
（`mat.h:26-31`，带奇异检测、带"阈值取相对量级因为模型单位可能是毫米"的注释、`test_mat.cpp:108-132` 有测试），
`grep -rn 'makeNormalMatrix\|tryInvert' src/` 显示**除了 mat.h 自己，没有一处调用**。
基础设施为 M6 建好，然后 M6 之前没人用它。

**所以 M6 的第一个任务不是写 shader，是把休眠接口叫醒并证明它醒着。**
本文 §8 的验证设计全部围绕这一句：每条新测试都必须**因为读到了那个字段**而能红，
而 M6 的 golden 必须带一条破坏性验证——**把 `normalMatrix` 改回 `identity`，golden 必须红**。
那条断言是"接口真的被消费了"的唯一硬证据；没有它，M6 可以全绿地光照在错误的法线上。

### 为什么这不是吹毛求疵

本项目已经为同一族付出过代价：M5 的"两个翻转互相抵消"（±1 容差下完全不可见，
零容差下只留 3 个像素的浮点舍入残差）与 M4 的"夹具在某个对称操作下不变"（五次）。
它们共同的形态是**代码正确、测试绿、画面看着对，而语义是错的**。
休眠接口是这个族的第六次，而且它更隐蔽：不是夹具对称，是**根本没有人读**。

## 2. 目标

- G-M6.1 三种着色模式并存、按键实时切换、同一场景同一相机：`unlit`（现状）/ `lambert_pervertex`（Gouraud）/ `blinn_phong_perpixel`（终态默认）。
- G-M6.2 球体上看得见**高光带**，且 Gouraud 与逐像素的差别**可被断言**（不是"看起来糊一点"）。
- G-M6.3 全部光照运算在**线性空间**，只在写颜色目标那一刻编码 sRGB（spec §7.1:165）。
- G-M6.4 光源参数（颜色、强度、方向、点光位置、衰减开关）从命令行来，可按键微调，**当前值显示在 HUD 上**。
- G-M6.5 `debug_normal` 视图（spec §7.3 的 `debug_*` 家族、G5 要求的法线图），实现为普通 FS。
- G-M6.6 一张 lit golden 进回归；一组 **A/B 帧时间实测**（unlit vs Gouraud vs Phong）进 notes，为 M8 的 G3 与 M9 的"shading 才是瓶颈"提供第一批数字。

## 3. 非目标

- 阴影（shadow map）、PBR、GI/光追、延迟管线、MSAA —— spec N2 明列为毕业后方向。
- 多光循环 / 光列表。本里程碑**恰好两盏**（产品方选的 C），且这是有意的：
  spec §7.3 明确说 SIMD 不含 shading，所以一个逐像素的光循环会直接吃掉 M8 的帧预算，
  而"两盏、无循环"让 shader 保持直白（反目标 A1）。
- 聚光灯（spot）、面光、辐照度立方图、发光材质（emissive 只作为 `Ke` 存而不用，见 D9）。
- 光照贴图 / 预计算。
- 逐像素 `powf` 的向量化或查表优化（N6：为了超过 15 fps 而做的 shading 优化不在范围内；
  为了**达到**它而做的不在 M6，在 M8 的 tile/线程）。
- 法线贴图（M5 的 `Texture::srgb=false` 已经为数据纹理留了开关，但没有 `map_Bump` 解析）。

## 4. 反目标（跑起来也算失败）

- **A1 用视图空间技巧绕开世界空间**：把光照全算在 view space 就不需要 `worldPos`/`worldNormal` 了，
  画面也对——但那正好让休眠接口继续睡，把 M6 的核心交付物变成零。**明令禁止**（见 D4 的取舍）。
- **A2 把光照写进光栅后端**：`raster_scalar.cpp` / `render_backend.cpp` 不许出现点积、半向量、`pow`。
  光照属于 FS（spec §7.3），后端只管覆盖/深度/写颜色。
- **A3 数字对但没人知道为什么**（spec §4 A3）：`N·L`、半向量、衰减、`pow` 的指数与
  **为什么 Gouraud 的高光必然比逐像素暗**，都必须有推导注释 + 一条能红的断言。
- **A4 用"看起来有高光"当验收**：高光带必须能被两条独立的量化断言钉住（§8.2），
  其中一条是破坏性的（normalMatrix→identity ⇒ golden 红）。
- **A5 里程碑结束时窗口里没有变化**（spec §8 的地板）。

## 5. 绑定本里程碑的既有约束

- **C1′**：golden 只在 MSVC 判定；**M6 让这条真的开始咬人**——`powf(N·H, Ns)` 是逐像素的，
  而 MSYS2 g++ 的 `pow` 与 MSVC 不保证逐位一致。所以 lit golden 必须 MSVC-only，
  且**所有涉及 `pow` 的单元测试断言一律容差**（`1e-5` 量级，个别跨编译器可能到 `1e-4`，逐条标注实测值）。
- **C5**：逐像素不许虚派发、不许分配。`LightRig` 因此必须是**每帧一份的普通结构体 + 指针**，
  FS 里不许构造临时对象、不许 `std::vector`。
- **C4**：core 无 Win32。`LightRig`、`makeNormalMatrix`、三个 shader 全在 core。
- **`Varyings` 的 12 个 float 与那条 `static_assert`**（`vertex.h:15-24`）是接口的一部分。
  **D6 决定不新增槽位**，所以 M6 结束时 `kVaryingFloatCount` 仍是 12、断言仍成立。
- **§7.3 的内置 shader 集合**照抄，不自造名字：`unlit` / `lambert_pervertex` / `blinn_phong_perpixel` / `debug_*`。
- **G8/§10.4**：`docs/notes/M6-*.md` 是验收条件。
- **验证纪律四条**（产品方 2026-11-21）：变异验证、构建失败不读测试、Release `--clean-first`、
  **golden 生成前必须产品方亲眼验收**。第四条对 M6 尤其关键：lit golden 烘进去之前，
  产品方必须在窗口里看过高光带并说"位置对、强度对、切 Gouraud 看得出差别"。

## 6. 方案对比（选定 C 的展开 + 四处内部取舍）

### 6.1 光的形态（产品方选 C）

| | 形态 | 取舍 |
|---|---|---|
| A | 一盏方向光 + 一盏补光，参数写死 | 最省，但 spec 的"光源参数"只剩材质项；**且没有视点相关项 ⇒ 看不见高光带** ⇒ 直接不满足 G-M6.2 |
| B | 一盏方向光 + N 盏点光（N≤4），参数可调 | 教科书完整形状，但逐像素一个循环；§7.3 明说 SIMD 不含 shading ⇒ 循环直接吃 M8 的预算；且违反 A1"抽象止步于阶段边界"之外的简洁性 |
| **C（选定）** | **一盏点光跟随相机 + 一盏方向光** | 高光必须有视点相关项才看得见，相机附近的点光是**最容易验证**的形态（`V` 与 `N` 的关系一眼能判，法线视图立刻能对照）；两盏 ⇒ shader 里没循环、直白；参数从命令行来、按键微调 |

代价：点光跟随相机 = 观感接近"头灯"，模型永远被正面照亮，**自阴影缺失会更明显**
（没有阴影是 N2，但"头灯让凹凸变平"是真实的视觉后果）。缓解：方向光的默认方向刻意与相机视线
成约 35°，notes 里用一张对比图说明这件事，并把"关掉点光只留方向光"做成一个按键状态。

### 6.2 光源放进哪个 uniforms（内部取舍）

`VertexUniforms`（`pipeline.h:30-35`）被固定功能阶段读；`FragmentUniforms`（`:39-46`）被 FS 读。
M4 的 Ruling A 把采样状态放进 `FragmentUniforms`，理由是"那是 FS 的事"。
**但光不是 FS-only 的**：Gouraud 是**逐顶点**着色，它必须在 VS 里读光。

- 方案一：光放 `FragmentUniforms`，Gouraud 用另一个 VS-only 的字段 ⇒ 两处各一份，会漂。
- 方案二：光塞进 `VertexUniforms` ⇒ FS 也得读 `vu`，而 FS 的签名只收 `FragmentUniforms`。
- **选定（D1）**：一个 `struct LightRig`，`VertexUniforms` 与 `FragmentUniforms` **各持一个 `const LightRig*`**，
  指向每帧同一份。理由：单一真源、两个阶段都能读、指针解引用在 VS/FS 入口各一次（不是逐像素，C5 无碍）；
  `nullptr` = 无光 ⇒ `unlit` 语义自然落地（不需要第四个 shader 状态位）。
  代价：两个 uniforms 结构体多一个字段，且"两边必须指向同一个对象"是一条新的不变式 ⇒ 写成注释 +
  一条测试（VS 与 FS 读到同一盏灯：把 rig 里方向光的颜色改一次，Gouraud 与 Phong 的输出必须同步变）。

### 6.3 `Kd` 在解析时解码 vs 贴图在采样时解码（M5 D5 预告的那次审视）

M5 的 D5 让 `Kd/Ka/Ks` 在**解析时**解码成线性，贴图像素在**采样时**解码，并承认"这两件事不对称"。
M6 第一次把 `Kd` 乘进光照方程，所以这次审视的结论必须写死：

- **D2：保持解析时解码**（`Kd` 是文件里的浮点数，没有"采样时"这个时刻），
  并把**光源颜色与强度也按同一规则处理**——命令行给的 `0.8 0.8 0.8` 是 display-referred 的显示值，
  必须 `srgbToLinear` 之后才进光方程。
  理由：不一致的后果是"光源比材质亮一档"这种无因偏差，而 M5 已经为 `Kd` 立过一次同样的道理。
  代价：命令行输入不再是"所见即所得"的系数；HUD 因此**同时显示原始输入与解码后的线性值**（D10）。
- **D3：`Ns` 的语义定死**为 Blinn-Phong 的高光指数（MTL 惯例 0–1000），
  并在解析或着色时**夹到 `[1, 4096]`**；`Ns == 0` 视为"无高光"（跳过 `pow`），
  因为 `powf(x, 0) == 1` 会让整个表面镀一层均匀白——那是最容易被误认为"材质参数没生效"的症状。

### 6.4 世界空间语义（本章的修复主体）

**D4（明令禁止视图空间技巧）**：
- `transformVS` 改成 `v.worldPos = xyz(u.model * makeVec4(in.position, 1.0f))`。
  这是**行为改变**：今天所有调用方的 `model` 除 golden 外都是 identity，所以只有 golden 场景的
  `worldPos` 会变——而没有任何 shader 读它，所以**两份既有 golden 必须逐字节不变**，
  这本身就是"改了字段但没改画面"的证明（M5 Ruling 39 的先例）。
- 新增 `bool makeNormalMatrix(const Mat4& model, Mat4& out)`：
  `upper3x3AsMat4` → `tryInvert` → `transposed`，奇异 ⇒ 返回 false 且**不写 out**。
  基础设施已在（§1），所以这是 6 行的组合而不是新数学。
- **法线必须在变换之后归一化**：`worldNormal = normalize(normalMatrix * normal)`。
  非均匀缩放会改变长度（`scale(2,1,1)` 下 `x` 分量翻倍），变换前归一化等于没归一化。
  这条有一个漂亮的单元测试（§8.1）。
- 奇异 model（例如 `scale(0,1,1)`）⇒ `makeNormalMatrix` false ⇒ app 在 HUD 显示原因并退回 identity，
  **不崩、不黑屏**（与 M5.6 的"载入失败绝不空屏"同一条原则）。

**D5：`V`（视点方向）用世界空间相机位置**，来自 `OrbitCamera::eye()`（`camera.h:27` 已有），
经 `LightRig.eyeWorld` 传入。点光位置默认 = `eyeWorld`（跟随相机），可用 `--light-pos` 覆盖。

**D6：不新增 `Varyings` 槽位**。Gouraud 的着色结果写进**已有的** `v.color`（rgb = 线性光色，
`w` 留作 alpha 占位），因此 `kVaryingFloatCount == 12` 与那条 `static_assert` 不动。
理由：新增槽位会牵动 `plane_eq` 数组长度、`varyingFieldName`、M9 的 SIMD 布局——
而 Gouraud 的**全部教学价值**恰恰是"它只是在 VS 里算了本该在 FS 里算的东西"，
复用 `color` 槽让这个说法变成代码事实。代价：Gouraud 模式下顶点色被覆盖 ⇒
`vertexColorFS`（M3 的 golden）与 Gouraud 不能同时用；这没问题，golden 走 unlit 路径。

### 6.5 衰减

**D7：默认关闭**，`--attenuate` 或按键开。公式定死为 `1 / (1 + (d/radius)²)`，
`radius` 由命令行给（默认 = 场景包围球半径的 3 倍，所以默认"几乎不衰减"，
打开时看得见变化）。理由：关掉时 `L` 仍然随位置变化 ⇒ 高光带与 `V`/`N` 关系照样可验，
而"有没有衰减"是另一个独立的可断言效应；默认关也让 G3 的帧时间基线更干净。
代价：多一个分支（可预测，C5 无碍）。

### 6.6 `powf` 与性能

**D8：先用最直白的 `powf(dot(N,H), ns)`，不优化**（N6），但**必须实测**：
同一场景、同一相机、同一分辨率下跑 `unlit` / `lambert_pervertex` / `blinn_phong_perpixel` 各 N 帧，
把每帧毫秒与 `fragmentsShaded` 记进 notes 的表。
理由：spec §7.3 要求"收益预期提前写低"，而 M9 的教学点是"为什么 GPU 必须把 shader SIMT 化"——
**M6 的这张表是那两个论断的第一批数据**，没有它，M9 的结论就是空的。
不加新的计时器：`FrameStats` 已有 `msVertex/msClip/msSetup/msRaster`，
而 shading 的成本用 A/B 差值得到更可信（加一个 `msShade` 会引入每三角形两次读时钟的开销，
反而污染要测的东西）。

### 6.7 环境项与 `Ka`

**D9：接上 `Ka`**，`ambient = Ka * 环境光颜色`（环境光颜色默认 `0.02 0.02 0.02` 线性，可命令行给）。
理由：M5 存了 `Ka` 却没人读，而"存了永远没人读"正是本章要修的毛病；
接上它的成本是一行乘法。代价：`Ka` 全 0 的模型（Blender 默认导出常是 0）看起来没有环境光，
所以 HUD 要显示 `Ka` 的实际值——这也是 D10 的一部分。
`Ks` 接上（高光颜色），`Ns` 接上（D3）。`d/opacity` **维持 deferred #4 不动**（无 alpha 通路）。

### 6.8 HUD 与可见性

**D10：当前着色模式、两盏灯的有效值、以及"输入值 vs 线性值"必须都在 HUD 上**
（本项目原则："该看到什么"必须在画面上，`main.cpp:433` 与 `:205` 的既有注释）。
第二行加 `SHADE UNLIT|GOURAUD|PHONG`，第三行加
`KEY L <输入>→<线性> x<强度>  FILL …  Ka …  ATT on|off`。
理由：光强被 sRGB 解码过一次（D2），只显示输入值会让人算错期望；
而"为什么这个模型看起来全暗"的第一嫌疑人就是 `Ka`/光强。
代价：HUD 从两行变三行，`drawText` 的 y 偏移与截图对比区域要跟着改 ⇒
**golden 的 HUD 排除矩形必须重算**（这是 M5.7 那条"标签变长会响亮地红"的机制第一次真的被用到）。

## 7. 数据结构与接口（签名定死，实现留给计划）

```cpp
// src/render/lights.h（core，header-only + 一个 .cpp 放 makeNormalMatrix 之外的实现）
struct LightRig {
    Vec3  keyDirWorld{};        // 方向光的"照向"方向（单位向量）：从光指向场景
    LinearColor keyColor{};     // 已解码到线性空间
    float   keyIntensity = 1.0f;
    Vec3  pointPosWorld{};      // 点光位置（默认 = eyeWorld）
    LinearColor pointColor{};   // 已解码
    float   pointIntensity = 1.0f;
    bool    attenuate = false;
    float   attenuateRadius = 1.0f;
    LinearColor ambient{};      // 环境项，与 Material::ambient 相乘
    Vec3  eyeWorld{};           // D5：V 的来源
    // 两盏灯的"关掉"用 intensity = 0 表达，不用 bool —— 少一个状态位，且 HUD 显示一致。
};

// src/math/transform.h（追加，与 makePerspective/makeLookAt 同族）
bool makeNormalMatrix(const Mat4& model, Mat4& out);   // 奇异 ⇒ false 且不写 out

// src/render/pipeline.h（追加字段，两处各一个指针，指向同一份）
struct VertexUniforms   { /* ... */ const LightRig* lights = nullptr; };
struct FragmentUniforms { /* ... */ const LightRig* lights = nullptr; };

// src/render/builtin_shaders.h（追加，命名照抄 spec §7.3）
ClipVertex lambertVS(const VSIn&, const VertexUniforms&);        // Gouraud：结果写进 v.color
bool lambertPerVertexFS(const FragmentInput&, const FragmentUniforms&, FragmentOutput&);  // 直读插值后的 color
bool blinnPhongPerPixelFS(const FragmentInput&, const FragmentUniforms&, FragmentOutput&);
bool debugNormalFS(const FragmentInput&, const FragmentUniforms&, FragmentOutput&);
```

`transformVS` 本身**不改**（它继续只写几何 + 属性），光照 VS 是**新增**的 `lambertVS`。
理由：`transformVS` 被 M3/M4/M5 的六处 golden 与测试共用，改它等于把所有回归锚一起晃动；
新增一个 VS 让"哪个场景在用 Gouraud"变成显式选择。代价：两个 VS 里有重复的 MVP 三行乘法 ⇒
抽一个 `clipFromPosition(...)` 内部小函数，重复只留一处（这是 M5 的 `lex.h` 判例的直接应用）。

## 8. 测试与验证策略

### 8.1 叫醒休眠接口（本章的地板）

1. `normal_matrix_undoes_non_uniform_scale_for_normals` ——
   `model = scale(2,1,1)`，取一个法线 `n = normalize(1,1,0)` 的面：
   **朴素变换**（直接拿 model 的 3x3）得到的法线与表面**不垂直**，
   而 `makeNormalMatrix` 得到的垂直。断言用点积：`dot(n_out, tangent) ≈ 0`（容差 1e-5），
   而朴素版 `> 0.1`。**这是"inverse-transpose 不是仪式而是几何必然"的可执行证明。**
2. `worldPos_is_world_space_not_object_space` ——
   `model = translation(5,0,0)`，断言 `v.worldPos.x ≈ 5 + in.position.x`。
   （今天这条会红——**它就是那条"没人读"的证明**；先让它红，再修。）
3. `normal_is_normalized_after_the_transform_not_before` —— 非均匀缩放下长度断言。
4. `golden_scene_normal_matrix_is_no_longer_identity` ——
   对 `goldenModel()`（两个旋转的乘积）断言 `makeNormalMatrix` 的结果**等于该旋转本身**
   （纯旋转时 `transpose(inverse(R)) = R`），并且**不等于 identity**。
   这条同时证明 §1 说的那个错确实存在过、以及现在被修好了。
5. `makeNormalMatrix_rejects_a_singular_model_and_does_not_write_out` ——
   `scale(0,1,1)` ⇒ false、out 保持原值（与 `tryInvert` 的既有约定一致，`mat.h:26-28`）。
6. `lights_reach_both_stages_from_one_rig` —— 改 rig 里 `keyColor`，
   `lambertVS` 的输出与 `blinnPhongPerPixelFS` 的输出必须**同步**变化（D1 的不变式）。

### 8.2 高光带与 Gouraud-vs-Phong（G-M6.2 的可断言形式）

**关键：夹具必须非对称**（M4 的教训第四次应用）。
"两种着色都亮"不算差别；要断言的是**峰值与位置**：

7. `gouraud_cannot_reach_the_per_pixel_specular_peak` ——
   同一球体、同一 rig、同一相机，渲染两次；断言
   **逐像素图的最大亮度 > Gouraud 图的最大亮度**，且差值 ≥ 一个由 `Ns` 决定的阈值。
   推导（写进注释）：Gouraud 的亮度是顶点值的**凸组合**，
   而 `pow(N·H, Ns)` 在 `N·H → 1` 处极陡，峰值几乎必然落在顶点之间 ⇒
   顶点采样**结构性地**够不到它。这条断言在"把 Phong 也写成逐顶点"的变异下必须红。
8. `mach_band_exists_between_two_vertices_under_gouraud` ——
   Gouraud 图上沿一条扫描线找**二阶差分的极值**（Mach band 的定义性特征），
   逐像素图上同一位置的该极值显著更弱。容差按 C1′ 给，实测值写进注释。
9. `specular_highlight_moves_with_the_eye_not_with_the_light` ——
   半向量 `H = normalize(L + V)`：只改 `eyeWorld`、不改光方向 ⇒ 高光像素的重心必须移动；
   只改 `keyDirWorld`、不改 eye ⇒ 也必须移动。**这两条把"视点相关"与"光向相关"分开钉住**，
   也正是 A1（视图空间技巧）想绕开的那个事实。
10. `attenuation_only_reduces_the_point_light_term` ——
    `attenuate` 开/关各渲一次：方向光贡献不变（取一个只有方向光照到的点），
    点光贡献按 `1/(1+(d/r)²)` 变小。断言用两个具体像素的比值，不用全图。
11. `Ns_zero_means_no_specular_not_a_uniform_white_coat` ——
    `Ns = 0` ⇒ 输出与 `keyIntensity=0` 时的漫反射一致；
    这条直接钉死 D3 说的 `powf(x,0)==1` 陷阱。
12. `light_color_is_srgb_decoded_once` ——
    `--key-color 0.5 0.5 0.5` ⇒ rig 里的线性值是 `≈0.2140`（**不是** 0.5，也不是 0.2140 的平方）；
    与 `Kd` 走同一个 `srgbToLinear(float)`（D2 的一致性）。

### 8.3 破坏性验证（"接口真的被消费"的唯一硬证据）

13. **`normalMatrix` 改回 `identity` ⇒ lit golden 必须红**。
    这是 M6 最重要的一条：没有它，整个里程碑可以全绿地光照在未旋转的法线上。
    报告里必须留下"红了多少像素、第一个在哪里"。
14. `worldPos` 改回 `in.position` ⇒ 高光位置类断言（#9）必须红。
15. lit golden 的 HUD 排除矩形**必须重算**（D10 让 HUD 变三行）：
    把矩形改回 M5 的 `x<110, y<12` ⇒ 必须红（证明矩形是被需要的，而不是装饰）。

### 8.4 三侧与守卫

- MSVC Debug / MSVC Release `--clean-first` / g++ 三侧全绿零警告；
  lit golden 与 #8 这类含 `pow` 的比对**只在 MSVC 判定**，g++ 侧打印可见 `[skip]`（C1′，
  `golden.cpp:90-92` 与 `:102` 的先例）。
- `grep -rn "stb_image" src/`、`grep -rn '\.string()' src/`、`grep -rn "windows.h" src/render src/math src/scene src/loader`
  三条守卫照旧；**新增**一条：`grep -rn 'normalMatrix = .*identity' src/` 在 M6 之后应当**只剩测试夹具**，
  生产代码里一处不许有（这条把 §1 的错变成可 grep 的约束）。
- 变异验证逐条做，且必须给"删掉这道守卫/改成另一个值 ⇒ 哪条断言红"的对应表
  （M5 的 Ruling 67：**一个循环切换 K 份状态，测试往往只证明其中一份** ——
  M6 的对应物是"一个 shader 读 K 个字段"：`N`、`L`、`V`、`H`、`Ns`、光强、`Ka` 每一个都要有它自己能红的断言）。

## 9. app 侧

- `3` 切着色模式（`unlit → Gouraud → Phong → unlit`），与 `1`/`2` 同族 ⇒
  `InputState` 加 `cycleShadeMode`、`win32_window.cpp` 的按键 switch 加 `case '3'`
  （那个 switch 的注释已经说明数字键与字母键同族，`VK_1..VK_9` 在本机 SDK 的 WINVER 下不定义）。
- `[` / `]` 调**点光强度**（±10% 一档），`-` / `=` 调方向光强度；
  `A` 切衰减。理由：调光强是"看高光移动"的必需动作，而鼠标拖拽已经占了旋转/推拉。
  代价：4 个新键 ⇒ HUD 第二行的按键表要跟着长（D10）。
- 命令行：`--key-color`、`--key-dir`、`--key-intensity`、`--point-color`、`--point-pos`、
  `--point-intensity`、`--ambient`、`--attenuate`、`--attenuate-radius`、`--shade unlit|gouraud|phong`。
  全部走 `cmdline.cpp` 既有的 `Parser::nextFloat/nextString` 风格（带 flag 名的错误消息），
  `--shade` 与 `3` 键是同一个状态的初值与运行时切换，不冲突。
- **默认值必须让"什么都不传"就看得见高光带**：否则 G-M5.1 式的"立刻可见"在 M6 重演一遍。
  默认 `Ns = 64`（几何体无材质时）、点光强度 1.0、方向光强度 0.35、方向与视线成约 35°。

## 10. golden 与资产

- **新增一份 lit golden**：`makeUVSphere` + 两盏灯 + `blinn_phong_perpixel`，
  固定相机（沿用 `golden_scene.h` 的姿态常量风格，**不引入 fit**——M5 的 D15 判例在这里同样适用：
  fit 相机会让这张图无法与任何已有图交叉验证）。
  参考图由**生产代码里的那一个函数**生成（M4 Ruling 39 的先例），入口是独立 flag
  `--golden-write-lit <path>`（Ruling 38：两个显式入口、没有默认值 ⇒ 没有"忘了指定场景"的坑）。
- **不做 Gouraud-vs-Phong 的第二份参考图**：那两图的差别由 §8.2 的三条量化断言守，
  notes 里放一张并排图给人看。理由同 M5 收紧 2：**每多一张参考图就多一个"重新生成之后一切正常"的失效面**，
  而 Gouraud/Phong 的差别是**可断言的**（峰值、Mach band、高光随 eye 移动），不需要靠图守。
- 资产：M6 **不新增任何 committed 二进制**（球是程序生成的，灯是参数）。
  这条是刻意的：M5 已经证明"手写资产 + 生成资产"的 provenance 成本不低。

## 11. 顺手关掉的两条旧账

- **M5 D5 的"待 M6 审视"**：本文 §6.2/D2/D3/D9 就是那次审视的结论，
  并且要把结论**回写进 M5 设计文档的 D5**（一句话指向本文），
  否则下一个读 M5 设计的人会以为那条 TODO 还开着。
- **终审遗留的 P32/P34 一类"文档与实现漂移"**：M6 的 §1 那张表就是同族问题，
  所以 M6 的 notes 必须把"休眠接口"写成一条**一般规律**，而不只是"我们修了个 bug"。

## 12. 留给计划/实施者的决定

- 文件切分细节（`lights.h` 是否需要 `.cpp`；`debugNormalFS` 放 `builtin_shaders.cpp` 还是单独文件）。
- Mach band 断言的具体二阶差分公式与阈值（必须实测后写死，不许"跑一遍抄回来"——M5.2 的教训）。
- `lambertVS` 里 `v.color.a` 的取值（保持 1 还是沿用顶点色的 alpha）。
- 三行 HUD 的排版与 `drawText` 的 y 偏移；golden 排除矩形的新尺寸。
- 是否给 `--bench` 补一条"分着色模式"的输出（M6.7 的表需要它，但也可以靠脚本跑三次）。

## 13. 规模与切分

比 M5 小一档：**8 个任务、30–40 条新用例、一份新 golden、一篇 notes**。
建议切分（计划可调顺序，但 **M6.0 必须排第一**——它是本章的地板）：

| # | 任务 | 可见成果 |
|---|---|---|
| M6.0 | **叫醒休眠接口**：`makeNormalMatrix`、`worldPos` 改世界空间、变换后归一化、`grep normalMatrix.*identity` 生产侧归零（§8.1 的 6 条 + #14） | 单测；两份既有 golden **逐字节不变**（这就是"改了字段没改画面"的证明） |
| M6.1 | `LightRig` + 两个 uniforms 的指针 + 光色 sRGB 解码 + D3 的 `Ns` 夹取与 0 语义 | 单测（#6、#12） |
| M6.2 | `lambertVS` + `lambertPerVertexFS`（Gouraud 通路，复用 `v.color` 槽） | 单测 + 窗口里能切到 |
| M6.3 | `blinnPhongPerPixelFS`（`L`/`V`/`H`、`Ks`、`Ns`、`Ka` 环境项、衰减） | 单测（#7、#9、#10、#11） |
| M6.4 | `debugNormalFS` + `V` 视图第 4 项 + 法线视图的解析断言 | 窗口里 `V` 能看法线；一张图进 notes |
| M6.5 | app：`3`/`[`/`]`/`-`/`=`/`A`、命令行 10 个 flag、HUD 三行（D10） | **窗口里可调光、可切着色模式** |
| M6.6 | lit golden 场景函数 + `--golden-write-lit` + 排除矩形重算 + **破坏性验证 #13/#15** | 一张参考图进 `assets/golden/`（**需先过肉眼门**） |
| M6.7 | A/B 帧时间实测表（D8）+ `docs/notes/M6-*.md` + README + 回写 M5 D5 + 三侧全量 + push | 交付 |

**两个硬停点**（都是产品方 2026-11-21 的纪律 #4）：
M6.6 生成参考图之前，产品方必须在窗口里看过高光带并确认**位置对、强度对、切 Gouraud 看得出差别**；
以及 M6.5 结束时值得先看一次 `V` 视图——**法线图是 normalMatrix 修复最便宜的肉眼验证**，
它比任何断言都先暴露"法线没跟着模型转"。
