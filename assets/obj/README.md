# assets/obj 与它引用的贴图

本目录放 M5 的手写 OBJ/MTL 资产（`cube.obj`、`twomats.obj` 等由 M5.4 提交）。
它们引用的 `../tga/checker.tga` 是**程序生成**的贴图（设计 D14：committed 资产全部
由仓库内代码生成，不含外部资产许可问题）。这份 README 记录它的 provenance ——
与 `third_party/README.md` 的 sha256 纪律同一条理由：**"这张图是怎么来的"必须可机器复现，
而不是"我记得是这样"**。

## checker.tga 的 provenance

| 项 | 值 |
|---|---|
| 生成 flag | `soft-raster --emit-checker-tga <path>`（独立入口、无默认值，Ruling 38） |
| 生成函数 | `sr::makeCheckerImage(kGoldenCheckerCells=4, kGoldenCheckerCellPx=32, {})` → `sr::writeTga`（`src/loader/tga_writer.cpp`） |
| 生成提交 | M5.3 的 feat 提交（`feat(loader): …（M5.3）`，BASE `0045778`；精确 SHA 由紧随的 docs 提交补记） |
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

## 生成提交

- SHA：`（待补记）`
- 复核：`git log --follow -- assets/tga/checker.tga` 的第一条即生成提交。
