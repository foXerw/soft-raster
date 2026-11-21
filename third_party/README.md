# third_party

本目录是 soft-raster 唯一的第三方代码位置。设计文档 C2 的三条限制在这里落地：
**只有这一个第三方文件、只做解码、`soft_raster_core` 不感知它。**

## stb_image.h

| 项 | 值 |
|---|---|
| 项目 | [nothings/stb](https://github.com/nothings/stb) 的 `stb_image.h` |
| 版本 | v2.34（文件第一行自带版本号声明） |
| 许可证 | Public Domain（另附可选的 Unlicense / MIT 双授权声明，见文件头） |
| 取用提交 | `1da9ade1e66195252114f5da497f244704a6c58d`（2026-11-19，改动 `stb_image.h` 的最后一个 commit） |
| 取用方式 | `GET https://api.github.com/repos/nothings/stb/contents/stb_image.h?ref=<commit>` + base64 解码 |
| 字节数 | 278,442 |
| SHA-256 | `dfc2aadda6295f67f677587773f25e5bf8c8ad427ed0db7c518525c91e72e020` |

> 为什么记 sha256：文件**逐字节保持上游原样**，一个字节都不改（版本与许可证信息它自己带着，
> 我加的注释会破坏"拿 sha256 就能确认没被动过"这个性质）。要记录的东西写在这个 README 里。
> 复核：`sha256sum third_party/stb_image.h` 应等于上面那串。

### 引入前做过的检查

- 扫描 `system(` / `_popen` / `WinExec` / `ShellExecute` / `CreateProcess` / `socket(` /
  `URLDownload`：**无命中**。文件里唯一的文件 I/O 是 `stbi_load*` 用到的 `fopen`/`_wfopen`
  （`stbi__fopen`，第 1395 行起），那是"加载图片"这件事本身。
- 本项目的 `src/loader/image_stb.cpp` 用 `#define STB_IMAGE_IMPLEMENTATION` 生成实现，
  且**只在这一个翻译单元**里定义（两处定义会有重复符号）。

### 已知取舍

- stb 自己声明"若安全是优先事项，考虑用 Wuffs"。本项目是学习用的本地渲染器，
  不解析不可信来源的图片流，所以接受这个权衡；真要处理网络下载的贴图时应该重估。
- `stbi_load` 默认把图当成 **top-down** 输出（第一行是图像上边缘），与我们的 C8 约定一致，
  所以 `image_stb.cpp` 里**不需要**翻转行。这条由测试
  `stb_loader_returns_top_down_rows_matching_our_bmp_writer` 钉住。

## 还没引入的东西

- `stb_image_write.h`：截图仍然走手写 BMP（spec §7.4 明确不做 PNG 编码导出）。要引入得单独提议。
- 其它任何依赖：C3 禁止构建期联网与包管理器，CMake 里不出现 `find_package` / `FetchContent`。
