# soft-raster 设计文档

日期：2026-11-21
状态：已批准范围待定（v2，已并入 builder review 的修订）
作者：与 Claude Code 协作头脑风暴产出

**修订记录**
- v1：初稿，交独立 builder 审阅。
- v2（本文件）：并入产品方的 4 个裁决（帧率目标、贴图来源、git 流程、笔记交付物），
  并修掉审阅指出的 4 处内部矛盾（裁剪只做近平面 / y 翻转归属 / depth 归属 / 逐像素 fs 与 SIMD 互斥），
  同时把审阅自行拍板的细节收进文档使其显式。

---

## 1. 意图与动机

做一个**纯 CPU 的软件光栅化渲染器**，C++ 实现，运行在 Windows 的实时窗口里。
最终形态：导入一个 3D 模型（OBJ/MTL），在窗口中用鼠标转视角、滚轮推拉，实时看到
一个被光照、带纹理的模型，并显示 FPS 与渲染统计。

**为什么做**：这是学习项目。目标是把实时渲染管线的每一步亲手实现一遍并且真正理解——
顶点变换、齐次裁剪、透视投影与除法、视口映射、三角形 Setup、扫描/覆盖判定、属性插值、
深度测试、纹理采样与 mipmap、光照着色、以及"为什么 CPU 要按 tile 渲染才能并行"。

成功标准不是"代码能跑"，而是**每一阶段都能被可视化、被单独验证、被测量**。
做完时，应当能回答这些问题：一个像素的颜色是怎么从模型的一个三角形来的；
透视正确的插值为什么必须除以 w；tile 化为什么让多线程不需要锁；
以及**为什么逐像素的 CPU shader 是纯 CPU 实时渲染真正的天花板**。

## 2. 目标

- G1 手写完整经典管线：从 OBJ 顶点到屏幕像素，不依赖任何 GPU、任何 3D API、任何第三方渲染/数学/模型库。
- G2 **可编程阶段的骨架**（本次核心架构选择）：Vertex Shader / 裁剪 / 投影 / Primitive Setup /
  Rasterize inner loop / Fragment Shader / Output sink 各自是独立可替换的部分，
  而不是糊在一个 `drawTriangle()` 函数里。
- G3 **实时交互，量化验收线**（产品方已定）：
  - 分辨率 1280×720，场景为 **5–10 万三角形、模型占满屏幕大半**的自带验收模型，
    材质为 Blinn-Phong 逐像素 + 三线性纹理。
  - **Backend B（tile + 多线程）≥ 15 fps** 即为达标；Backend A（标量单线程）只作对照组，不承诺帧率。
  - 本机 12 逻辑核已确认，性能数据在该机采集。
- G4 画质可达"真实感"：Blinn-Phong 逐像素光照 + 纹理 + mipmap + gamma 正确的颜色处理。
- G5 阶段可视化与调试视图：线框、深度图、法线图、UV checker、tile 分箱图、overdraw 热图、
  逐阶段计时与计数统计。
- G6 CPU 性能工程作为明确里程碑：tile 分箱 + 多线程（无锁），以及 SIMD 化的 inner loop，
  两者都能与标量实现**同时存在、按键切换、跑出对比数据**。SIMD 范围见 §7.3 的裁决。
- G7 可回归验证：纯函数单元测试 + 固定场景固定相机的 golden image 比对（编译器范围见 C1′）。
- G8 **学习笔记是交付物**（产品方已定）：每个里程碑一篇 `docs/notes/Mn-*.md`，
  写"为什么"——数学推导、踩过的坑、实测数据、渲染出来的图。里程碑的验收条件包含这一篇。

## 3. 非目标

- N1 不做通用引擎：没有场景图、没有 ECS、没有编辑器 UI、没有材质资产系统、没有预制体概念。
- N2 不做 PBR、不做阴影（shadow map）、不做 GI/光追、不做延迟管线、不做 MSAA。这些在 §11 列为
  "毕业后可扩展方向"，不在本项目的实现范围内。
