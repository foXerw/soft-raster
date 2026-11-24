# M6 设计 — 光照，以及"休眠接口"这个失效模式

产品方 2026-11-24 在三个光形态里选了 **C**：一盏跟随相机的点光 + 一盏方向光，
光源参数从命令行来、按键微调，`3` 切着色模式。其余 **D-nn 都是我（builder）的裁决**。

**本文的第二版吸收了 fresh-context builder 的 10 处纠正**（其中 4 处是关于既有代码的事实错误、
6 处是我提的断言在真实夹具上根本不成立）。逐条列在 §14。**第一版不能直接开工** ——
它会让 lit golden 烘进一张零高光的图，并让两条核心断言在饱和与顶点位置上必然失效。

权威上游：[`2026-11-21-soft-raster-design.md`](2026-11-21-soft-raster-design.md)
§2（G3/G4/G5）、§3（N2/N6）、§4（A1/A3）、§5（C1′/C4/C5）、§7.1（线性空间着色）、
§7.3（内置 shader 集合与 SIMD 边界）、§7.6（按键表）、§8 的 M6 行。
前章：[`2026-11-22-soft-raster-m5-obj-design.md`](2026-11-22-soft-raster-m5-obj-design.md)
（D5 明写"M6 换 shader 时要重新审视 `Kd` 在解析时解码这件事"—— 本文 D15/D19 就是那次审视）。

---

## 1. 这一章真正的主题：休眠接口

M3 为了光照预留了三样东西，每一样都写了注释说明"M6 才用"：

| 东西 | 生产代码里的事实 | 核过的位置 |
|---|---|---|
| `Varyings::worldNormal` | **没有任何 shader 读它** | `builtin_shaders.cpp` 的三个 FS 全文 |
| `Varyings::worldPos` | 没有任何 shader 读它，而且**它是物体空间位置**，字段名却叫 world | `builtin_shaders.cpp:13` `v.worldPos = in.position;` |
| `VertexUniforms::normalMatrix` | **生产侧四处全是 `identity`**，包括那个 `model = rotX(0.43)·rotY(0.61)` 的 golden | `main.cpp:150`、`golden_scene.cpp:83/111`、`obj_scene.cpp:24` |

第三条是**当前就存在的错**：纯旋转时 `transpose(inverse(R)) = R`，而 `transformVS` 做的是
`normalMatrix * normal`，所以 golden 场景的 `worldNormal` 是**未旋转的法线**。它今天无害，因为没人读它。

**评审纠正了我第一版的一处夸大，而纠正之后故事更尖锐**：不是"没人算对过"——
`tests/unit/test_pipeline_integration.cpp:25-26` 一直在算
`tryInvert(model) ? inv.transposed().upper3x3AsMat4() : identity`，**正确的配方活在测试夹具里，
生产代码里一处都没有**。一个已经写对、也已经测过的用法，绕开了所有真实调用点两年。

### 1.1 同一章里还有第二批休眠字段，而我第一版正靠它们做测量

| 字段/入口 | 事实 |
|---|---|
| `FrameStats::msVertex / msClip / msSetup / msRaster` | `stats.h:31-34` 声明、`merge()` 里加总（`:52-55`），**全仓库没有任何一处写它们**；`render_backend.cpp:138` 只写 `msTotal` |
| `--bench N` | `cmdline.cpp:185-191` 只设 `bench` 与 `offscreen`，**不设 `frames`** ⇒ `--bench 60` 实际跑 1 帧 |
| `runOffscreen` 的 bench 分支 | `main.cpp:319-321` 打印"[bench] 分阶段计时要到 M2 才有内容可测"—— M2 早已落地，这句话本身成了休眠文案 |

第一版的 D8 写着"`FrameStats` 已有分阶段计时，所以不需要新增"。**那句话是错的，而且错在本章正在谴责的地方**：
读到 0 会被当成"这个阶段很快"。所以 D21 把"叫醒这四个字段 + 修 `--bench`"纳入 M6，
理由不是性能测量本身需要它们，而是**留着它们就是留下一个会把 0 读成结论的陷阱**（G5 也要求逐阶段计时）。

### 1.2 因此本章的地板

M6.0 不是写 shader，是把休眠接口叫醒并**证明它醒着**：§8.3 的破坏性验证
"**把 `normalMatrix` 改回 `identity` ⇒ lit golden 必须红**"是本章最重要的一条断言。
没有它，整个里程碑可以全绿地光照在未旋转的法线上。

本项目为同一族付过五次代价（M4 的"夹具对称"五次、M5 的"双翻转抵消"）。
休眠接口是第六次，而且形态更隐蔽：不是夹具对称，是**根本没有人读**。

## 2. 目标

- G-M6.1 三种着色模式并存、按键实时切换、同一场景同一相机：`unlit`（现状）/ `lambert_pervertex`（Gouraud）/ `blinn_phong_perpixel`（终态默认）。
- G-M6.2 球体上看得见**高光带**，且 Gouraud 与逐像素的差别**可被断言**——断言必须在不饱和、且**没有顶点正对点光**的夹具上成立（§8.2）。
- G-M6.3 全部光照运算在线性空间，只在写颜色目标那一刻编码 sRGB（spec §7.1:165）。
- G-M6.4 光源参数（颜色、强度、方向、点光位置、衰减开关）从命令行来、按键可调、当前值显示在 HUD。
- G-M6.5 `debug_normal` 视图，实现为**普通 FS**（spec §7.3），并说清它为什么能和深度/灰度视图走不同机制（D23）。
- G-M6.6 一张 lit golden 进回归；一组 A/B 帧时间实测进 notes；`--bench` 与分阶段计时可用（D21）。

