# M5 设计 — OBJ / MTL 导入，以及"两个错误互相抵消"

产品方 2026-11-22 说"你来决策"⇒ 本文里的 19 条 **D-nn 都是我（builder）的裁决**，
每条附理由与代价（D15–D18 是 fresh-context 评审回来后补的）。spec §7.5 与 §8 已经定死的东西（解析子集、fit 数值、多 material → 多 draw call、
程序生成验收模型以避免外部资产许可问题）**原样继承，不重开**。

权威上游：[`2026-11-21-soft-raster-design.md`](2026-11-21-soft-raster-design.md) §5（C1–C8）、
§7.4（纹理与采样）、§7.5（模型与场景）、§7.6（窗口与输入）、§7.7（目录与目标）、§8（里程碑）。

---

## 1. 这一章要什么

`--obj` 现在只打印一句"从 M5 开始生效"（`src/app/main.cpp:230`、`:246`）。M5 让它变成真的：
命令行给一个 OBJ 路径，窗口里**立刻看到这个模型**，不管它的单位是 mm 还是 m、有没有法线、
带几张 PNG 贴图、分几个材质。同时把"M5 之前不存在的三件事"落地：

1. **文件 IO 进入渲染路径**（此前所有几何与贴图都是程序生成的）。
2. **一次提交多个 draw call**（此前一帧只有一个 `backend.draw`）。
3. **第一次消费真实世界的 sRGB 像素**（M4 notes §11 预告的就是这件事）。

### 为什么这一章真正的主题是"抵消"

M5 有**三处独立的方向约定**要从文件格式翻译到我们的管线：

| 约定 | 文件里怎么说的 | 我们怎么表示 | 翻译发生在 |
|---|---|---|---|
| `vt` 的 v 轴 | v=0 是**图片下边缘**（OBJ 惯例） | v 正方向是**往下**（`texture.h:55`） | obj 解析器 |
| TGA 的行序 | header bit5 决定 origin 在上还是下 | 一律 top-down（C8） | tga 解析器 |
| PNG/JPG 的行序 | 总是 top-down | top-down | `image_stb.cpp:41`（显式关掉了 stb 的翻转） |

前两条**都是翻一次行**（`mesh.cpp:51-52` 里已经立着一条契约：那一次 `v = 1 - v` 由 M5 的载入器负责，
"格式的差异在格式的门口处理掉，不留进管线"）。于是出现本项目到目前为止最危险的一类假通过：
如果 obj 忘了翻转、tga 也忘了翻转，**画面看起来完全正确**，而三线性采样、mip 选择、
golden 比对全都不会报警——因为两个错误在合成路径上精确抵消了。
它和 M4 那一族"夹具在某个对称操作下不变"是同一家族（`docs/notes/M4-纹理与采样.md` §1），
只是这次的对称操作是"翻两次"。

**因此 M5 的验证设计不是"多写测试"，而是结构性的：每一次翻转必须各自对着一个非对称基准被钉住，
绝不允许只有合成路径的断言。**具体是三条独立的锚（§10），少任何一条都不算过。

## 2. 目标

- G-M5.1 载入一个外部 OBJ（含 MTL 与贴图）后**窗口里立刻正确显示**，包含被 fit 过的相机。
- G-M5.2 多 material → 多 draw call，且 draw call 数**可被断言**（新 `FrameStats::drawCalls`）。
- G-M5.3 解析器对"真实导出器会写的形状"不崩、不越界、不半加载；对畸形输入给出**指名到行**的错误。
- G-M5.4 三处方向约定各有独立的、有牙齿的守卫（§10）。
- G-M5.5 中文路径能载入（产品方是中文用户，"拖进来"撞上中文目录名的概率很高，
  而症状是"加载失败"——最难归因的一类）。

## 3. 非目标

- glTF / FBX / assimp / STL / PLY。OBJ 的一个子集，手写。
- 自动判断 Z-up 与 Y-up、自动单位换算。**反目标 A-式失效**：猜坐标系的症状是"模型躺倒了"，
  看起来像我们的变换写错了，会浪费一整天。宁可让它躺倒，由 `--no-fit` + 手动旋转解决。
- 透明度混合（`FragmentOutput` 至今没有 alpha 通道，终审 deferred #4 维持）。
  `d`/`Tr` 照解析照存，不用。
- `Ka`/`Ks`/`Ns` 参与着色（M6 的事）。M5 只让 `Kd` 与 `map_Kd` 影响画面。
- 平滑组 `s`、`vp`（参数化纹理坐标）、曲线曲面（`curv`/`tapr`/`hed`…）、`bevel`/`cset`。
  遇到就忽略并计数（见 D3），**不**假装实现了 `s on`。
- 大网格的 GPU 式优化（顶点属性压缩、索引重排、cache 友好的 face 排序）。M8 的 binning 才谈这些。
- OBJ **导出**。只读不写（TGA 写盘是资产生成工具，不是模型导出，见 §10.2）。

## 4. 反目标（跑起来也算失败）

- **A1 半加载**：解析到一半失败却返回一个"能画"的残缺 Model。症状是"模型缺一块"，
  而真正的错在第 4000 行的一条 `f`。⇒ 全有或全无（D6）。
- **A2 靠肉眼看贴图朝向对不对**：M4 已经用 Ruling 36 把这件事变成可断言的，M5 不许退回肉眼。
- **A3 只有合成路径的守卫**：见 §1，"两个翻转都错所以画面正确"是本项目最贵的失效模式。
- **A4 静默忽略未知指令**：`s off` 被忽略后，模型看起来是"平的"，用户会去怀疑我们的法线插值。
  忽略可以，**必须计数并打印**（D3）。
- **A5 里程碑结束时窗口里没有变化**（spec §8 的地板）。

## 5. 绑定本里程碑的既有约束