- N3 不做 glTF 解析（列为后期可选）。
- N4 不做跨平台：窗口/输入层只有 Win32。但核心库必须平台无关（见 C4），移植成本保持很低。
- N5 不引入包管理器；依赖以 vendoring 单头文件的形式进仓库（见 C2）。
- N6 不追求"最快 CPU 光栅器"。G3 把这条变成了可执行的界线：**为了达到 15 fps 而必须做的
  （tile、多线程、覆盖/插值 SIMD）在范围内；为了超过它而必须做的
  （shading 向量化、tile 特化 shader、纹理页缓存、手写汇编、极端微优化）不在范围内。**
  若 15 fps 达不到，处理方式是缩小验收模型并如实记录实测，不是把上述项目请回范围内。

## 4. 反目标（即使技术上跑起来，也算失败的情形）

- A1 抽象吞掉数学：如果 Shader/Pipeline 的接口层让人看不到 `edge function` 和
  `plane equation` 的实现，就是失败。抽象必须止步于"阶段边界"，阶段内部是直白的代码。
- A2 作弊依赖：为了显示画面或算矩阵去调 OpenGL/DirectX/Vulkan/glm/Eigen/assimp/Qt/SDL，
  等于取消项目本身。
- A3 数字对但没人知道为什么：没有推导注释、没有阶段可视化、没有可验证的中间结果，就是抄了一份管线。
- A4 变成"mini engine"：加了编辑器、粒子系统、后处理框架、脚本系统——与学习目标无关的发散。
- A5 只有终态没有阶梯：中途没有能看见画面的可运行版本，逼着一次性写完再调。

## 5. 约束

- C1 语言与工具链：C++20。主构建路径 CMake + Visual Studio 2022（MSVC cl；`cl` 不在普通 shell 的 PATH，
  走 VS 生成器或 `vcvars64.bat`）。本机已确认：CMake 4.4.0、VS Community 2022、MSYS2 g++ 16.1.0、无 ninja。
- **C1′ 数值可复现的范围（审阅问题 2 的裁决）**：
  - golden image 回归**只在 MSVC 主路径判定**；g++ 侧的要求是
    核心库/loader/测试**能配置、能编译、纯逻辑单测全绿**，不比对 golden BMP。
  - 理由：PATH 上的 g++ 是 MSYS2 自带（非 mingw-w64），libstdc++ 的 `sin/cos/pow/log2` 与 MSVC CRT
    不保证逐位一致；GCC 默认 `-ffp-contract=fast` 会把插值融合成 FMA 而 MSVC 不融合。
    为 bit-stability 自建三角函数/pow 属于计划外的一大块，且与学习目标无关。
  - 相应地：单元测试的数值断言一律写成**容差比较**（默认 1e-5 量级），文档里不再出现"精确等于"。
    浮点约定：GCC 构建加 `-ffp-contract=off` 以减少两边差异；三角函数/`pow`/`log2` 直接用标准库。
- C2 依赖：**渲染数学、光栅化、着色、模型解析全部手写，零第三方**。
  唯一允许的第三方文件是 `third_party/stb_image.h`（产品方已定），且受三条限制：
  只被 `src/loader/image_stb.*` 包含；只在 loader 里做像素解码；`soft_raster_core` 不得感知它。
  vendored 时在文件头记下版本号与许可证（MIT/public domain），保证离线构建（C3）。
- C3 构建期不联网：不使用 FetchContent / find_package 外部包。
- C4 核心库 (`soft_raster_core`) 内不得出现 `<windows.h>` 或任何 Win32 符号；
  framebuffer 只接收一个 `uint32_t*` 像素指针与宽高/步长。平台代码全部关在 `src/app/` 里。
- C5 热路径纪律：光栅化与着色内层不用异常、不逐像素做虚函数派发（虚调用按 draw call 一次，
  内层用函数指针或模板单态化）、不逐像素分配内存、不加锁。
- C6 单一可执行文件；Debug 与 Release 都可调试、可剖析；Release 不剥符号（保留内联后的可见性）。
- C7 交互必须支持"暂停"：任意时刻冻结当前帧，然后单步、切换调试视图，画面不闪。
- **C8 方向约定（消除审阅矛盾 3）**：`soft_raster_core` 的颜色与深度输出**一律 top-down**
  （第 0 行是图像上边缘）。Win32 DIB 的 bottom-up 翻转只发生在 `src/app/dib.*`
  （写 DIB 时按行号取反）。因此窗口显示与 golden BMP 的 y 方向必然一致，Viewport 阶段完全不知道 DIB 存在。