## 3. 非目标

- 阴影、PBR、GI/光追、延迟管线、MSAA（spec N2）。
- 多光循环 / 光列表。本里程碑**恰好两盏**（产品方选的 C）：§7.3 明说 SIMD 不含 shading，
  逐像素光循环会直接吃 M8 的预算，而"两盏、无循环"让 shader 保持直白（反目标 A1）。
- 聚光灯、面光、发光材质（`Ke` 不存在）、光照贴图、法线贴图（`map_Bump` 未解析）。
- **色调映射与 HDR**：不做。取而代之的是 D19——**默认参数必须让高光不撞 255**，
  因为一旦饱和，Gouraud 与逐像素两张图都变成白饼，本章的核心对比在像素层面消失。
- `powf` 的向量化/查表优化（N6）。
- 把深度/灰度视图改造成 FS（D23 说明为什么它们**不能**是 FS）。

## 4. 反目标（跑起来也算失败）

- **A1 用视图空间技巧绕开世界空间**：全算在 view space 就不需要 `worldPos`/`worldNormal`，画面也对，
  但休眠接口继续睡 ⇒ **明令禁止**（D4）。
- **A2 把光照写进光栅后端**：`raster_scalar.cpp` / `render_backend.cpp` 不许出现点积、半向量、`pow`。
- **A3 数字对但没人知道为什么**（spec §4 A3）：`N·L`、半向量、衰减、`pow` 指数、
  以及"为什么顶点处采样够不到高光峰"，都要有推导注释 + 一条能红的断言。
- **A4 用"看起来有高光"当验收**：高光必须被量化断言钉住，且**必须先证明它没饱和**（D19）。
- **A5 里程碑结束时窗口里没有变化**（spec §8 地板）。
- **A6 把 0 读成测量**：任何"字段存在但没人写"的状态都不许进入 M6 的结论（§1.1、D21）。

## 5. 绑定本里程碑的既有约束

- **C1′**：golden 只在 MSVC 判定；**M6 让这条真的开始咬人**——`powf(N·H, Ns)` 是逐像素的，
  MSYS2 g++ 与 MSVC 的 `pow` 不保证逐位一致 ⇒ lit golden 与一切含 `pow` 的比对 MSVC-only，
  单元测试的浮点断言一律容差。
- **C5**：逐像素不虚派发、不分配 ⇒ `LightRig`/`MaterialShade` 是每帧一份的普通结构体 + 指针。
- **C4**：core 无 Win32。
- **`Varyings` 的 12 float 与 `static_assert`**（`vertex.h:15-24`）是接口的一部分；D6 不新增槽位。
- **spec §7.3 的内置 shader 命名**照抄：`unlit` / `lambert_pervertex` / `blinn_phong_perpixel` / `debug_*`。
- **验证纪律四条**，其中 #4 在 M6 有两次落点（§13 的两个硬停点）。

## 6. 方案对比（选定 C 的展开 + 六处内部取舍）

### 6.1 光的形态（产品方选 C）

| | 形态 | 取舍 |
|---|---|---|
| A | 一盏方向光 + 一盏补光，写死 | 无视点相关项 ⇒ **看不见高光带** ⇒ 直接不满足 G-M6.2 |
| B | 一盏方向光 + N 盏点光 | 循环直接吃 M8 预算；违反 A1 的简洁性 |
| **C（选定）** | 一盏点光跟随相机 + 一盏方向光 | 高光需要视点相关项才看得见；相机附近的点光是**最容易验证**的形态；两盏 ⇒ 无循环 |

**代价（评审指出，必须写明）**：头灯式点光照亮整个可见半球 ⇒
① 凹凸变平、自阴影缺失更明显（N2 不做阴影，但这是真实视觉后果）；
② **"取一个只有方向光照到的点"这种测试夹具不存在**（§8.2 因此改用纯函数测试，D20）；
③ **高光峰值恰好落在正对相机的那个顶点上** ⇒ 第一版的 #7 断言在这个夹具上不成立（D19/§8.2 已改）。
缓解：方向光默认与视线成约 35°；`[`/`]` 是**加法**步进且能走到 0（D24），所以"只留方向光"可达。

### 6.2 光源与材质进 shader 的通路（评审 Q1 的裁决）

第一版只说了"光放 `LightRig*`"，**完全没说 `Ks/Ns/Ka` 怎么到 FS**——
而 `FragmentUniforms`（`pipeline.h:39-46`）今天只有 `baseColor/texture/sampler`，
`drawModel`（`model.cpp:229-250`）只把 `mat.diffuse` 抄进 `baseColor`。
照第一版实现会得到：**lit golden 的球体 `Ks=0`、`Ka=0`、`Ns=0` ⇒ 零高光 ⇒ G-M6.2 静默失败**。