- **C2**：`stb_image.h` 只许被 `src/loader/image_stb.*` 碰到，`soft_raster_core` 不得感知它。
  M5 第一次让"一个函数同时需要文本解析与像素解码"，这条约束直接塑造了 §6 的分层。
- **C4**：core 内不得出现 `<windows.h>`。M5 要处理中文路径，但**不许**用 Win32 API 解决
  （用 `std::filesystem::path` + CRT `_wfopen`，见 D12）。
- **C5**：热路径不逐像素分配、不逐像素虚派发。`drawRange` 是**每个 draw call 一次**虚调用，
  与现状同阶，M8 的多线程不受影响。
- **C8**：core 的颜色与深度输出一律 top-down。M5 的三处行序翻译全部以它为目标。
- **G8/§10.4**：`docs/notes/M5-*.md` 是验收条件。

## 6. 方案对比（选定 A）

**A. 解析在 core、装配在 loader、场景持有在 app** ← 选定
`loader/obj.*` 与 `loader/mtl.*` 进 `soft_raster_core`（纯文本 → 纯数据，零 IO，零 stb 感知），
`loader/model.*`（把 OBJ + MTL + 贴图文件拼成一个可画的 Model）进 `soft_raster_loader`。
理由：**"一个 OBJ + 它的 MTL + 它的 PNG"必须在同一处合成，而那处需要 stb；
而 `tests` 链 core + loader、不链 app（`CMakeLists.txt:127`）**。
所以装配层落在 loader 是"端到端可测"与"C2 守得住"的唯一交集。
代价：两个 target、一次额外的头文件层次约定；`src/loader/` 这个目录跨两个 target
（**已有先例**：`bmp_writer.cpp` 在 `src/loader/` 却属于 core，`CMakeLists.txt:27` ⇒ 目录 ≠ target）。

**B. 全部塞进 `soft_raster_loader`**：少一层，但 obj/mtl 的解析单测从此必须链 loader，
而 loader 带着 stb 的 7 千行 —— 每次改一行解析器都要重编它。开发反馈变慢，且 C2 的
grep 守卫（`grep -rn stb_image src/` 只命中 `image_stb.*`）不再能一眼说明"core 是干净的"。排除。

**C. 不引入 sub-range，每个 material 造一个独立 `Mesh`**：改动最小，但 positions/normals/uvs
按材质复制一份，且**丢掉了"一个 index buffer + N 次提交"这个 M5 想教的东西**；
M8 的 binning 需要看到"整模型的索引域"才能分箱。排除。

**D. 装配放 app**：app 不被 `tests` 链接（`CMakeLists.txt:154-157`），端到端加载路径不可测。排除。

## 7. 数据模型

```cpp
// ---- core: loader/obj.h、loader/mtl.h ----
struct Material {                        // MTL 的一个条目，值语义
    LinearColor diffuse{0.8f, 0.8f, 0.8f};   // Kd，**已在解析时解码到线性空间**（D5）
    float opacity = 1.0f;                // d / Tr，M5 存而不用
    float shininess = 0.0f;              // Ns，M6 用
    LinearColor ambient{0, 0, 0};        // Ka，M5 存而不用
    LinearColor specular{0, 0, 0};       // Ks，M6 用
    std::string diffuseMap;              // map_Kd 的**文件名字符串**；core 不知道像素
};

struct MeshGroup {                       // 一段索引区间 + 用哪个材质
    uint32_t indexOffset = 0;            // **半开区间** [offset, offset+count)
    uint32_t indexCount  = 0;            // 恒为 3 的倍数（D4 保证）
    int32_t  material    = 0;            // 下标进 Model::materials
};

struct Model {
    Mesh mesh;                           // 全部几何**一份** buffer
    std::vector<Material> materials;
    std::vector<MeshGroup> groups;
};
bool parseMtl(std::string_view text, std::vector<Material>& out, std::vector<std::string>& warnings,
              std::string& err);          // **纯文本入参，不做 IO**
bool parseObj(std::string_view text, std::span<const Material> materials, Model& out,
              std::vector<std::string>& warnings, std::string& err);

// ---- loader: loader/model.h ----
struct LoadedModel {                     // move-only：贴图是几百 MB 级的东西
    Model model;
    std::vector<Texture> textures;       // materials[i].textureSlot 指进这里
    // ... 计划阶段定形：贴图下标放 Material 里还是并行的 vector
};
bool loadModel(const std::filesystem::path& objPath, LoadedModel& out, std::string& err);
```

**D1（裁决）**：`Mesh` **不**长材质字段。`Mesh` 是 `BackendA::draw` 消费的绘制单元，
把材质塞进去等于把两个层次焊死。分组信息全在 `Model::groups`。

**D2**：解析函数一律收**文本缓冲**，IO 在 `loadModel` 一处发生。
这条不是我发明的，是仓库既定约定 —— `image_tga.h:11` 写着"解析函数**不做 IO**
（与 M5 计划里 parseObj 同一条理由）"。好处：畸形输入（截断、BOM、CRLF、中文、越界索引）
全部可以内联在测试里当字符串写，不需要 fixture 文件，也不需要能删临时文件。

**D3**：未知的 `K*`/几何指令（`s`、`vp`、`bevel`、`cset`、未知材质属性）**忽略 + 计数 + 汇总进 warnings**。
`warnings` 由 `loadModel` 打印到 stdout（含文件名与行号），并在 HUD 第二行显示"UNRECOGNIZED: n"。
理由：反目标 A4；代价：一个字符串 vector 在加载路径上（非热路径，C5 不管这里）。
**已知指令携带未知选项 token 的情形不算"坏参数"**，见 D17（真实导出器每天都在写那种行）。