## 6. 方案对比（已选定）

**方案1：分层库 + 单一 app（经典 mini-engine）**
按 math/core/loader/app 分层，`core` 里一个 `Rasterizer` 类把 Setup、覆盖、插值、shading 写在一起。
取舍：起步最快、目录最清楚；但 shading 与光栅循环耦合，到性能里程碑要并行化时必须拆开重写。

**方案2：单文件渐进式（tinyrenderer 风格）**
一个 `main.cpp` 逐步长大，零脚手架。
取舍：学"每一行在干什么"最纯粹；但无法加多线程/SIMD，与目标 G6 直接冲突，
也不满足 G7 的测试目标与 G3 的实时要求。排除。

**方案3：可编程阶段管线（真 GPU 模型）**
显式阶段：VS → clip → 投影 → 视口 → Primitive Setup → Rasterize → FS → Output sink。
Shader 是可调用的接口；inner loop 是可替换后端；输出目标可替换（framebuffer / debug / stats）。
取舍：起步多一层接口设计；换来的是多线程（按 tile 分任务）、SIMD（换后端）、换光照（换 FS）、
阶段可视化（换 sink）都能直接插进去。

**选定：方案3 的形态 + 方案1 的目录结构。**（产品方选择"3+1混合"）
理由：目标里明确要"实时"，就必然走到 G6 的性能里程碑；只有把 Setup 与 inner loop 与 shading
三者分干净，性能工作才是"加一个后端"而不是"推翻重来"。目录仍按 math/core/loader/app 分层，
便于编译与测试隔离。

**inner loop 的实现轴（同时定下）**：先做**标量重心坐标 / edge function 逐像素**后端（Backend A），
直观、可验证；再在其后加**微 tile 化 + SIMD 覆盖判定**后端（Backend B）。
两个后端消费同一份"裁剪后屏幕空间三角形"数据结构，并存可切换，用于对比测量。

## 7. 架构（决定的部分）

### 7.1 数据流

```
Mesh: {pos, normal, uv}  (OBJ 索引化)
  │
  ├─ Uniforms { MVP, MV, 法线矩阵, 光源, 纹理, 材质 }
  ▼
[Vertex Shader]  用户函数，逐顶点：object → clip space (vec4)，产出 varying
  ▼
[Primitive Assembly]  每 3 个顶点组成三角形
  ▼
[Clip]  clip space 六面裁剪（-w ≤ x,y ≤ w，-w ≤ z ≤ w），Sutherland–Hodgman 多边形裁剪，
        裁剪后多边形以扇形重三角化（1 个凸多边形 → 最多 3 个三角形）
  ▼
[Perspective Divide]  xyz / w → NDC
  ▼
[Viewport]  NDC → 屏幕坐标 (x, y 像素，depth ∈ [0,1])。输出 top-down（C8）
  ▼
[Primitive Setup]  每三角形一次：edge function 系数、每个 varying 的 plane equation 梯度、
                   朝向判定与背面剔除、scissor/包围盒、面积（用于 overdraw/统计）
  ▼
[Rasterize 后端]  A: 标量逐像素；B: 微 tile + SIMD + 分箱，含多线程
  │   逐像素：coverage → depth test → [Fragment Shader] → 写 color target(+blend)
  ▼
[Output Sink]  RenderTarget(颜色+深度) | Debug | Stats   （同一接口，可替换/可组合）
```

**裁剪为什么做六面而不是只做近平面**（审阅问题 4 的裁决）：只裁近平面时，视锥 x/y 之外的三角形
仍会投影成极大坐标，Primitive Setup 的包围盒与 M8 的 tile bin range 会炸开，M9 若用定点 edge function
还有溢出风险，只能靠 scissor 硬夹。六面裁剪把这一整类问题在源头消掉，代码只比近平面版多约 40 行
（同一个裁剪循环参数化到 6 个平面），而且这正是 GPU 的做法，符合"学为什么 GPU 长这样"的目标。
副作用：`camera` 推进模型内部时不会看到面弹出。