- **D15（裁决）**：新增 `struct MaterialShade { LinearColor diffuse, ambient, specular; float shininess; }`，
  `VertexUniforms` 与 `FragmentUniforms` 各持 `const MaterialShade*`（与 `LightRig` 同一个指针模式，单一真源）。
  `drawModel` 逐组填它（它本来就知道每组是哪个材质）；
  **程序生成几何必须显式给一个非零的高光默认值**：`litDefaultMaterialShade()` =
  `diffuse 1 1 1`、`specular 0.35 0.35 0.35`、`shininess 64`、`ambient 0.02 0.02 0.02`。
  理由：`Material` 的默认是 `Ks=0/Ka=0/Ns=0`（`mtl.h:24-27`），
  那对"没写材质的 OBJ"是对的（尊重文件），对"我要演示高光"的 golden 是错的（会烘进一张白饼或一张无光球）。
  **代价**：两条默认路径 ⇒ 必须有一条断言钉住"lit golden 的高光像素数 > 0 且峰值 < 255"（§8.2 #7a）。
- **D16（裁决）**：**逐像素模式仍然采样 `map_Kd`**，albedo = `texel × diffuse`（都在线性空间）。
  理由：丢掉贴图会让带贴图的 OBJ 在切到 Phong 时**看起来像回归**，
  而且 D21 的 A/B 表要测的必须是 M8 真正要跑的那个工作负载（Blinn-Phong + 三线性，spec G3 原话）。
  代价：Gouraud 与逐像素的差别因此**有两个来源**（求值点 + 纹理细节）⇒
  **lit golden 用无贴图球体**，让两者只差在求值点；带贴图的差别由肉眼门与 notes 的图承担。

### 6.3 Gouraud 的 albedo 从哪来（顺手兑现一句旧承诺）

`main.cpp:161` 的注释写着 Mesh 的顶点色留着是因为"**M6 的光照 shader 还要它**"。
第一版从没消费顶点色 ⇒ 那句话会变成第七个休眠承诺。

- **D17（裁决）**：`lambertVS` 的 albedo = **顶点色 × `diffuse`**（VS 不采样贴图，也绝不为它加采样）；
  `blinnPhongPerPixelFS` 的 albedo = **`(texture ? texel : white) × diffuse`**。
  于是"顶点色在 Gouraud 下有作用、在逐像素下被贴图取代"成为**代码事实**，
  而那句注释被兑现；同时它给出一个免费的、可断言的差别：
  无贴图球体上两者只差求值点，有顶点色的立方体上两者还差 albedo 来源。
  代价：两个模式的 albedo 来源不同 ⇒ 断言必须成对设计（#7 用无贴图球，#12 用有贴图模型）。

### 6.4 世界空间语义（本章修复主体）

- **D4（禁止视图空间技巧）**：`transformVS` 的 `worldPos` 改成 `xyz(model * vec4(position,1))`；
  `worldNormal = normalize(normalMatrix * normal)`，**归一化必须在变换之后**
  （非均匀缩放改变长度，变换前归一化等于没归一化）。
  这是**行为改变**，但没有任何 shader 读这两个字段 ⇒ **两份既有 golden 必须逐字节不变**，
  那本身就是"改了字段没改画面"的证明（M5 Ruling 39 先例）。
  §7 会说"`transformVS` 不加光照"——**那是范围，不是"不改"**（第一版两处给了相反的理由，已修正）。
- **D18（法线矩阵的奇异兜底，与 grep 守卫的冲突这样解）**：
  新增 `bool makeNormalMatrix(const Mat4& model, Mat4& out)`（`upper3x3AsMat4` → `tryInvert` → `transposed`，
  奇异 ⇒ false 且不写 out；基础设施已在 `mat.h:26-31`，这是 6 行组合而非新数学）。
  app 侧**永远不写字面量 identity**：调用 `makeNormalMatrix`，失败时**沿用上一次的矩阵**（last-good）
  并在 HUD 显示原因。这样 §8.4 的 grep 守卫（生产侧 `normalMatrix = ...identity` 归零）
  与"奇异 model 不崩不黑屏"（M5.6 的同一条原则）**同时成立**。
  代价：多一个 last-good 状态位；`main.cpp:150` 那处今天其实是**对的**（那里的 model 真是 identity），
  也要改写成 `makeNormalMatrix(identity)` 的合法调用——这是守卫故意要的代价。

### 6.5 `V`（视点方向）

`OrbitCamera::eye()`（`camera.h:27`）经 `LightRig.eyeWorld` 传入；点光位置默认 = `eyeWorld`，可 `--point-pos` 覆盖。

### 6.6 不新增 `Varyings` 槽位

**D6**：Gouraud 的着色结果写进已有的 `v.color`（rgb = 线性光色），`kVaryingFloatCount` 仍是 12、
`static_assert` 不动。理由：新增槽位牵动 plane-eq 数组长度、`varyingFieldName`、M9 的 SIMD 布局，
而 Gouraud 的全部教学价值恰恰是"它只是在 VS 里算了本该在 FS 里算的东西"。
代价：Gouraud 覆盖顶点色 ⇒ `vertexColorFS`（M3 golden）与 Gouraud 不同时用；golden 走 unlit，无冲突。

### 6.7 `Ns` 与 `pow` 的两个陷阱