**D4（裁决，本节唯一的接口改动）**：`RenderBackend` 加虚函数 `drawRange`，`draw` 变成非虚包装：
```cpp
class RenderBackend {
  public:
    virtual void drawRange(const Mesh& mesh, const PipelineState& state,
                           uint32_t firstIndex, uint32_t indexCount) = 0;
    void draw(const Mesh& mesh, const PipelineState& state);   // = drawRange(…, 0, indices.size())
};
```
现有调用点**源码全部不动**：生产侧三处（`golden_scene.cpp:84`、`:118`、`main.cpp:146`）
+ 测试里约 25 处（`test_depth` / `test_pipeline_integration` / `test_texture_pipeline` /
`test_vertex_color`）—— `draw` 保住签名与语义，`BackendA` 是唯一的 `RenderBackend` 子类。
**M5.0 的"证明这次重构没改任何东西"的回归面是这约 28 处 + 两份 golden 逐字节不变**，不是三行。
`indexCount` 不是 3 的倍数 ⇒ 尾部不足 3 个索引被丢弃并计入 `FrameStats::trianglesDegenerate`
（不是断言，不是崩溃 —— 一个坏 group 不该带走整帧）。
`FrameStats` 加 `int64_t drawCalls`，在 `drawRange` 入口自增 ⇒ **G-M5.2 的"draw call 可断言"
靠它，不靠数三角形**（三角形数分不出"1 个 draw 交 12 面"与"2 个 draw 各交 6 面"）。
理由：C5 要求虚调用按 draw call 一次，这个形状正好；M8 的 Backend B 直接继承同一接口，
分箱也需要"索引域"这个概念。代价：多一个虚函数槽。

**D5**：`Kd` **在解析时**解码成线性空间，`map_Kd` **不在加载时解码** —— 这两件事不对称，
理由分别是：

- **贴图**：解码在**采样时**逐像素发生（`toTexelF`，`sampler.cpp:50-66`），由 `Texture::srgb`
  这个开关决定（`texture.h:23-26` 的既有区分，`docs/notes/M4-纹理与采样.md` §3 的标题就是
  "sRGB 解码发生在采样时，不是加载时"）。M5 要做的事只有一件：`makeTexture(img, /*colorDataIsSrgb=*/true)`
  —— 这个参数**已经存在**（`sampler.h:21`），M5 不改采样器、不在 `makeTexture` 里加任何解码。
  （**本条的前一版把这句话写反了**，说"解码发生在 loader 的 makeTexture 里"。评审抓出来后核对代码：
  `makeTexture` 在 **core** 的 `sampler.cpp:10`，且只做"设 srgb + 建 mip 链"。按那句错话去规划
  会得到**双重解码**——贴图整体压暗一档，正是 `color.h:7-10` 警告的那个症状——或者把 `makeTexture`
  搬进 loader，砸掉 core 侧的调用方 `golden_scene.cpp:58` 与整条 C2 分层。留这段是为了记住
  **"设计文档里关于既有代码的陈述也是待验证的断言"**。）
- **`Kd`**：它是文件里的一个**浮点数**，没有"采样时"可用，只能在解析时定身。
`Kd` 是 0–1 的浮点，**必须走一个浮点 sRGB→linear（`srgb8ToLinear` 只吃 uint8_t，
所以 `color.h` 需要一个 `srgbToLinear(float)`）**，否则 `Kd 0.5` 会被量化成 128/255 再解码，
白白引入一档误差。
理由：MTL 里的 `Kd` 与 PNG 像素是同一类东西（display-referred 的颜色值），
不一致地处理会让"有贴图的材质比没贴图的老是偏亮"。代价：与某些引擎（把 Kd 当线性）不同，
所以这条**必须**进 notes，并且 M6 换 shader 时要重新审视一次。
`Ka`/`Ks` 按 RGB 三元组解析并存进 Material（M6 要用），M5 不参与着色。

## 8. OBJ 解析规则

覆盖 spec §7.5 定的子集：`v` `vn` `vt` `f`（3/4/n-gon 扇形分解、负索引）、`o` `g` `usemtl` `mtllib`、
`#` 注释、UTF-8 BOM 剥离、CRLF 与裸 CR、tab/多空格分隔、空行。

**D6（裁决）全有或全无**：任何一个**结构性**错误（索引越界、`vn` 越界、`usemtl` 指向不存在的材质、
`f` 少於 3 个顶点、`mtllib` 文件读不到）⇒ `parseObj`/`loadModel` 返回 false、`err` 带**行号与原文片段**、
`out` 保持为空。**没有"跳过这一面继续"这种模式。**
理由：A1；代价：一个有小错的模型完全不可用，所以错误消息的质量就是可用性（也是 D3 要计数而非
崩掉的原因 —— 未知**指令**是警告，已知指令的**坏参数**是错误）。

**D7（裁决）顶点去重与"缺 vn 就 unroll"**：
OBJ 的顶点是 `(v, vt, vn)` **三元组**，同一个几何顶点在不同面可以配不同 uv。所以：
- 三元组齐全的面 ⇒ 按三元组去重（一个 64×32 球：2080 个唯一顶点 vs 4032 个面角，值得一个 map）。
- **某面缺 `vn` ⇒ 该面 unroll 成 3 个独占顶点，法线 = 面法线**（spec §7.5 的"缺 vn 按面法线补"）。
  共享顶点无法携带两个面法线，这是几何事实不是实现选择。unroll 的顶点数膨胀写进 notes。
- 缺 `vt` ⇒ 该面的 uv 填 `(0,0)`，**并且不翻转**（D9）；`texturedFS` 会画成一个纹素的纯色，
  这是"没有 uv"最诚实的可见症状。
代价：去重需要一个 hash map（键是三个 int64）。**只发生在加载期**，不在热路径。

**D8（裁决）n-gon 用扇形分解**（`f a b c d e` → `abc, acd, ade`）。
理由：这是 OBJ 规范写的 fan 语义，且凹多边形分解结果与面片法线方向一致（不会翻面）。
代价：非凸 n-gon 与三角化器（`b c d, b d e`）结果不同 —— 那是导出器的问题，notes 记一句。