关键约定（写进代码注释，属于学习内容本身）：
- 插值统一走 plane equation：给定三角形的 2D 投影，属性值 `f(x,y) = n·(x,y) + c`，
  采样时对重心权重 `β/γ` 做同样插值；透视正确性来自 `attr/w` 插值后再除以 `1/w` 插值。
- 深度使用 non-linear 的 `z/w` 结果（近高远密），不做 reverse-Z（保持与教科书一致）。
- 光照在**线性空间**计算，写颜色目标前做 sRGB 编码（gamma 2.2，查找表或 `pow`）。
- **光栅化规则（两后端必须一致，审阅自行拍板后收进文档）**：fill rule 采用 top-left 规则；
  深度比较用 `<=`，深度 clear 到 1.0；深度缓冲为 `float32`。

### 7.2 RenderTarget、tile 与分箱

**深度归谁（消除审阅矛盾 4）**：颜色与深度**不属于后端**，而属于一个显式对象：

```
RenderTarget { ColorTarget(uint32* + w/h/pitch) , DepthTarget(float* + w/h) }
```

由 app/render 层创建并作为参数传给光栅器。`DepthTarget` 支持**子矩形 view**：

- Backend A：整帧一个 view，单线程共享，逐三角形原地 depth test。
- Backend B：每个 tile 一个 `DepthTarget` 子矩形 view，指向**同一份全局 float 深度缓冲**。
  tile 之间不重叠 ⇒ 无竞争 ⇒ 无锁、无原子、也**无需回写**；
  `debug_depth` sink 直接读全局缓冲即可拿到整帧深度。
  这一条是 M8 的核心教学点：tile 化不只是 SIMD 的前提，也是并行安全的前提。

分箱：
- Framebuffer 划成 tile（默认 32×32 像素；8×8 微 tile 用于 SIMD 覆盖判定）。
- 几何阶段每个三角形求覆盖的 tile 范围（六面裁剪后范围有界，不会炸），
  把 triangle reference **追加进每个 tile 的 bin**。
- 渲染阶段：每个 tile 一个任务投给线程池。

### 7.3 Shader 接口与 SIMD 的边界（消除审阅矛盾 1 / 问题 1 的裁决）

接口形状**服从可读性与可验证性，不服从 SIMD 最大化**。理由：产品方定的 15–20 fps（G3）
不需要向量化 shading，而一旦为了 SIMD 把 FS 改成批量接口，M3–M6 的每一个 shader 都要写成
SoA 数组 + mask 语义，牺牲的正是本项目的主题。

- `VertexShader`：`VSOut vs(const VSIn&, const VSUniforms&)`，以函数指针形式存在 Pipeline 里（逐顶点调用，必须可内联）。
- `FragmentShader`：`bool fs(const FSIn&, FSOut&, const FSUniforms&)`，返回 false = discard。
  **逐像素、标量**；用模板或函数指针单态化，不用 `virtual` 逐像素派发（C5）。
- M9 的 SIMD 加速范围**明确为**：edge function / 覆盖判定、plane-equation 插值的前向差分、
  深度测试与比较掩码、颜色写入。**不包含** Blinn-Phong 本体、不包含纹理双线性/三线性采样。
- 因此 M9 的收益预期要**提前写低**：在覆盖与插值主导的场景（大量小三角形、高 overdraw）收益明显；
  在 shading 主导的场景（少量大三角形）收益有限，因为串行逐像素 `fs()` 调用就是瓶颈本身。
  实测数字与这个分析一起进 `docs/notes/M9`，**"为什么 GPU 必须把 shader 也 SIMT 化"作为本里程碑的学习产出**。
- 真要再提速，走的是关闭 SSAA / 降分辨率 / 换更便宜的 shader，而不是向量化 shader（N6 界线）。
- 内置 shader 集合：
  - `unlit`（顶点色/纹理色直出）
  - `lambert_pervertex`（Gouraud）——保留它是为了和 `blinn_phong_perpixel` 做视觉对比，观察高光带与 Mach band。
  - `blinn_phong_perpixel`（终态默认）
  - `debug_*` 系列（depth / normal / uv / overdraw / binning）实现为普通 FS，与正式 shader 走同一条路。

