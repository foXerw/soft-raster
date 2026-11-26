# golden 基准图

这些 BMP 是**回归基准**，不是展示图。它们被 `tests/golden/golden.cpp` 逐像素比对，
容差 ±1/255/通道（设计文档 C1′）。

| 文件 | 场景 | 生成于 |
|---|---|---|
| `m3_cube.bmp` | `sr::renderGoldenScene`：320×180、Backend A 单线程、位置编码顶点色的立方体 + HUD 文字 | `soft-raster --golden-write assets/golden/m3_cube.bmp` |
| `m4_textured_cube.bmp` | `sr::renderGoldenSceneTextured`：**同一个立方体、同一套姿态**，但走 `texturedFS` + 程序生成的 4×4 checker（128×128、三线性 + mip、ClampEdge）+ HUD 文字 | `soft-raster --golden-write-textured assets/golden/m4_textured_cube.bmp` |
| `m6_lit_sphere.bmp` | `sr::renderGoldenSceneLit`：**同一套固定姿态**（D15，不用 fit），无贴图 UV 球（半径 1、12 stacks / **15 slices 奇数** ⇒ 无顶点正对眼）+ `blinnPhongPerPixelFS` + `litDefaultMaterialShade()` + D19 默认 rig（`goldenLitRig()` 单例）+ HUD 文字 `"GOLDEN M6 LIT 320X180"`（21 字符） | `soft-raster --golden-write-lit assets/golden/m6_lit_sphere.bmp` |

三张图共用姿态常量（`golden_scene.h` 的 `kGoldenModelRotX/Y`、`kGoldenCamZ`、`kGoldenCubeSize`；
lit 球沿用同一套固定姿态，D15 不用 fit），所以 M4 那张相对 M3 的差别只有"面上画的是什么"和标题里的一个字。
三个生成入口是**三个 flag**（`--golden-write` / `--golden-write-textured` / `--golden-write-lit`）
而不是一个带默认值的场景选择：默认值意味着"忘了指定场景"
会把 M4 的图画进 M3 的路径，而那个错误的症状是"重新生成之后一切正常"。

`m4_textured_cube.bmp` 里贴图是**正的**：每个面从外面看，红格在左上、绿格在右上、蓝格在左下。
这三格标出的是 uv 的原点与两个轴的方向（`src/render/texture.h` 的 `makeCheckerImage` 说明了
为什么必须是三个而不是零个 —— 纯棋盘在转置下恒等，用它当夹具就永远测不出 uv 接反）。

## 两种门禁：M4 逐字节，M3 ±1（Ruling 50）

ctest 对三张图都用 ±1/255/通道判，但**额外的要求不同**：

- **M4（`m4_textured_cube.bmp`）另要求 md5 逐字节可复现**。M5 的锚 3 把 OBJ 场景与它**零容差**比对，
  依赖的正是这个性质；M5.8 实测生成 vs 参考 md5 同为 `1f767a114a49d38c454448eec4465d51`。
- **M3（`m3_cube.bmp`）只由 ±1 门禁判，md5 不是它的门禁**。它最后写于 `3f8cc2a`（M3 期），
  之后 `src/render`/`src/scene` 又变了 12 个提交而参考图没重生成 ⇒ 今天的输出与它差 **2 个字节、各 ±1**
  （M5.8 实测，偏移 64925 与 91699），在 ±1 下绿，**不是缺陷**，与任何重构无关。
  拿 md5 当 M3 的门禁会误判自己的重构。**是否重生成 M3 参考图留给产品方**（验证纪律 #4 要求先肉眼验收），
  本里程碑不自行处理。

## m6_lit_sphere.bmp 的 provenance（M6.8）

- **生成于提交 `ad7ae06`**（feat(scene): lit golden …（M6.8），branch `feat/m6-lighting`），
  由 `sr::renderGoldenSceneLit` 经 `--golden-write-lit` 写出（Ruling 39：参考图与测试
  渲染的是**同一个生产函数**）。构建：soft-raster 0.0.1 Debug MSVC。
- **生成在产品方肉眼验收之后**：M6.6 Step 7 的人眼门于 2026-11-26 通过（"都没问题"），
  纪律 #4 不再阻塞本图。M3/M4 参考图**未重生成**，逐字节原样。
- sha256：`47fc4462a070a5d6b158a7b91c8734a7769b3c87a3908f31f1e472992d897054`。
  可复现性：同一构建连续生成两次逐字节相同（cmp 通过）。
- 门禁：ctest ±1/255/通道（MSVC-only，g++ 打印 `[skip]`，C1′）。**没有排除矩形**：
  本图与自己的参考图比对，两边含同一条标签条；M5 锚 3 的 `x<110,y<12` 属于
  "OBJ 场景 vs M4 参考"那条跨场景等式，抄到这里会静默忽略标签条上的真实差异
  （标签墨迹到 x=127 > 110）。
- 配套守卫（`tests/unit/test_golden_lit.cpp`）：确定性、**不饱和**（表面区最大通道
  <255 且高光像素 >0，D19 —— 坏重生成必红某条）、非 M3/M4 帧（拿错参考图不可能
  静默通过）、三条量化断言（#7b 半高宽/面积、#7c 峰位随眼、#8 边 kink）。
  地板级破坏性验证 #13（法线矩阵改 identity ⇒ 比对红 2985 像素）见 M6.8 报告。

## 重新生成

```bash
cmake --build build --config Debug
./build/Debug/soft-raster.exe --golden-write assets/golden/m3_cube.bmp
./build/Debug/soft-raster.exe --golden-write-textured assets/golden/m4_textured_cube.bmp
./build/Debug/soft-raster.exe --golden-write-lit assets/golden/m6_lit_sphere.bmp
```

然后**肉眼看一次**再提交：错位的 golden 会把之后每个里程碑都锁死。
重新生成必须在这里补一行说明原因（数值/算法变更？还是仅仅分辨率变了？）。

## 为什么参考图进仓库

- 它是二进制，`.gitattributes` 里 `*.bmp binary` 保证 Git 不对其做行尾转换。
  若被转换，golden 会整帧失败且症状看起来像"渲染回归"。
- 只在 MSVC 主路径判定。g++ 侧标准库的 `sin/cos/pow/log2` 与 MSVC 不保证逐位一致，
  而且 GCC 默认会把插值融合成 FMA —— 为跨编译器 bit-stability 自建数学库属于计划外的一大块。

## 已验证它真的在守门

把 `Viewport` 的 y 翻转去掉（`pos.y = (ndcY*0.5+0.5)*height`）之后，golden 报出大量差异并失败；
恢复后全绿。这条破坏性验证记在 `docs/notes/M3-深度与插值.md`。