**D9（裁决）`vt` 的翻转发生在解析器的最后一行**：`uvOut = { u, 1.0f - v }`。
不放 sampler（会把文件格式知识漏进采样器）、不放 mesh gen（会破坏 M4 golden 已钉死的约定：
`texture.h:55` "v 的正方向是往下"）。代价：读 OBJ 文件时看到的 v 与内存里的 v 是反的，
所以那一行必须有注释 + 有测试（§10 的锚 1）。

**D10（裁决）面方向（winding）**：OBJ 的 `f` 顶点按**从正面看逆时针**排列，
与我们 NDC 的 CCW 约定一致（`render_backend` 之后 top-down 屏幕上正面是 CW，`pipeline.h:72`）
⇒ **解析器不改顺序，也不翻转法线**。
风险与守卫：如果某导出器写成 CW，`backfaceCull` 开着的直接症状是**模型看起来是凹进去的 /
内表面**。M5.6 的验收里必须显式检查这一点，`W` 键（现在借来做背面演示，`main.cpp:282`）
就是它的诊断工具。不加自动翻转选项（N: 猜方向 = A3 那一族的另一支）。

## 9. 装配（`loadModel`）

```
读 obj 文件 → parseObj（无 mtllib 时：materials = {默认白材质}）
  → mtllib：路径**相对 OBJ 所在目录**解析 → 读 → parseMtl
  → 每个 map_Kd：路径相对 **MTL 所在目录**解析 → 去重 → loadImageFile → makeTexture（建 mip 链）
  → 尺寸检查 → fit 用的包围球已由 Mesh::recomputeBounds 给出
```

**D11（裁决）贴图去重的键是 `std::filesystem::weakly_canonical(mtl 所在目录 / 文件名)`，不是原始字符串。**
（本条的前一版写的是"精确匹配原始字符串，不做大小写折叠"，评审指出一个真实后果：
`checker.tga` 与 `./checker.tga` 指向同一张图却会各建一条 mip 链 —— canonical 化把这两种写法收拢。）
去重**不影响像素**（同一个文件 → 同一份 `Texture`），所以 Windows 的大小写不敏感在这里是良性的，
不违反 C1′（那条管的是**数值可复现**，不是"内存占用可复现"）。
理由：3 个材质共用 `brick.png` 时只建一条 mip 链（一张 2048² 的图连链约 21 MB，
复制三份会让人以为是渲染器在吃内存）。代价：`weakly_canonical` 会碰文件系统
（文件不存在时它也**不**报错，只是不做存在的解析）⇒ "贴图文件缺失"必须由
`loadImageFile` 的失败来报（D6），不能指望 canonical 化替我们检查存在性。

**D12（裁决）路径一律 `std::filesystem::path`，且非 ASCII 路径必须能载入（G-M5.5）**：
- OBJ/MTL 文本：`std::ifstream(path)`（C++17 起接受 `filesystem::path`，MSVC 内部走宽字符）。
  **注意本条只管到 loader 的门口**：从 shell 敲进来的 `--obj` 还有一半在 app 侧，见 D18。
- 贴图：`stbi_load` 吃 `const char*` = **ANSI 代码页**，中文路径当场失败。
  ⇒ `image_stb` 改为 `stbi_info_*` 探尺寸 + `stbi_load_from_file(FILE*)`，
  FILE 由 CRT `_wfopen` 打开（`#ifdef _MSC_VER` 走 `_wfopen(path, L"rb")`，否则退回 `fopen`）。
  **不引入 `<windows.h>`**（C4）。代价：`image_stb.cpp` 多约 15 行 + 一个 Windows 专属分支；
  换来的是中文目录不是一句"无法解码"。
- 测试：造一个带中文名的临时目录 + 复制贴图进去，断言加载成功。

**D13（裁决）贴图尺寸上限 `kMaxTextureSide = 4096`，且在分配之前问**。
`stbi_info` 只读头部就能拿到宽高 ⇒ 超限直接返回 false + 一句说清限制的错。
理由：这是终审 6f625a8 那条"TGA 先问再分配"（`image_tga.h:21-27`）的同一件事，
而 M5 第一次让**不可信输入**（网上下载的图）走进这条路径；一个 32768² 的 PNG
解成 RGBA8 是 4 GB，而它的症状只是"程序没了"。
为什么是 4096：软件光栅化 + 逐像素三线性在 4096² 上已经不是实时目标，更大没有学习价值。
代价：真实 8K 贴图集会被拒（错误消息里给出原因与常量名，所以可归因）。

## 10. 三次翻转，三条独立的锚

这是 M5 的核心。**每条锚只对一次翻转负责，任何一条都不能被"合成画面看起来对"替代。**

1. **锚 1（OBJ 的 v）**：`parseObj` 后的 `Model::mesh.uvs` **逐元素等于**
   `makeCube(1.6f, true).uvs`（`mesh.cpp:37` 的既有约定），容差 1e-6。
   `cube.obj` 的 `vt` 按 **OBJ 规范**手工书写（v=0 在图片下边缘）⇒ 忘了翻转必然变红。
   变异验证：删掉 D9 那一行 ⇒ 本条与锚 3 同时变红。
2. **锚 2（TGA 的行序）**：`loadTgaFile("checker.tga").rgba` **逐字节等于**
   `makeCheckerImage(4, 32, {}).rgba`。夹具必须是 Ruling 36 那张**三角不对称**的棋盘 ——
   纯棋盘在 y 翻转下要么逐字节不变（`cells` 为奇数：`(i + cells-1-j) ≡ i+j (mod 2)`），
   要么只是明暗整片互换（`cells` 为偶数），**两种都看不出翻没翻**。
   变异验证：把 writer 的 origin bit 与 parser 的处理同时改 ⇒ 本条**必须**变红。
