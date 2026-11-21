# golden 基准图

这些 BMP 是**回归基准**，不是展示图。它们被 `tests/golden/golden.cpp` 逐像素比对，
容差 ±1/255/通道（设计文档 C1′）。

| 文件 | 场景 | 生成于 |
|---|---|---|
| `m3_cube.bmp` | `sr::renderGoldenScene`：320×180、Backend A 单线程、位置编码顶点色的立方体 + HUD 文字 | `soft-raster --golden-write assets/golden/m3_cube.bmp` |
| `m4_textured_cube.bmp` | `sr::renderGoldenSceneTextured`：**同一个立方体、同一套姿态**，但走 `texturedFS` + 程序生成的 4×4 checker（128×128、三线性 + mip、ClampEdge）+ HUD 文字 | `soft-raster --golden-write-textured assets/golden/m4_textured_cube.bmp` |

两张图共用姿态常量（`golden_scene.h` 的 `kGoldenModelRotX/Y`、`kGoldenCamZ`、`kGoldenCubeSize`），
所以 M4 那张相对 M3 的差别只有"面上画的是什么"和标题里的一个字。
两个生成入口是**两个 flag**而不是一个带默认值的场景选择：默认值意味着"忘了指定场景"
会把 M4 的图画进 M3 的路径，而那个错误的症状是"重新生成之后一切正常"。

`m4_textured_cube.bmp` 里贴图是**正的**：每个面从外面看，红格在左上、绿格在右上、蓝格在左下。
这三格标出的是 uv 的原点与两个轴的方向（`src/render/texture.h` 的 `makeCheckerImage` 说明了
为什么必须是三个而不是零个 —— 纯棋盘在转置下恒等，用它当夹具就永远测不出 uv 接反）。

## 重新生成

```bash
cmake --build build --config Debug
./build/Debug/soft-raster.exe --golden-write assets/golden/m3_cube.bmp
./build/Debug/soft-raster.exe --golden-write-textured assets/golden/m4_textured_cube.bmp
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