### 7.4 纹理与采样

- 纹理格式 RGBA8（内部），mipmap 链。
- 采样器：nearest → bilinear → trilinear(mip + bilinear)。mip 层级用 `log2` 从导数（`dP/dx`，
  `dP/dy`）推，用 `rasterize` 提供的偏导量（后端 A 用有限差分从 triangle plane 直接算，不引入 GPU derivative 指令概念）。
- Wrap：repeat / clamp。
- **贴图来源**（产品方已定，修正 v1 的自指失效）：
  - 程序生成的 checker/gradient/uv 调试图（永远可用，零 IO）。
  - `stb_image` 解码：PNG / JPG / BMP / TGA / PSD —— 于是网上下载的 OBJ + PNG 贴图可以直接用（C2 的三条限制仍生效）。
  - 同时保留一个手写 TGA 加载器（`image_tga.*`），因为它是理解"图像就是字节数组"的最低成本样本，也是 stb 缺席时的回退。
- **不**支持 PNG/JPG 的**编码导出**（截图仍为手写 BMP；想要 PNG 导出再单独提议引入 `stb_image_write`）。

### 7.5 模型与场景

- `Mesh`：SoA 的 `pos/normal/uv` + 索引；提供 index buffer 也提供 unrolled 回退路径（SoA 为默认，
  理由：varying 梯度与 tile binning 对 SoA 更友好，会写进 notes）。
- OBJ 解析支持子集：`v` `vn` `vt` `f`（三角/四边形/fan，负索引）、`o/g`、`usemtl`、`mtllib`、
  `Kd` `Ka` `Ks` `Ns` `d` `map_Kd`。缺 `vn` 时按面法线补。
- 一个 OBJ 内多个 material → 多个 draw call（按 material 分组，这是最小的 batching，不做更复杂的）。
- 场景是"一个模型 + 相机 + 光"。为了早期里程碑和测试，程序生成 cube / UV sphere / 圆环，
  并允许 `--obj path` 载入外部模型；仓库内 `assets/` 提交若干 OBJ（cube、sphere、验收模型）
  与对应 golden BMP。验收模型（G3 的 5–10 万三角形）由程序生成（细分球/圆环面），
  以保证 golden 与帧率数字都可复现、且不含外部资产许可问题。
  > 偏离见 [`2026-11-22-soft-raster-m5-obj-design.md`](2026-11-22-soft-raster-m5-obj-design.md) D14 续（M5 实施时定）：M5 只 committed `cube.obj`/`twomats.obj`/`checker.tga`；sphere 与验收模型推迟到 M8（committed 一个程序生成的 OBJ 需要先有导出器，那是 M5 的非目标）。
- **场景 fit 行为**（审阅问题 7 的裁决）：载入 OBJ 后自动按包围球 fit 相机距离，
  并推 `near = radius/1000`、`far = radius*10`，fov 默认 60°。这样单位是 mm 还是 m 的模型都能立刻看到；
  `--no-fit` 可关闭，`F` 键重新 fit。golden 与性能测量都用 fit 后的确定性相机。
  > 偏离见 [`2026-11-22-soft-raster-m5-obj-design.md`](2026-11-22-soft-raster-m5-obj-design.md) D15（M5 实施时定）：golden（锚 3）保留 M4 固定姿态，以便与既有 `m4_textured_cube.bmp` 零容差比对（fit 相机给出的是另一张毫不相干的图）；**性能测量仍用 fit 后的确定性相机**，那半句照旧执行。

### 7.6 窗口与显示（`src/app/`）

- 一个 Win32 窗口类，`CreateWindowEx` + 双缓冲：`CreateDIBSection` 建 32bpp BGRA DIB，
  渲染器直接写它的像素内存（top-down，按 C8 在 app 层翻行），`BitBlt` 到窗口 DC。
- 窗口尺寸**不可拖拽缩放**，只由 `--width/--height` 决定（避免每帧重分配 tile/深度数据）。
- 帧循环：`QueryPerformanceCounter` 计时，不强制 vsync（可自行限帧），
  FPS/统计以纯软件方式画在画面上（自带 8×8 位图字体，绘制函数十几行，不引入 GDI 字体 API——
  保持"像素全是我们算的"）。