- **D3**：`Ns` 夹到 `[1, 4096]`；**`Ns == 0` 视为"无高光"并跳过 `pow`**，
  因为 `powf(x, 0) == 1` 会给整个表面镀一层均匀白——最容易被误认为"材质参数没生效"。
- **D19（饱和）**：`encodeSRGB`/`linearToSrgb8` 把输入 clamp 到 [0,1]（`color.cpp:21-27,64-69`），
  所以默认光强一旦让线性值超过 1，**两张图都在峰值处变成 255**，本章的核心对比在像素层面消失。
  裁决：默认参数必须让 lit golden 的峰值**留在线性 1.0 以下**，并有断言（#7a：峰值通道 < 255 且高光像素数 > 0）。
  这不是"更好看"，而是**让比较有意义的前提**。notes 里要放一张故意把强度调到饱和的图，
  说明"为什么很多渲染器需要色调映射"——本项目不做色调映射（N2/§3），但要说清它为什么存在。

### 6.8 衰减

**D7**：默认关，`--attenuate` / `A` 键开。公式 `1 / (1 + (d/radius)²)`，默认 `radius` = 场景半径 ×3。
理由：关掉时 `L` 仍随位置变化 ⇒ 高光与 `V`/`N` 的关系照样可验；衰减是另一个独立效应。
断言用**纯函数**（D20），不用"只有方向光照到的点"（那种点不存在，§6.1 代价 ②）。

### 6.9 光照数学要可单测

**D20（裁决）**：把光照本体抽成一个**自由纯函数**
`LinearColor shadeBlinnPhong(const Vec3& N, const Vec3& P, const LightRig&, const MaterialShade&, bool withTexture, const LinearColor& albedo)`，
FS 只负责取插值结果 + 调它；Gouraud 的 VS 调同一个函数的"只算漫反射"分支（或同一函数 + `Ns` 视为 0）。
理由：① 衰减/两项叠加/`Ns=0` 这些**必须能解析地测**，而"在整帧里找一个满足条件的像素"这类夹具
在头灯几何下不存在（评审抓出的 #10 之错）；② 反目标 A1 说抽象止步于阶段边界——
一个自由函数里的点积、半向量、`pow` 正是**直白的数学**，不是抽象。
代价：FS 与 VS 共用一个函数 ⇒ 该函数的参数表会变宽（`withTexture` 之类）；用两个入口
（`shadeDiffuseOnly` / `shadeBlinnPhong`）而不是布尔开关，评审的建议在此采纳。

### 6.10 debug 视图为什么有两种机制

**D23（debug 视图的两种机制）**：
`debug_normal` **必须是 FS**（spec §7.3 明写 `debug_*` 走正式 shader 同一条路），
因为它读的是**本像素插值出来的法线**——FS 才拿得到。
而 `depth` / `gray` 视图**不能是 FS**：深度视图要读整张深度缓冲（后处理），
灰度要读已经写好的颜色。把它们硬改成 FS 等于伪造一个不可能的接口。
裁决：**两种机制并存且各归其位**，`V` 键循环 4 个视图，app 侧把"视图"映射成
"FS 选择 + 是否再过后处理 sink"。第一版把这件事写成"实现为普通 FS"却没有说明现有三个视图是 sink
（`sink.cpp:17-53`、`main.cpp:376-383` 的 `% 3`），照那句话做会把深度视图也塞进 FS。
代价：两套机制并存 ⇒ 必须在 `sink.h` 与 notes 里写明分界（"能逐像素判定的走 FS，需要整缓冲的走 sink"），
否则下一个读代码的人会花一天去"统一"它们。

### 6.11 环境项与 `Ka`

**D9**：接上 `Ka`（`ambient = Ka × 环境光颜色`，默认线性 `0.02`）与 `Ks`、`Ns`。
理由：M5 存了 `Ka` 没人读，而"存了没人读"正是本章要修的毛病，成本是一行乘法。
代价：Blender 默认导出的 `Ka` 常是 0 ⇒ 看起来没有环境光，所以 HUD 显示 `Ka` 实际值（D10）。
`d/opacity` **维持 deferred #4 不动**（无 alpha 通路）。

### 6.12 HUD 与可见性

**D10**：HUD 加一行着色模式与光参数：`SHADE UNLIT|GOURAUD|PHONG`、
`KEY <输入>→<线性> ×强度`、`POINT …`、`Ka …`、`ATT on|off`。
理由：光强与颜色被 sRGB 解码过一次（D15/§6.2），只显示输入值会让人算错期望。
**并纠正第一版的一处混淆**：golden 参考图上的 HUD 是**一行 17 字符的标签条**
（`golden_scene.cpp:91,126`、`obj_scene.cpp:39`），与 app 窗口的多行 HUD 是两回事；
M5 的排除矩形 `x<110, y<12` 属于**标签条**。第一版说"HUD 变三行 ⇒ 排除矩形必须重算"是错的。
真正需要重算的是 lit golden 的标签条 ⇒ **D22：lit golden 的标签写成
`"GOLDEN M6 LIT 320X180"`（20 字符，墨迹到 x≈123 > 110）**，
这样"沿用 M5 的矩形"会**响亮地红**（§8.3 #15 因此才有牙齿——第一版里那条变异是惰性的）。

