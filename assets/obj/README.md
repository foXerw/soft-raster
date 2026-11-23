# assets/obj 与它引用的贴图

本目录放 M5 的手写 OBJ/MTL 资产（`twomats.obj` 由 M5.4 提交，`cube.obj` 由 M5.7 提交）。
它们引用的 `../tga/checker.tga` 是**程序生成**的贴图（设计 D14：committed 资产全部
由仓库内代码生成，不含外部资产许可问题）。这份 README 记录它的 provenance ——
与 `third_party/README.md` 的 sha256 纪律同一条理由：**"这张图是怎么来的"必须可机器复现，
而不是"我记得是这样"**。

## checker.tga 的 provenance

| 项 | 值 |
|---|---|
| 生成 flag | `soft-raster --emit-checker-tga <path>`（独立入口、无默认值，Ruling 38） |
| 生成函数 | `sr::makeCheckerImage(kGoldenCheckerCells=4, kGoldenCheckerCellPx=32, {})` → `sr::writeTga`（`src/loader/tga_writer.cpp`） |
| 生成提交 | `8b3c11a`（`feat(loader): …（M5.3）`，BASE `0045778`） |
| 格式 | 未压缩 type 2、32bpp、18 字节头、**bottom-up（descriptor bit5 = 0）**、alpha 第 4 字节 |
| 字节数 | 65,554（18 + 128×128×4） |
| SHA-256 | `fba10df24aed389d6022de2843a66f26768d146633953ea4ea66eec28166b2de` |

一条命令复现（逐字节）：

```bash
cmake --build build --config Debug
./build/Debug/soft-raster.exe --emit-checker-tga assets/tga/checker.tga
sha256sum assets/tga/checker.tga   # 应等于上表那串
```

复核锚 2：`soft_raster_tests --only anchor2` ——
`anchor2_the_committed_tga_equals_the_program_checker` 断言读回的 rgba 逐字节等于
`makeCheckerImage` 的内存字节，尺寸 128×128、mip 链 8 级。

### 为什么是 bottom-up

**故意的**（设计 §10 锚 2）：读回一个 bit5=0 的文件必然要走一次行翻转，
"TGA 行序翻译"那条路径才有东西可守。写成 top-down + bit5=1 的话读回不翻转，
锚 2 就退化成一次无翻转的 memcpy。bottom-up 也是 TGA 的历史默认，文件更"真实"。

### 为什么夹具是三角标记色而不是纯棋盘

纯棋盘在竖直翻转下要么逐字节不变（奇数 cells）、要么只是明暗整片互换（偶数 cells），
**两种都看不出翻没翻**。三个角各一个不同标记色（origin 红 / uAxis 绿 / vAxis 蓝）、
第四角留素 ⇒ 任何对称都不保持这张图，行序与通道序的错误在字节级可见。
肉眼验收：红在左上、绿在右上、蓝在左下（top-down 视角）。

### 纪律：锚 2 变红时不许重新生成这张图

往返测试（write→read→compare）对**成对反转**是盲的：writer 与 reader 共享同一个
错误约定（行序反了且标反、或 B/R 双向互换）时往返全绿。锚 2 对着的是**独立 oracle**：
内存里的 `makeCheckerImage` + 这张由正确代码生成后冻结在 git 里的资产。
用 `--emit-checker-tga` 重生成来"修"红掉的锚 2，等于把 oracle 换成被告自己的证词。
重新生成参考资产需要产品方肉眼验收（验证纪律 #4），不是测试红了就能做的事。

## twomats.obj / twomats.mtl 的 provenance（M5.4）

**手写**资产（作者用一段一次性脚本算好顶点/索引再落盘，脚本不入库；几何沿用
`makeCube` 的面序与绕向约定）。它钉住三件事：

| 项 | 值 / 守什么 |
|---|---|
| 几何 | 两个轴对齐盒子（中心 ±1.2，半边 0.5），16 个 `v`、6 个 `vn`、盒 B 24 个 `vt` |
| 分组 | **三个材质组**：盒 A 全 `plain`；盒 B 前三面 `texA`、后三面 `texB` ⇒ `drawCalls == 3`（`draw_model_uses_one_draw_call_per_group`） |
| 贴图去重 | `texA`/`texB` 的 `map_Kd` 都写 `../tga/checker.tga`（**相对 MTL 目录**，Ruling 47）⇒ canonical 去重后 `textures.size() == 1`（D11） |
| 纯色兜底 | `plain` 无 `map_Kd`、`Kd 1 0 0` ⇒ 守 Ruling 32"无贴图退回 Kd 纯色不是黑" |