3. **锚 3（合成的 golden）**：`assets/golden/m5_obj_cube.bmp` 与
   `assets/golden/m4_textured_cube.bmp` **逐字节相同**（只有 HUD 那行 "GOLDEN M5" 除外 ——
   沿用 M4 的做法：同长度、只差一个字，`golden_scene.cpp:120-124`）。
   这条是端到端的：几何、v 翻转、sRGB、winding、ClampEdge 采样状态、mip 链、`Kd` 白 tint
   全对才可能相同。
   **它的价值恰恰建立在锚 1、2 各自独立成立之上** —— 只有锚 3 的话，双翻转抵消会通过它。
   **锚 3 用 M4 那套固定姿态（`golden_scene.h:23-27`），不用 fit 后的相机** ——
   这一点与 spec §7.5 的"golden 与性能测量都用 fit 后的确定性相机"冲突，是**有意的偏离**，
   裁决与代价见 D15。

**D14（裁决）committed 资产只有三样，且全部由仓库内代码生成**（spec §7.5：不含外部资产许可问题）：

| 资产 | 来源 |
|---|---|
| `assets/tga/checker.tga` | 新 `--emit-checker-tga <path>`，写 `makeCheckerImage(4,32,{})` |
| `assets/obj/cube.obj` + `cube.mtl` | 手写，几何与 `makeCube(1.6f,true)` 一致，`map_Kd checker.tga` |
| `assets/obj/twomats.obj` + `.mtl` | 手写：两个盒子、**三个材质组**，其中两组各自 `usemtl` 但 `map_Kd` 指向**同一个** `checker.tga`（一组写 `checker.tga`、另一组写 `./checker.tga`）⇒ 一次守住 `drawCalls == 3` 与 `textures.size() == 1`（D11 的 canonical 去重），另一组守住"没贴图就画 `Kd` 纯色" |

`writeTga`（未压缩 32bpp，约 30 行，放 `src/loader/`）故意**写成 bottom-up（origin bit = 0）**：
这样"读回来"必然要走一次翻转，锚 2 才有内容；顺带 `--emit-checker-tga` 让资产的 provenance
是"由 `makeCheckerImage` 生成"这句话**可机器复现**，而不是"我记得是这样"。
（为什么不复用 `--golden-write` 那条 flag 加个默认值：Ruling 38 已经答过 ——
两个显式入口，没有默认值，就没有"把 A 的东西写进 B 的路径"这个坑。）

**D14 续（评审 Q2：spec §7.5 的资产清单是"cube、sphere、验收模型 + 对应 golden BMP"）**
它说得对，而且顺带暴露了一个比清单更实的问题：**`cube.obj` 测不到顶点去重**
—— 每面的 `vt` 都不同 ⇒ `(v,vt,vn)` 三元组在六个面之间**一次都不共享**，
D7 那个 map 走的全是"新建顶点"分支。裁决分三块：

1. **`sphere.obj` 与"验收模型"推迟到 M8**。理由：spec §7.5 同时说了验收模型**由程序生成**
   （`makeUVSphere` / `makeTorus` 早就在 `mesh.h:37-38`），而"committed 一个程序生成的 OBJ"
   需要先有一个 OBJ 导出器 —— 那是本文 §3 明令的非目标。真要在 git 里塞一个 5–10 万三角形
   的文本模型，仓库体积与 review diff 都会烂掉，而它守的东西 M8 才需要。**这条也是对 §7.5 的偏离，
   与 D15 同批进 notes 与账本。**
2. **曲面几何与去重的覆盖全部放进 M5.2 的内联 OBJ 文本夹具**（D2 让它们不需要文件）：
   一份手写"病理标本"文本，必须**同时**包含 ① 跨面共享的 `(v,vt,vn)` 三元组（断言唯一顶点数
   < 面角数，即 map 真的合并了）② 一个五边形 `f`（D8 扇形分解）③ 一个缺 `vn` 的面（D7 unroll，
   断言顶点数**膨胀**）④ 负索引 ⑤ 一条 `vt` 越界（配合 deferred #2 的新计数）
   ⑥ `mtllib` 指向不存在的文件（D6 报错带行号）⑦ 尾部不足 3 个索引的 group（D4 丢尾 + 计数）。
   **每一条都要过一轮变异验证**（本项目的第一条验证纪律），特别是 ①③ —— 它们是一对方向相反的
   断言，去重写坏会让一个变红，unroll 写坏会让另一个变红。
3. **一份曲面 OBJ 文件仍然值得有，但不是 sphere**：`twomats.obj` 的两个贴图组共用一张图，
   已经要过文件路径上的去重（D11）；曲面的文件路径覆盖**不加**，理由同 1（没有导出器）。

**为什么不做多 draw call 的第三份 golden**：`twomats.obj` 的守卫是
`drawCalls == 3` + `LoadedModel::textures.size() == 1` + `trianglesSubmitted` 与单 mesh 时相等
+ 两个盒子的 `Kd` 颜色各命中一个像素点
（用 Ruling 40 的"分类 + 距离差距"断言，不用逐字节），
再加**一张 `docs/notes/img/m5-*.png` 给人看**。理由：材质 tint 是纯数值，用断言比用参考图便宜，
而参考图每多一张就多一个"重新生成之后一切正常"的失效面。

## 11. app 侧

**scene 持有者**：新增 `src/app/scene.h/.cpp`（不收 Win32 头，与 `cmdline.*` 同一先例，
因此可以被测试直接链接）持有 `OrbitCamera` + `std::unique_ptr<LoadedModel>` +
`std::string statusError` + 程序生成的立方体兜底。`renderFrame` 里那个
`static sr::Mesh cube`（`main.cpp:109`）搬进去。
理由：finding —— 现在模型是 `renderFrame` 内的函数局部 static + 几个文件级 global
（`g_useMip`/`g_filter`/`g_showBackfaces`/`g_sink`），`R` 重载与多 draw call 会让它变成一堆 global。
（贴图借用：`FragmentUniforms::texture` 是裸指针（`pipeline.h:44`）—— 只要 `LoadedModel`
活到帧末、且**每帧不重建** `Texture` 对象，裸指针就仍然成立。`std::vector<Texture>`
一旦扩容就会让旧指针失效 ⇒ **`LoadedModel` 建好之后不得再 push textures**，
这条写成注释 + 写成 `loadModel` 的收尾断言。）