### 6.13 叫醒分阶段计时与 `--bench`

**D21（裁决）**：`FrameStats::msVertex / msClip / msSetup / msRaster` 今天**声明了、在 `merge()` 里加总了、
但没有任何一处写它们**（`render_backend.cpp:138` 只写 `msTotal`）；`--bench N` 不设 `frames`
（`cmdline.cpp:185-191`）所以 `--bench 60` 实际只跑 1 帧；`runOffscreen` 的 bench 分支还在打印
M0 时代那句"分阶段计时要到 M2 才有内容可测"（`main.cpp:319-321`）。
裁决：M6.7 把这三件事一起修好 —— 在 `drawRange` 的四个阶段边界上各放一个 `sr::Timer`
（**每 draw call 一次，不是每像素**，C5 无碍），`--bench N` 设 `frames = N`，删掉那句占位文案。

为什么在一个光照里程碑里做本来属于 M7/G5 的事：**本章的论点就是"声明了没人写的字段是个陷阱"，
而 D8 的 A/B 表第一批数据差点就建立在这些 0 上**。读到 0 的人会得到"这个阶段几乎不花时间"，
那是比没有测量更糟的东西。守卫是 §8.1 #7（`stage_timers_are_written`）与 §8.3 #16。
代价：每 draw call 多四次读时钟（`QueryPerformanceCounter` 量级，相对一次 draw 的工作量可忽略），
以及 M6 的范围比"光照"宽一点 —— 我判断值得，因为它是本章论证的直接产物。

## 7. 数据结构与接口

```cpp
// src/render/lights.h（core）
struct LightRig {
    Vec3 keyDirWorld{};         // 方向光"照向"的方向（单位向量）
    LinearColor keyColor{};     // 已解码到线性
    float keyIntensity = 1.0f;
    Vec3 pointPosWorld{};       // 默认 = eyeWorld
    LinearColor pointColor{};
    float pointIntensity = 1.0f;
    bool attenuate = false;
    float attenuateRadius = 1.0f;
    LinearColor ambient{};
    Vec3 eyeWorld{};
    // "关掉一盏"= intensity 走到 0（D24 的加法步进保证可达），不用 bool。
};
struct MaterialShade {          // D15
    LinearColor diffuse{1, 1, 1}, ambient{0, 0, 0}, specular{0, 0, 0};
    float shininess = 0.0f;
};
const MaterialShade& litDefaultMaterialShade();   // specular .35 / shininess 64 / ambient .02

// src/math/transform.h（追加）
bool makeNormalMatrix(const Mat4& model, Mat4& out);   // 奇异 ⇒ false 且不写 out

// src/render/pipeline.h（追加，两处各一个指针，指向每帧同一份）
struct VertexUniforms   { /* ... */ const LightRig* lights = nullptr;
                            const MaterialShade* mat = nullptr; };
struct FragmentUniforms { /* ... */ const LightRig* lights = nullptr;
                            const MaterialShade* mat = nullptr; };

// src/render/builtin_shaders.h（命名照抄 spec §7.3）
ClipVertex lambertVS(const VSIn&, const VertexUniforms&);
bool lambertPerVertexFS(const FragmentInput&, const FragmentUniforms&, FragmentOutput&);
bool blinnPhongPerPixelFS(const FragmentInput&, const FragmentUniforms&, FragmentOutput&);
bool debugNormalFS(const FragmentInput&, const FragmentUniforms&, FragmentOutput&);

// src/render/shading.h（D20）—— 光照本体，自由函数，可解析单测
LinearColor shadeDiffuseOnly(const Vec3& N, const Vec3& P, const LightRig&, const MaterialShade&,
                             const LinearColor& albedo);
LinearColor shadeBlinnPhong(const Vec3& N, const Vec3& P, const LightRig&, const MaterialShade&,
                            const LinearColor& albedo);
```

`transformVS` **不加光照**（范围），但**要改 `worldPos` 那一行**（D4）。
`lambertVS` 是新增 VS；`lambertVS` 与 `transformVS` 共用的 MVP 三行抽成内部小函数
（M5 的 `lex.h` 判例：逐字重复的逻辑块必须有唯一一份）。

## 8. 测试与验证策略

### 8.1 叫醒休眠接口

1. `normal_matrix_undoes_non_uniform_scale_for_normals` —— `model = scale(2,1,1)`，
   法线 `n = (1,1,0)/√2`，**并显式给出它所在的表面**：`t1 = (1,-1,0)/√2`、`t2 = (0,0,1)`。
   缩放后 `t1' = (2,-1,0)/√2`、`t2' = (0,0,1)`。
   - 朴素变换 `model * n = (2,1,0)/√2` ⇒ `dot(·, t1') = 1.5 ≠ 0`（断言 `> 0.1`）；
   - `makeNormalMatrix` ⇒ `normalize(0.5,1,0) ≈ (0.4472,0.8944,0)` ⇒ 与 `t1'`、`t2'` 都 `≈ 0`（容差 `1e-5f`）。
   **测试必须先断言前提**（`dot(n,t1) ≈ 0` 且 `dot(n,t2) ≈ 0`）再断言现象。
   这条"inverse-transpose 是几何必然不是仪式"的可执行证明，第一版只写了"与表面不垂直"而没给出表面，
   于是计划作者（我）顺手配了一个不可能的面（xy 平面配 `(1,1,0)/√2`）——
   **规格里出现"表面/法线"这类成对对象时，两个都必须写死，只写一个等于把矛盾留给下游发现**（M6.0 的实现者抓的）。