- 输入：鼠标拖拽/滚轮 + 键盘。按键至少包含：
  线框、各 debug 视图循环、后端 A/B 切换、线程开/关与线程数、mip 开关、采样模式、
  着色模式（unlit/Gouraud/Phong）、SSAA 档位、暂停/单步、重新 fit、截图（BMP）、OBJ 路径重载。
  **M6 已落地的键**（M6.6，回写于 M6.9）：`3` 循环着色模式（Phong→Gouraud→Unlit）、
  `[`/`]` 点光强度加法步进 ±0.1 夹 [0,3]、`-`/`=` 方向光强度同形、`A` 切衰减、
  `0` 恢复**启动基线**（命令行意图，R99，非内置默认）；`V` 的 debug 视图循环扩到 4 个
  （color→depth→gray→normal，法线走 FS、深度/灰度走 sink，D23）。光参数另有 11 个命令行 flag
  （`--key-color/--key-dir/--key-intensity/--point-color/--point-pos/--point-intensity/--ambient/--ambient-color/--attenuate/--attenuate-radius/--shade`）。
- 命令行：`soft-raster [--obj path] [--width N] [--height N] [--backend a|b] [--threads N] [--bench N] [--no-fit]`。
  `--bench N`：离屏渲染 N 帧并打印每帧/分阶段耗时（性能里程碑的测量入口，也顺手满足"离线出图"）。
  **（M6.9 回写：分阶段耗时 msVertex/msClip/msSetup/msRaster 与 `--bench N` 真跑 N 帧在 M6.7 才落地
  （D21）；此前四个 ms* 字段声明了没人写、`--bench` 不设 frames，属"休眠接口"，见 M6 notes ①。）**

### 7.7 目录结构

```
soft-raster/
  CMakeLists.txt
  README.md                 # 构建、运行、按键表、里程碑进度与效果图
  third_party/
    stb_image.h             # 唯一第三方文件；头记版本与许可证（C2）
  docs/
    superpowers/specs/      # 本文件
    superpowers/plans/      # 实施计划
    notes/                  # 学习笔记：每个里程碑一篇"为什么"（G8，含推导/坑/实测/图）
  src/
    math/                   vec.h mat.h transform.h
    render/                 pipeline.h vertex.h fragment.h uniforms.h render_target.h
                            rasterizer_scalar.* rasterizer_tiled.* triangle_setup.*
                            clipping.h viewport.h sampler.h texture.h lighting.h
    loader/                 obj.* mtl.* image_stb.* image_tga.* bmp_writer.*
    scene/                  mesh_gen.* camera.*
    app/                    win32_window.* dib.* input.* overlay_font.* main.cpp
    bench/                  timing.* stats.*
  tests/                    harness.h(自建~100行) unit/ golden/ fixtures/
  assets/                   *.obj *.mtl *.tga *.png golden/*.bmp
```

- `soft_raster_core`（静态库：math+render+loader+scene，平台无关且不含 stb 感知）/
  `soft-raster`（app 可执行）/ `soft_raster_tests`（测试可执行，CTest 注册）。
- `tests` 只依赖 core；core 不依赖 app（C4）。

### 7.8 测试与验证策略

- 单元测试（无第三方框架，自建极简断言 + CTest；数值断言用容差，见 C1′）：
  - 数学：矩阵乘法/转置/逆、`perspective` 与 `lookAt` 的已知点映射结果。
  - 裁剪：完全在内 / 完全在外 / 跨每个平面的各种三角形，与"推进模型内部"的情形，
    验证输出顶点数、w 符号、以及重三角化后覆盖面积守恒。
  - 视口：NDC 角点 → 屏幕像素坐标（top-down 约定）。
  - 插值：三角形三顶点处取属性值应在容差内等于给定值；中心点与手工重心结果一致；
    透视正确性用一个"深度差异大的四边形"验证不发生线性插值的扭曲。
  - fill rule：跨边界像素在两后端下必须判定一致。
  - OBJ：对 fixture 文件比较顶点/面/材质解析结果（含负索引、quad、缺 vn）。