- `--obj <path>`（字段已存在）生效：载入失败 ⇒ **保留程序生成的立方体** + 打印 + HUD 显示错误，
  不是空屏。
- `R`：重读同一路径（README 承诺的"M5 落地"）。**失败时保留上一份模型**，错误行持续到下次成功。
- 鼠标拖放 `.obj` 进窗口（`DragAcceptFiles` + `WM_DROPFILES`）：换 `--obj` 的路径并载入。
  非 `.obj` 后缀 ⇒ 走同一条 HUD 错误行，**不**静默忽略。
  这是我加的（spec 只要求"OBJ 路径重载"），理由是"拖进来立刻可见"是 M5 唯一能自己演示自己的交互；
  代价：约 15 行不可单测的 Win32 胶水，所以路径字符串**必须**立刻转交 `loadModel`
  （可测的那一半全在 loader 里）。
- `F`：**只**重新 fit（distance / near / far / target），**不**动 yaw/pitch。
  理由：按 F 的人是因为模型太大或跑出画面，不是为了让视角被重置。
- HUD 第二行追加 `DC n`（draw call 数）与 `UNRECOGNIZED n`；载入失败时第二行整行给错误。
  理由：`main.cpp:325-326` 的那句注释是本项目的好品味 —— "该看到什么"必须 always 在画面上。

**fit 公式**（spec §7.5 定了 near/far/fov，没定 distance）：
`distance = boundRadius() / sin(min(fovX, fovY) / 2)`，`target = boundCenter()`，`model = identity`。
取两个 fov 里较小的那个 ⇒ 竖直或水平**任一边**都能装下（窗口是 1280×720 还是 400×900 都不裁）。
不动 `st.vu.model`：让"fit"只发生在相机侧，golden 的姿态常量（`golden_scene.h:23-27`）继续有效。

## 12. 顺手关掉的两条 deferred（因为它们被 M5 变"活"了）

终审的 deferred 列表（`.superpowers/sdd/2026-11-21-soft-raster-m4-textures/progress.md`）里
有两条在 M4 是理论的、到 M5 变成现实的，**在本里程碑内关掉**：

- **#8 harness 退出码 = 失败用例数，POSIX 截断到 0–255** ⇒ M5 会新增 30–45 条用例（总数破 300），
  一次大回归完全可能正好跨过 256。改成夹到 1。**排在 M5 第一个提交**（它保护后面所有提交的红/绿判读）。
- **#2 `sampler.cpp` 的 `texel()==nullptr` 静默给全 0** ⇒ M5 第一次让**外部文件的 uv 可以越界**
  （真实 OBJ 里 `vt` 大于 1 或小于 0 很常见），这条从"理论上的静默"变成" reachable 的静默"。
  加 debug-only 计数（照 `interpClampedCount` 的样子），并在 `--obj` 载入后打印。
- **#3 mip 链的 alpha 平均没有测试** ⇒ M5 的 TGA/真 PNG 第一次带非 255 的 alpha。补一条。
- **#4 `FragmentOutput` 没有 alpha**：**维持 deferred**，理由：混合需要颜色目标侧的 blend 状态，
  那是与 M6 的"线性空间"一起定的事，不该塞进一个 IO 里程碑。

## 13. 评审回来的四条补充裁决（D15–D18）

一个不带上下文的 builder 读了本文并核对了被引用的代码。它抓出四件原文**解决不了**的事，
其中 D15 与 D14 续是对上游 spec 的偏离，D17 与 D18 是"照原文写会当场失效"的漏。

**D15（本文唯一一处推翻产品方已定的东西）**
spec §7.5 定的是"golden **与**性能测量都用 fit 后的确定性相机"。锚 3 要的"逐字节等于
`m4_textured_cube.bmp`"与这句话**不可能同时成立**：同一个 1.6 立方体 fit 之后是
`distance = 0.8√3 / sin 30° ≈ 2.7713`、`model = identity`、`near ≈ 0.001386`、`far ≈ 13.856`，
而 M4 的锚定姿态是 `camZ = 5.0` + `rotX(0.43)·rotY(0.61)` + `near 0.1 / far 100`
（`golden_scene.h:23-27`）—— 两张图毫无关系。
**裁决：锚 3 保留 M4 姿态。** 理由：锚 3 的全部价值 = "与一张已被 M4 逐项验过的参考图
**零容差**相同"；换成 fit 相机，它立刻退化成一张**只能靠肉眼验收**的新参考图（= 反目标 A2），
而 fit 的正确性本来就有更强的守法（D16）。§7.5 那句话的真实意图是"性能数字必须可复现"，
**那半句（性能测量用 fit 相机）照旧执行**。
代价：spec 与实现之间多一条明文偏离 ⇒ 与 D14 续同批写进 `docs/notes/M5-*.md` 与 SDD 账本
（Rulings 从 **43** 起编号）。

**D16（fit 的守卫换成数值断言 —— 这里 golden 是更弱的守卫）**
fit 的承诺是"单位是 mm 还是 m 都能立刻看到"（G-M5.1）。一张单一姿态的参考图**结构上**证明不了
"任何尺度"，而两条断言可以：
1. `fitCamera(model)` 的 `distance/near/far/target` 与 §11 的公式**逐字段容差相等**（1e-5，沿 C1′）。
2. **包围盒 8 个角投影回 NDC，断言全部落在 `[-1,1]³`**，在三个尺度上各跑一遍：`0.001`、`1.6`
   （与 M4 同尺寸）、`1600`（同一个立方体放大 1000 倍）。这一条同时钉住三个数：
   `near` 太大 ⇒ 近角被裁，`far` 太小 ⇒ 远角被裁，`distance` 太小 ⇒ 上下或左右溢出。