2. `worldPos_is_world_space_not_object_space` —— `model = translation(5,0,0)` ⇒
   `v.worldPos.x ≈ 5 + in.position.x`。**今天这条会红**，它就是"没人读"的证据。
3. `normal_is_normalized_after_the_transform_not_before`。
4. `golden_scene_normal_matrix_is_no_longer_identity` —— 对 `goldenModel()` 断言
   `makeNormalMatrix` 结果**等于该旋转本身**且**不等于 identity**。
5. `makeNormalMatrix_rejects_a_singular_model_and_does_not_write_out`（与 `tryInvert` 约定一致）。
6. `lights_and_material_reach_both_stages_from_one_source` —— 改 rig 的 `keyColor` 与
   `MaterialShade::specular`，Gouraud 与逐像素的输出必须同步变化（D15 的单一真源不变式）。
7. `stage_timers_are_written` —— 一次 draw 之后 `msVertex/msClip/msSetup/msRaster` **全部 > 0**（D21）。
   这条断言的存在本身就是对本章主题的负责：它让"声明了但没人写"从此不可通过。

### 8.2 高光带与 Gouraud-vs-逐像素（夹具必须非对称，且必须不饱和）

**夹具约束（三条，缺一条断言就失去意义）**：无贴图球体；`stacks/slices` 选成
**没有任何顶点正对点光**（例如 `slices` 取奇数，使 `θ = π/2` 不落在顶点上）；
默认强度让峰值线性值 < 1.0（D19）。

7a. `lit_golden_has_a_highlight_that_does_not_saturate` —— 高光像素（线性值 > 1.2× 局部均值）数 > 0，
    且**全图最大通道 < 255**。这条是 #7b/#7c 的前提；没有它，比较的是两块白饼。
7b. `gouraud_smears_the_specular_peak_that_per_pixel_localizes` ——
    同一几何同一 rig：逐像素图的**半高宽度**（峰值一半所围的高光区域面积）
    **严格小于** Gouraud 图的对应面积，比值 ≥ 实测阈值。
    推导（写进注释）：Gouraud 的高光是顶点值的**凸组合**，顶点处采样够不到 `N·H → 1`，
    能量被摊到整片三角形；逐像素在每个像素重算 `H`，因此集中。
    **注意第一版写的是"最大亮度严格更大"——那条在真实夹具上是假的**（饱和后两边都 255，
    且头灯几何下峰值恰好落在一个顶点上），已换成宽度断言。
7c. `the_per_pixel_highlight_center_moves_with_the_eye` —— 高光重心随 `eyeWorld` 移动，
    而漫反射项的重心基本不动 ⇒ 把"视点相关"与"光向相关"分开钉住（也是 A1 想绕开的事实）。
8. `interpolation_kink_at_triangle_edges_is_stronger_under_gouraud` ——
    **改名并重新定义**：沿一条指定扫描线取二阶差分，断言 **Gouraud 在三角形边界处的 kink
    相对其局部斜率的比值** 明显大于逐像素图。
    **第一版把它叫"Mach band"是范畴错误**：Mach band 是感知的过冲（侧抑制），不是像素性质；
    二阶差分能测的是**一阶导的不连续**。而且逐像素 Blinn-Phong 的法线在球面网格上**也只是 C0**，
    边界 kink 在两张图里都存在 ⇒ "逐像素显著更弱"不成立。notes 里 Mach band 用**图**说明，
    断言只测可测的东西。
9. `attenuation_matches_the_formula_analytically` —— 用 `shadeBlinnPhong` 纯函数（D20）
    在两个已知距离上比对 `1/(1+(d/r)²)`，**不依赖"找一个只有方向光照到的像素"**（那种点不存在）。
10. `Ns_zero_disables_specular_and_is_equivalent_to_Ks_zero` ——
    **第一版写成"与 `keyIntensity=0` 时的漫反射一致"是错的**：`Ns=0` 保留两盏灯的漫反射，
    `keyIntensity=0` 去掉方向光的漫反射但保留点光高光 ⇒ 正确实现下两者也不相等。
    已改成 `Ns=0 ≡ Ks=0`。
11. `light_and_material_colors_are_srgb_decoded_once` —— `--key-color 0.5 0.5 0.5` ⇒
    rig 里 `≈0.2140`（不是 0.5，也不是它的平方）；与 `Kd` 同一个 `srgbToLinear(float)`。
12. `textured_model_keeps_the_texture_under_phong` —— 带 `map_Kd` 的模型在 Phong 下仍然
    逐纹素变化（不是纯色），钉住 D16 的"逐像素模式仍采样贴图"。

### 8.3 破坏性验证（"接口真的被消费"的唯一硬证据）