- Golden image（仅 MSVC 主路径判定，C1′）：固定场景 + 固定相机 + Backend A 单线程 + 关闭随机项，
  渲染一帧存 BMP，与 `assets/golden/*.bmp` 逐像素比较，容差 **±1/255/通道**。
  任何数值回归都会被抓到。golden 只在数值/算法里程碑重新生成，且重新生成必须在 notes 里说明原因。
- 后端一致性：同一场景下 Backend A 与 Backend B（含 1 线程、含 N 线程、含/不含 SIMD）
  的结果必须在 ±1/255/通道内一致——**"正确性不因优化而改变"是本项目的硬约束**，写成一个专门的测试。
- 性能测量：`--bench` 输出每帧各阶段耗时、三角形/秒、像素/秒、overdraw 比；
  每个后端 × 线程数 × SIMD 组合形成一张表，写进 `docs/notes/`。

## 8. 里程碑（阶段划分与每阶段的"可见成果"）

每个里程碑结束时必须有：能编译、能跑、能在窗口里看到画面变化、`docs/notes/Mn-*.md` 写完、被提交（并按 §10 推送）。
这是反目标 A5 与目标 G8 的直接约束。

| # | 内容 | 可见成果 |
|---|------|----------|
| M0 | 仓库与骨架：git init + remote、CMake/MSVC 工程、`.gitignore`、Win32 窗口 + DIB、位图字体、命令行解析 | 窗口里程序生成的渐变画面在动，显示 FPS；能截 BMP |
| M1 | math 库 + Backend A 的 edge function 逐像素光栅器 + 一个固定屏幕空间三角形 | 屏幕上出现一个纯色三角形，能改顶点看变化 |
| M2 | 完整几何阶段：VS / **六面裁剪** / 透视除法 / 视口 + 三角形装配 + 背面剔除；pipeline 骨架按 §7 落地，`depthTest` 标志位预留（当时恒通过） | 一个 MVP 变换的旋转立方体，面纯色，无深度（此时穿帮是好事，用它讲 painter's 的不足） |
| M3 | Z-buffer + 属性插值（plane equation）+ 顶点色；`RenderTarget` 落地 | 立方体平滑颜色、遮挡正确；depth debug 视图 |
| M4 | 透视正确 UV + 纹理采样（nearest→bilinear）+ mipmap + stb_image/TGA 加载 | 立方体贴 checker，旋转不扭曲；关掉 mip 会闪 → 打开不闪 |
| M5 | OBJ/MTL 导入 + 多 material draw call + 包围球自动 fit + 程序生成球体 | 载入任意外部 OBJ（含 PNG 贴图）立刻可见；缺 vn 的模型自动面法线 |
| M6 | 光照：Lambert → Gouraud vs Blinn-Phong 逐像素对比；线性空间 + sRGB 编码；光源参数 | 球体上有高光带，切换 shader 能看出 Gouraud 与 Phong 差别 |
| M7 | 轨道相机 + 交互打磨 + 阶段可视化（wireframe/normal/overdraw/tile bin 图）+ 阶段计时 HUD | **终态可演示版本**：导入模型、转视角、切视图、看统计 |
| M8 | 性能里程碑 1：tile 分箱 + **tile 局部 DepthTarget view** + 线程池（Backend B）；A/B 与线程数按键切换 | 同一场景下标量单线程 vs 多线程 tiled 的 FPS/耗时表；G3 的 15 fps 验收在此达成 |
| M9 | 性能里程碑 2：SIMD 覆盖判定 + 插值前向差分 + 深度比较掩码（SSE2 起步，AVX2 可选开关），与标量结果逐像素一致（§7.3 界定范围） | 三种后端组合的性能对比表 + 一篇"为什么 shading 才是瓶颈"的 notes |
| M10 | 收尾：**SSAA（超采样式，见下）**、golden 全量脚本化、notes 成文、README 补效果图 | 完整可交付仓库 |