3. 加上 M5.5 的**肉眼门**（验证纪律第 4 条）：真模型拖进窗口，产品方说"看见了、装得下"。
理由：断言"任何尺度都装得下"比断言"这一个尺度长这样"更接近需求；而且 fit 是 M5 新写的代码，
**没有历史可退化**，golden 在 M5 能给的只有"这次没变"，给不了"这次是对的"。

**D17（`map_Kd` 的选项 token：跳过取文件名，**不是**结构性错误）**
真实导出器天天写 `map_Kd -s 1 1 1 brick.png`（Blender / 3ds Max / Substance 都这样）。
原文只有 D6（已知指令的坏参数 ⇒ 整个加载失败）与 D3（未知指令 ⇒ 忽略并计数），
"**已知指令带未知选项**"两边都不属于：按 D6 处理会当场毙掉大量真实文件，直接违背
G-M5.1 与 G-M5.3；按 D3 处理则需要一条明文规则。裁决：
- `map_Kd` 之后按空白切 token；**开头的 `-选项` 连同其参数一起丢**。
- 未知 `-xxx` ⇒ 丢 1 个 token，**并把紧随其后的整个"数字串"一并丢**（一串，不是一个），计一条 warning（走 D3 那一路）。
- **D17 修订（M5.1 评审发现原表写错了）**：原文的 arity 表是我凭印象写的，与真实 MTL 规范有三处不符
  ——`-mm` 是 **2** 个参数（`-mm <multiplier> <add>`）不是 3、`-cc` 根本不是 MTL 的选项、真选项叫
  **`-imfchan`** 而不是 `-imf`。表里写错的代价不是"少吞"而是**多吞**：`map_Kd -mm 0 1 brick.png`
  会把 `brick.png` 当成第三个参数吃掉，剩下空串 ⇒ 按 D6 判"缺文件名" ⇒ **整个文件加载失败**。
  裁定：**只保留高置信度的条目**，其余一律走"未知选项"那一路（修订后的未知规则能正确吞掉它们）：

  | 选项 | 参数个数 | 含义 |
  |---|---|---|
  | `-s` | 3 | 缩放 `sizeX sizeY sizeZ` |
  | `-o` | 3 | 平移 `offsetX offsetY offsetZ` |
  | `-t` | 3 | 扰动/平铺 `tr tg tb` |
  | `-bf` | 2 | blend mask 系数 |
  | `-mm` | 2 | `multiplier add` |
  | `-bm` | 1 | bump multiplier |
  | `-texres` | 1 | 纹理分辨率 |
  | `-imfchan` | 1 | 通道选择 `r/g/b/a/l/z`（**非数字**，所以必须靠表，未知分支的数字串规则接不住它） |
  | `-clamp` / `-cl` | 1 | `on/off`（同上，非数字） |

  **这张表只用来决定"少吞还是多吞"，不是白名单**；表里没有的 `-xxx` 一律按未知选项处理。
  表的正确性靠两件事守：条目少而确定，加上"未知 ⇒ 丢 token + 一串数字"的兜底规则。
  原表里我编的 `-cc -fov -imf -pr -sa -tile_res -blend -boost -bobst -bump` **全部删掉** ——
  一个错条目比少一个条目危险得多，因为它会安静地吃掉文件名。
- 已知限制（写明，不追）：**以数字或 `-` 开头的文件名**（`4k.png`、`-tex.png`）在选项之后会被误吞。
  代价是这类文件加载失败并报"缺文件名"，而真实导出器不会产生这种名字；若要支持，得让 `parseMtl`
  能问文件系统（违反 D2"解析不做 IO"），不值。
  **同一条限制覆盖带符号与前导点的数字**（`+0.5`、`.5`）：它们不被判为数字，会**粘进文件名**。
  与数字开头的文件名不同，那种情形**不报"缺文件名"而是静默给出一个错文件名** ⇒ 症状变成"贴图找不到"。
  实测三类都安全或不 corrupt：表里的选项按**个数**吞参（不看内容），所以 `-mm -0.5 1 tex.png` 正确；
  未知选项后面跟 `-0.5` 被当成又一个伪选项丢掉并多一条警告；`1e-3` 按数字正常吞。
  复审的结论是"记录即可，不改代码"——真实的 Blender/3ds Max/Substance 只写普通或带负号的十进制。
- **第一个不以 `-` 开头的 token 起，剩余部分用单空格重新拼接 = 文件名**（保住带空格的贴图名）。
- 拼出来为空 ⇒ 这才是 D6 意义上的坏参数，报错并带行号。
代价：`map_Kd` 的解析从"取最后一个字段"变成一个小状态机（约 30 行 + 4 条内联单测：
带 arity 表的、未知选项的、文件名含空格的、空的）。
好处：`bump` / `disp` / 将来的 `map_Ks` 直接复用同一形状。

**D18（`--obj` 的中文路径：argv 那一半也得修，否则 G-M5.5 是假的）**
D12 只守到 loader 门口。`--obj` 走 **narrow argv**，而产品方实际用的 shell 是 Git Bash / MSYS2
—— 它传 **UTF-8** 字节，MSVC 的 `path(std::string)` 按 **ANSI 代码页（本机 936）** 解释：
`--obj D:\模型\cube.obj` 会 mojibake 然后失败，**而 D12 的中文目录单测照样全绿**
（那个测试直接调 `loadModel(path)`，绕过了 argv）。`main.cpp:379` 现在把这件事写成
"由调用方决定"，等于没决定。裁决：
1. `main.cpp` 开头用 `GetCommandLineW()` + `CommandLineToArgvW()` 取 **UTF-16 argv**，转成
   **UTF-8 narrow argv** 再喂给现有的 `parseCmdline` ⇒ `cmdline.cpp` 与它的测试**一行不改**
   （它们喂的一直是 ASCII）。
