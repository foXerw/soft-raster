# M4.1 未完成的工作（从构建里摘出来了）

这里放的是 M4 第一个任务的代码与测试：mipmap 链生成。它们**没有被 CMake 引用**，
所以仓库当前是全绿的（191 个用例），而这份代码处于"实现写完、测试进程崩溃"的状态。

## 症状

测试进程 `rc=3` 中止，一行 `FAIL` 都没有 —— 是崩溃，不是断言失败。
把 `test_texture.cpp` 摘出构建后其余全绿，所以崩溃在这个文件里，具体哪一条未定位。

## 两个最可疑的点（按优先级）

1. `generateMipChain`：`const ImageRGBA8& prev = chain.back();` 之后 `chain.push_back(next)`
   会重分配 ⇒ `prev` 悬垂。这是 Task 0.5 记过的"含内部指针的对象被移动"同一类坑。
2. 测试夹具 `makeImage`：无捕获 lambda 转函数指针 + 按值返回 `ImageRGBA8`。

## 恢复步骤

1. 三个文件改名回 `src/render/texture.h`、`src/render/texture.cpp`、`tests/unit/test_texture.cpp`
2. `CMakeLists.txt` 两处加回：core 源文件列表加 `src/render/texture.cpp`，
   `soft_raster_tests` 源文件列表加 `tests/unit/test_texture.cpp`
3. **先给 `tests/harness.h` 加一个按名字过滤用例的参数**（`soft_raster_tests --only mip_chain`），
   再用它把崩溃缩小到单条测试。别用"摘掉整个文件再跑"——那只能定位到文件，
   上一轮我就在这上面白花了两轮。

上下文与裁决记录见 `.superpowers/sdd/2026-11-21-soft-raster-m0-m3/progress.md` 的
"M4 开工（未完成）"一节。