- `twomats.mtl` 复用 M5.3 的 `assets/tga/checker.tga`，**零新增图像资产**（Ruling 47）。
- 行尾 LF（`.gitattributes` 已声明 `*.obj`/`*.mtl text eol=lf`）；提交后复核
  `git show HEAD:assets/obj/twomats.mtl | od -c | head` 无 CR。
- 它**不是** golden 锚（锚 1/3 用 M5.7 的 `cube.obj`）；朝向/绕向的肉眼验收归 M5.6。

## cube.obj / cube.mtl 的 provenance（M5.7，锚 1 资产版 + 锚 3）

**手写**资产（作者按 `mesh.cpp` 的约定推导后落盘，无脚本、无导出器）。
它是 M5 验证架构（设计 §1/§10）的落地点：三条方向约定各有一个独立的锚。

| 项 | 值 / 守什么 |
|---|---|
| 几何 | 8 个 `v`（±0.8 = `makeCube(1.6f)` 的半边）、6 个 `vn`（轴法线）、24 个 `vt`（每面 4 个）、6 个四边形 `f v/vt/vn` |
| vt 惯例 | **按 OBJ 惯例写：v=0 是贴图的下边缘** —— 这句话是锚 1 有牙齿的前提。解析器出口的 `v = 1 - v`（D9）把它翻译成 mesh 契约（v=0 = 面的顶边，`mesh.cpp:49-52`） |
| 面序 | `+X,-X,+Y,-Y,+Z,-Z`（`mesh.cpp:15` 的既有约定）；每面角序与 `makeCube` 逐面一致 ⇒ D8 扇形 `(a,b,c),(a,c,d)` 与 `appendQuad` 同一条对角线 a–c |
| 材质 | 一个 `checker`，**`Kd 1 1 1`**（纯白：`srgbToLinear(1.0f)==1.0f` 精确 ⇒ tint 可证中性）+ `map_Kd ../tga/checker.tga`（相对 MTL 目录，Ruling 47；复用 M5.3 资产，零新增图像） |
| 解码路径 | `.tga` 后缀 ⇒ `loadModel` 路由到**我们自己的** `loadTgaFile`（Ruling 63）—— 锚 2 守的解析器正是这张 golden 经过的那条 |

- **锚 1 资产版**：`anchor1_cube_obj_parses_to_the_same_triangles_as_makeCube`
  （`tests/unit/test_obj_golden.cpp`）—— 解析结果与 `makeCube(1.6f,true)` 按规范化三角形
  （3 种旋转取字典序最小、排序、逐元素 1e-5f）比对：吸收起点选择，**不吸收绕向/镜像**。
  `positions.size()==24` 是立方体的固有形状（每面 vt/vn 不同 ⇒ 跨面不共享），不是去重失效。
- **锚 3**：`anchor3_obj_golden_is_the_m4_golden_except_the_hud_strip` —— OBJ 场景进程内渲染，
  与既有 `assets/golden/m4_textured_cube.bmp` **零容差**逐像素比对，只跳过 HUD 矩形
  （x<110 && y<12）。**不新增参考图**（收紧 2）：少一张参考图就少一个"重新生成后一切正常"
  的失败面。姿态用 M4 固定 pose（Ruling D15），HUD 写 `GOLDEN M5 320X180`（与 M4 同长度差一个字）。
- 人眼查看入口：`soft-raster --obj assets/obj/cube.obj --golden-write-obj <path.bmp>`
  （独立 flag、必须配 `--obj`，Ruling 38）；notes 图在 `docs/notes/img/m5-obj-cube.bmp`。
- 行尾 LF（`.gitattributes` 的 `*.obj`/`*.mtl text eol=lf`）；提交后复核无 CR。
- **纪律**：锚 1/3 变红时先修文件或查翻转，**不许**改解析器来就文件（M5.2 已钉死解析器）、
  不许加宽容差/加宽 HUD 矩形、不许重生成 M4 参考图。
- **变异③实测（M5.7 Step 7）**：双翻转同时做错时，抵消在 level 0 上逐字节精确
  （关 mip 的两帧 md5 相同），但 mip 路径的浮点舍入留下 **3 个 ±1 像素** —— 零容差的锚 3
  仍红，±1 容差则会放过抵消帧（锚 1/2 各自红）。设计 §1 的"抵消后画面完全正确"按此修订：
  肉眼正确、±1 容差下正确，**逐字节不正确**。这不是放松合成检查的许可，是零容差的又一条理由。

## 生成提交

- SHA：`8b3c11a`（feat/m5-obj 分支；资产与生成它的代码同提交入库）
- 复核：`git log --follow -- assets/tga/checker.tga` 的第一条即生成提交。
