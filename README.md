# soft-raster

纯 CPU 软件光栅化渲染器。C++20，零第三方渲染依赖，运行在 Windows 实时窗口里。

这是一个**学习项目**：目标不是做一个引擎，而是把实时渲染管线的每一步亲手实现一遍并且真正理解——
顶点变换、齐次裁剪、透视投影、视口映射、三角形 Setup、覆盖判定、属性插值、深度测试、纹理采样、
光照着色，以及"为什么 CPU 要按 tile 渲染才能并行"。

- 设计文档：[`docs/superpowers/specs/2026-11-21-soft-raster-design.md`](docs/superpowers/specs/2026-11-21-soft-raster-design.md)
- 实施计划：[`docs/superpowers/plans/2026-11-21-soft-raster-m0-m3.md`](docs/superpowers/plans/2026-11-21-soft-raster-m0-m3.md)
- 学习笔记：[`docs/notes/`](docs/notes/)（每个里程碑一篇"为什么"）

## 构建

```bash
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

主构建路径是 CMake + Visual Studio 2022。核心库与测试也能用 MSYS2 g++ 配置编译：

```bash
cmake -S . -B build-gcc -DSR_BUILD_APP=OFF -DCMAKE_BUILD_TYPE=Debug
cmake --build build-gcc && ctest --test-dir build-gcc --output-on-failure
```

## 运行

```bash
./build/Debug/soft-raster.exe                       # 1280x720 窗口
./build/Debug/soft-raster.exe --width 640 --height 360
./build/Debug/soft-raster.exe --offscreen --frames 1 --out shot.bmp   # 离屏渲染并存图
```

按键表与命令行选项会在对应里程碑落地后补全。

## 里程碑进度

| # | 内容 | 状态 |
|---|---|---|
| M0 | 仓库骨架、CMake、Win32 窗口 + DIB、测试图案、BMP 截图、位图字体 HUD | ☐ |
| M1 | 数学库 + 标量 edge-function 光栅器（Backend A）| ☐ |
| M2 | 完整几何阶段：VS / 六面裁剪 / 透视除法 / 视口 / Setup / 背面剔除 | ☐ |
| M3 | Z-buffer + 透视正确插值 + 顶点色 + golden image | ☐ |
| M4 | 透视正确 UV + 纹理采样 + mipmap | ☐ |
| M5 | OBJ/MTL 导入 + 多材质 + 包围球自动 fit | ☐ |
| M6 | 光照：Gouraud 与 Blinn-Phong 逐像素对比、线性空间 + sRGB | ☐ |
| M7 | 轨道相机 + 阶段可视化 + 计时 HUD（**可演示终态**）| ☐ |
| M8 | tile 分箱 + 多线程（Backend B），验收 ≥15 fps @1280×720 | ☐ |
| M9 | SIMD inner loop（覆盖 + 插值 + 深度掩码）| ☐ |
| M10 | SSAA、golden 脚本化、笔记成文、效果图 | ☐ |

## 依赖策略

渲染数学、光栅化、着色、OBJ 解析全部手写。唯一的第三方文件是 `third_party/stb_image.h`
（只在 loader 里做 PNG/JPG 贴图解码，核心库不感知它），这样网上随便下载的 OBJ + PNG 贴图能直接拖进来演示。
窗口与显示是 Win32 GDI + `CreateDIBSection`：像素由渲染器直接写进 DIB 内存，`BitBlt` 上屏。
HUD 文字也是自绘的 8×8 位图字体——屏幕上没有任何一个像素不是我们算出来的。