13. **`normalMatrix` 改回 `identity` ⇒ lit golden 必须红**（本章的地板）。
14. `worldPos` 改回 `in.position` ⇒ 高光重心类断言（#7c）必须红。
15. **lit golden 的标签条比 M5 的长**（D22：20 字符，墨迹到 x≈123）⇒
    把排除矩形沿用 M5 的 `x<110, y<12` **必须红**。
    第一版说"HUD 变三行所以矩形必须重算"混淆了 app HUD 与 golden 标签条，且那条变异是惰性的，已重做。
16. 删掉 `stage_timers` 的写入 ⇒ #7 红（D21 的守卫）。

### 8.4 三侧与守卫

- MSVC Debug / MSVC Release `--clean-first` / g++ 三侧全绿零警告；含 `pow` 的比对 MSVC-only + 可见 `[skip]`。
- `grep -rn "stb_image" src/`、`grep -rn '\.string()' src/`、`grep -rn "windows.h" src/render src/math src/scene src/loader` 照旧。
- **新增守卫**：`grep -rn 'normalMatrix = ' src/ | grep identity` 在 M6 之后**生产侧零命中**
  （`tests/` 里的夹具不算，守卫脚本要显式区分；第一版写"只剩测试夹具"却 grep `src/`，是矛盾的）。
- 变异验证逐条做。**Ruling 67 的 M6 形态**：一个 shader 读 K 个字段
  （`N`、`L`、`V`、`H`、`Ns`、`Ks`、`Ka`、光强、`albedo`）⇒ **每一个都要有它自己能红的断言**，
  置空/置零不算（只证明"读了"，不证明"读对了哪一个"）。

## 9. app 侧

- `3` 切着色模式（`unlit → Gouraud → Phong`）⇒ `InputState.cycleShadeMode` + `win32_window.cpp` 的 `case '3'`
  （那个 switch 的注释已说明数字键与字母键同族，`VK_1..VK_9` 在本机 SDK 的 `WINVER` 下不定义）。
- **D24（裁决）**：`[` / `]` 调点光强度、`-` / `=` 调方向光强度，用**加法步进 ±0.1 并夹到 `[0, 3]`**。
  第一版写的是"±10% 一档"（乘法），那**永远到不了 0** —— 而 §6.1 明确承诺了
  "关掉点光只留方向光"是一个可到达的状态，notes 的对比图也依赖它。
  乘法步进还有一个更隐蔽的后果：从默认值出发按 N 下与按 N 上不对称，"回到默认"要靠 `0` 键而不是按回去。
  代价：0 附近一档就是 10% 的相对变化，微调手感比乘法粗 —— 用 `0` 键复位默认来补。
  `A` 切衰减；`0` 把两盏灯与衰减恢复到默认。
- 命令行：`--key-color --key-dir --key-intensity --point-color --point-pos --point-intensity
  --ambient --ambient-color --attenuate --attenuate-radius --shade`，全部走 `Parser::nextFloat/nextString` 风格。
- **默认值必须让"什么都不传"就看得见不饱和的高光带**（D19），否则 G-M6.2 与肉眼门一起失败。
- `--bench N` **修成真跑 N 帧**（设 `frames = N`），并删掉 `main.cpp:319-321` 那句 M0 占位文案（D21）。

## 10. golden 与资产

- 新增一份 lit golden：`makeUVSphere`（**无贴图**、`slices` 取奇数、姿态与相机固定）+ 两盏灯 +
  `blinn_phong_perpixel` + `litDefaultMaterialShade()`。
  参考图由生产代码里那一个函数生成（Ruling 39），入口是独立 flag `--golden-write-lit <path>`（Ruling 38）。
- **不做 Gouraud-vs-Phong 的第二份参考图**：差别由 #7a/#7b/#7c 三条量化断言守，notes 放并排图。
  理由同 M5 收紧 2：每多一张参考图就多一个"重新生成之后一切正常"的失效面。
- M6 **不新增 committed 二进制资产**（球是程序生成的，灯是参数）。

## 11. 顺手关掉的两条旧账

- **M5 D5 的"待 M6 审视"** → 本文 D15/§6.2/D19 就是结论，且要**回写 M5 设计文档的 D5** 一句话指向本文。
- **`main.cpp:161` 的顶点色承诺** → D17 兑现（Gouraud 的 albedo 用顶点色），
  并在该注释处补一句"逐像素模式下由贴图取代"，否则它又是一句没人消费的说明。

## 12. 留给计划/实施者的决定

- 文件切分（`lights.h` / `shading.h` 是否需要 `.cpp`；`debugNormalFS` 放哪儿）。
- #7b 的"半高宽度"具体度量（面积阈值 vs 半径阈值）与实测比值。
- #8 的扫描线选哪一条、比值阈值取多少（**必须实测后写死，不许跑一遍抄回来**——M5.2 的教训）。
- 分阶段计时用现有 `sr::Timer` 还是每三角形一次（D21 要求"不逐像素"）。
- 是否给 `--bench` 加"分着色模式"的输出，还是靠脚本跑三次。

## 13. 规模与切分

比 M5 略小：**10 个任务（M6.0–M6.9）、35–45 条新用例、一份新 golden、一篇 notes**。
（第一版写"8 个任务"而表里 10 行——本章正是讲"文档与实现对不上"的，不能自己犯。）