依赖顺序：M0→M1→M2→M3→M4→M5→M6→M7 是功能主线，严格按序（后一阶段用前一阶段的产物）。
M8/M9 依赖 §7.2/§7.3 的接口边界，不依赖 M7 的交互打磨，可在 M7 前后调整，但必须在 M6 之后（需要有真实 shading 才有意义）。
M10 最后。

**SSAA 是什么（审阅问题 5 的裁决）**：取 **(a) 超采样**——整条管线在 2×/4× 内部分辨率跑，
最后 box filter 降到窗口分辨率。对现有设计几乎零改动（只是 RenderTarget 变大 + 一个 resolve sink），
代价是 4× 着色量，因此默认 2×，4× 作为可关选项，且 G3 的 15 fps 验收在关闭 SSAA 下测。
**不是** MSAA：不引入每像素多 coverage/depth sample，不改 §7.2 的 depth 布局（MSAA 属 N2/§11）。
SSAA 在 Backend A 与 B 下都可用（它只是把 RenderTarget 换大）。

## 9. 留给实施者的决定（builder's call）

以下不需要产品方回答，实现时按最合理方案定，并在 `docs/notes/` 记录理由：

- 具体类型名/函数签名、文件切分、tile 尺寸（默认 32×32，微 tile 8×8）、SSE2 还是 AVX2（默认 SSE2 保证跨机器，AVX2 作可选编译开关）。
- 线程池是自建固定大小工作队列（默认，约 150 行，属于学习目标之内）还是每帧起 `std::thread`。
- 测试 harness 自建（默认，约 100 行，见 C1/C3）。
- CMake 具体写法、警告等级、CTest 在 MSYS2 下 `msys-2.0.dll` 的 PATH 处理。
- 提交粒度的细节（每里程碑至少一次，里程碑内允许更细的 WIP 提交）。

**已经不再"留给实施者"的**（v1 里是开放项，现已由产品方或审阅定下）：
stb_image 引入（产品方定）、PNG 导入（产品方定）、笔记交付物（产品方定）、git 流程（产品方定）、
裁剪面数（裁决见 §7.1）、depth 归属（§7.2）、FS 接口与 SIMD 边界（§7.3）、SSAA 语义（§8）、
y 方向归属（C8）、g++ 侧验证范围与容差数值（C1′）、fill rule / 深度比较 / 窗口不可缩放（§7.1、§7.6）。

毕业后（或本项目之后）可扩展方向：shadow map、简单光追与管线混合、PBR + glTF、MSAA、
shading 的 SIMT 化、以及把 RenderBackend 做成真正的多后端插件层。

## 10. 已确认的决定与协作节奏（产品方 2026-11-21 批准）

1. **帧率与验收规模**：G3 定为 Backend B 多线程下 **≥15 fps** @1280×720、5–10 万三角形、
   Blinn-Phong + 三线性。性能工作以此为界（N6）。
2. **贴图**：引入 `stb_image`（唯一第三方，只在 loader，core 不感知）。任意外部 OBJ + PNG/JPG 可直接演示。
3. **git**：由我执行 `git init` + 添加 remote `https://github.com/foXerw/soft-raster.git`，
   每个里程碑一次提交并 push 到 `main`；`.gitignore` 排除 `build/`、截图输出、IDE 缓存。
   `assets/golden/` 与 `docs/notes/` 里的图 **提交进仓库**（它们是回归基准与学习成果）。
   push 前会先让你确认首次提交内容（不把 secrets 或大二进制推上去）。
4. **学习笔记**：每里程碑一篇 `docs/notes/Mn-*.md`，属验收条件。
5. **代码注释用中文**，标识符用英文（推导解释读起来更快）。
6. **C++20**（非 C++17）。
7. **主构建 MSVC**；g++ 保证核心库/loader/测试可编译且纯逻辑单测全绿（C1′）。
8. **默认窗口 1280×720**，不可拖拽缩放；性能数据在此分辨率采集。
9. **不引入 CI**，只提供本地脚本（golden 重新生成、bench 跑表）。
10. **计划粒度（我的决定）**：实施计划先给 **M0–M3 的完整任务分解** + M4–M10 的概要，
    每个里程碑结束时按计划复审节奏更新后续细节——因为 M8–M10 的细节必然被 M3 的实际数字影响。