2. `AppOptions::obj` 的语义明文定为 **UTF-8**（注释写死），它同时是 HUD 上要显示的那串。
3. 新增 `app/path.h`：`std::filesystem::path nativePath(const std::string& utf8)`，内部
   `MultiByteToWideChar(CP_UTF8, ...)`，约 8 行。**全仓库只有这一处做这个转换**，
   `WM_DROPFILES` 拿到的宽字符路径也走同一个入口。
4. `main.cpp:379` 那句"由调用方决定"删掉，换成"argv 已在 main 开头统一成 UTF-8"。
分层干净：转换在 **app**（C4 只绑 core，`user32` 已链，`CMakeLists.txt:164`），
loader 收 `std::filesystem::path`（D12），两边都不脏。
代价：`nativePath` 是 Win32 的，**不可单测** ⇒ 它必须留在 8 行内且只做一件事，
而可测的两半各有测试（loader 的中文目录、cmdline 的 ASCII）。argv 那一半由 M5.6 的肉眼门兜底：
**产品方亲手在 Git Bash 里跑一次带中文路径的 `--obj`**，成功才算这条关掉。

## 14. 留给计划/实施者的决定

- 具体文件切分、`LoadedModel` 里贴图下标的存放形状、hash map 的键写法、warnings 的字符串格式。
- `parseObj` 的行迭代用 `std::string_view` 手工 split 还是 `istream`（前者更容易给出行号）。
- M5 是否需要第四份 golden：**默认不需要**（§10 的理由）。若产品方要求"外部贴图也要有参考图守"，
  加一份，但它守的是锚 3 已经守住的东西。
- 非 2 的幂尺寸贴图：现有 mip 链已允许（`downsampleBox2` 每维不低于 1），**不加**额外要求。
- 提交粒度与账本：沿用上两个里程碑的做法（`.superpowers/sdd/<plan>/progress.md`，
  Rulings 从 **43** 起编号；**`.superpowers/` 不在 git 里，不许删**）。

## 15. 规模与切分

比 M4 略大：**8 个任务、30–45 条新用例、一次三方交叉锚点、一篇 notes**，
外加两条对上游 spec 的明文偏离（D15、D14 续）。
建议切分（计划可调整顺序，但 **M5.0 必须排第一**）：

| # | 任务 | 可见成果 |
|---|---|---|
| M5.0 | `tests/main.cpp:20` 退出码夹到 1（deferred #8）；`FrameStats::drawCalls` + `drawRange`（`draw` 转非虚包装） | 三侧全绿、约 28 处调用点源码不动、两份 golden 逐字节不变（**这一步的任务就是"证明重构没改任何东西"**） |
| M5.1 | `loader/mtl.*`：文本 → `Material`；`Kd/Ka/Ks` 浮点 sRGB 解码（`color.h` 加 `srgbToLinear(float)`）；`d/Ns` 存而不用；未知属性计数（D3）；`map_Kd` 选项状态机（D17） | 纯单测 |
| M5.2 | `loader/obj.*`：文本 → `Model`，D6–D10 + **D14 续第 2 条的七项病理标本**（含锚 1） | 纯单测（内联字符串夹具，无 IO）；每项过变异验证 |
| M5.3 | `writeTga`（故意 bottom-up）+ `--emit-checker-tga` + `checker.tga`（锚 2）+ `image_stb` 的 `stbi_info` 先问后分配（D13）+ loader 侧中文目录（D12） | 锚 1、锚 2 **各自**变红过（不是只红合成那一条） |
| M5.4 | `loader/model.*`：`loadModel` 装配、`mtllib`/贴图路径解析、canonical 去重（D11）、mip 链、deferred #3 的 alpha mip 测试、deferred #2 的 uv 越界计数 | 单测：内存里造的 OBJ + 真文件两样 |
| M5.5 | fit：`fitCamera` + 包围盒 8 角 NDC 三尺度断言（**D16**） | 数值断言，无 golden |
| M5.6 | app：`scene.*` 持有者、`--obj`、`R`、拖放、`F`、`--no-fit`、HUD 的 `DC n`/`UNRECOGNIZED n`/错误行、**UTF-8 argv 桥 + `app/path.h`（D18）** | **窗口里载入真模型** + 肉眼门（见下） |
| M5.7 | `cube.obj` + `twomats.obj`（三组两贴图）+ `--golden-write-obj`（锚 3）+ `drawCalls == 3` 与 `textures.size() == 1` 断言 | **两份参考图逐字节相同** |
| M5.8 | `docs/notes/M5-*.md`（含 D15 / D14 续两条偏离的说明）+ README + 三侧全量验证 + push | 交付 |

**M5.6 有个硬门（产品方 2026-11-21 定的验证纪律第 4 条，也是 4.7 的约定 2）**：
生成任何 golden（锚 3）之前，产品方必须**亲眼在窗口里看过**载入的模型并说"看到了、朝向对"，
并且**亲手在 Git Bash 里用带中文的路径跑一次 `--obj`**（D18 的 argv 那一半只有这里能验）。

**留给我们这一对的一个待办**：D15 与 D14 续是对 `docs/superpowers/specs/2026-11-21-soft-raster-design.md`
§7.5 的**明文偏离**（"golden 用 fit 后的确定性相机"、"assets 提交 cube/sphere/验收模型与对应 golden BMP"）。
实施完成后应在上游 spec 的对应段落加一行指向本文的脚注，否则下一个读 spec 的人
会以为 M5 没照 §7.5 做 —— 而不是照 D15 改了 §7.5。