| # | 任务 | 可见成果 |
|---|---|---|
| M6.0 | **叫醒休眠接口**：`makeNormalMatrix` + last-good 兜底、`worldPos` 改世界空间、变换后归一化、生产侧 identity 归零（§8.1 #1–#5、#13/#14 的前提） | 单测；两份既有 golden **逐字节不变** |
| M6.1 | `LightRig` + `MaterialShade` + 两个 uniforms 的指针 + 光色 sRGB 解码 + `Ns` 夹取与 0 语义 + `litDefaultMaterialShade` | 单测（#6、#10、#11） |
| M6.2 | `shading.h` 纯函数（`shadeDiffuseOnly` / `shadeBlinnPhong`）+ 解析单测 | 单测（#9、#10）——**先有可测的数学，再有 shader** |
| M6.3 | `lambertVS` + `lambertPerVertexFS`（Gouraud，复用 `v.color` 槽，albedo = 顶点色 × diffuse） | 单测 + 窗口里能切到 |
| M6.4 | `blinnPhongPerPixelFS`（贴图 × Kd 的 albedo、`Ka` 环境项、衰减） | 单测（#7a 前置、#12） |
| M6.5 | `debugNormalFS` + `V` 循环 4 视图 + FS/sink 两种机制的分界写进 `sink.h` 与注释（D23） | 窗口里 `V` 能看法线图 |
| M6.6 | app：`3`/`[`/`]`/`-`/`=`/`A`/`0`、命令行 11 个 flag、HUD 加一行、默认参数调到不饱和 | **窗口里可调光、可切模式、看得见高光** |
| M6.7 | D21 三件：分阶段计时写入 + `--bench` 修 + 删 M0 占位；A/B 帧时间表；`stage_timers_are_written` | 第一批性能数据 |
| M6.8 | lit golden 场景函数 + `--golden-write-lit` + **长标签条与排除矩形重算**（#15）+ 破坏性验证 #13/#14/#16 | 一张参考图进 `assets/golden/` |
| M6.9 | `docs/notes/M6-*.md` + README + 回写 M5 D5 与 `main.cpp:161` 注释 + 三侧全量 + push | 交付 |

**三个硬停点**（纪律 #4 与其精神）：
① **M6.5 结束时先看一次 `V` 法线视图**——它是 normalMatrix 修复最便宜的肉眼验证，
比任何断言都先暴露"法线没跟着模型转"；
② **M6.8 生成参考图之前**，产品方必须在窗口里看过高光带并确认"位置对、强度对、没饱和成白饼、切 Gouraud 看得出差别"；
③ M6.2 之后**先别写 FS**——纯函数测试全绿才继续（否则错误会被插值、饱和、采样三件事盖住）。

## 14. 评审纠正了什么（留档，防止我再次写错）

1. **"每个调用方都传 identity"是夸大** —— `test_pipeline_integration.cpp:25-26` 一直在正确计算 inverse-transpose。
   正确说法：正确配方活在测试里，生产侧四处全是 identity。
2. **"四个 FS"→ 三个**（`flatColorFS`/`vertexColorFS`/`texturedFS`；debug 视图是 **sink** 不是 FS）。
3. **`test_mat.cpp:108-132` 只覆盖 `tryInvert`**；`transposed`/`upper3x3` 在 `:136`/`:148`。
4. **`msVertex/msClip/msSetup/msRaster` 从未被写**，`--bench` 不设 `frames`，`runOffscreen` 还打印 M0 占位文案
   ⇒ 第一版 D8"不需要新增计时"建立在休眠字段上（→ D21，本章主题的第二次命中）。
5. **#7 断言在真实夹具上是假的**：线性值超过 1 会被 `encodeSRGB` clamp ⇒ 两张图峰值都 255；
   且头灯几何 + 偶数 `slices` 时**顶点正对相机**，"峰值必然落在顶点之间"不成立（→ D19 + #7a/#7b/#7c）。
6. **#8 是范畴错误**：Mach band 是感知现象，二阶差分测的是一阶导不连续；
   逐像素的法线本身在网格上只是 C0，边界 kink 两张图都有（→ #8 重定义）。
7. **#10 的夹具不存在**：跟随相机的点光照亮整个可见半球，没有"只有方向光照到的点"（→ D20 纯函数）。
8. **#11 数学写错**：`Ns=0` 与 `keyIntensity=0` 在两灯 rig 下不等价（→ 改成 `Ns=0 ≡ Ks=0`）。
9. **#15 是惰性变异**：golden 的标签条是同长度惯例（17 字符），沿用旧矩形不会红；
   且第一版把 app 多行 HUD 与 golden 标签条混为一谈（→ D22 长标签条）。
10. **§7 与 D4 自相矛盾**（"`transformVS` 不改" vs "改 `worldPos` 那一行"）⇒ 已改为"范围 vs 内容"的说法。
11. **Q1 是致命缺口**：第一版没说 `Ks/Ns/Ka` 怎么进 shader，而 `Material` 默认全 0 ⇒
    lit golden 会烘进一张**零高光**的球，G-M6.2 与肉眼门一起静默失败（→ D15）。

**一般规律（进 notes）**：一份设计文档里"关于既有代码的陈述"和"我提的断言"是两类不同的待验证物。
前者靠 grep（本文已逐条核），后者靠**在真实夹具上算一遍**——第一版十条错里六条属于后者，
而它们全都"看起来像数学常识"。
