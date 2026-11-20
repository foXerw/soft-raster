cmake_minimum_required(VERSION 3.24)
project(soft_raster VERSION 0.0.1 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# 全局约定（对应设计文档 §5 的 C1 / C1′ / C2 / C3）
#   - C++20，不启用编译器扩展（-std 而非 -std=gnu++）
#   - 构建期不联网：本文件不出现 find_package / FetchContent
#   - MSVC 是主路径；g++ 侧用 -DSR_BUILD_APP=OFF 只构建核心库与测试
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SR_BUILD_APP "Build the Win32 application target" ON)
option(SR_BUILD_TESTS "Build the unit/golden test targets" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# 头文件根目录 = src/，因此源码内写成 #include "render/color.h" 这种形式。
add_library(soft_raster_core STATIC
  src/version.cpp
  src/render/color.cpp
  src/render/render_target.cpp
  src/render/patterns.cpp
  src/loader/bmp_writer.cpp
  src/render/overlay_font.cpp
  src/render/raster_scalar.cpp
  src/render/viewport.cpp
  src/render/clipping.cpp
  src/render/triangle_setup.cpp
  src/render/builtin_shaders.cpp
  src/render/render_backend.cpp
  src/scene/mesh.cpp
)
target_include_directories(soft_raster_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# 项目版本与构建配置要能被代码读到：性能表与 golden 都必须能归因到具体构建。
# SR_BUILD_CONFIG 用 $<CONFIG> 而不是靠 NDEBUG 猜 —— 多配置生成器下 NDEBUG 只在
# Release 与 RelWithDebInfo 都有定义，靠它区分会把两种构建标成同一个名字。
target_compile_definitions(soft_raster_core PRIVATE
  SR_VERSION="${PROJECT_VERSION}"
  SR_BUILD_CONFIG="$<CONFIG>"
)

if(MSVC)
  # /utf-8 是必需的：源码注释里有中文，默认代码页会把它读成乱码并触发 C4819。
  target_compile_options(soft_raster_core PRIVATE /W4 /utf-8 /MP /EHsc)
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # -ffp-contract=off：见 C1′。GCC 默认 fast 会把插值融合成 FMA，与 MSVC 结果不同。
  target_compile_options(soft_raster_core PRIVATE -Wall -Wextra -ffp-contract=off)
endif()

# ---------------------------------------------------------------------------
# 测试
#   每个 tests/unit/*.cpp 都必须直接列进 add_executable 的源文件列表：
#   harness 靠静态初始化自注册，若把测试做成静态库，链接器会把"没人引用"的
#   整个翻译单元丢掉，于是那个文件里的用例静默地一条都不跑。
# ---------------------------------------------------------------------------
if(SR_BUILD_TESTS)
  enable_testing()
  add_executable(soft_raster_tests
    tests/harness.cpp
    tests/main.cpp
    tests/unit/test_smoke.cpp
    tests/unit/test_version.cpp
    tests/unit/test_color.cpp
    tests/unit/test_render_target.cpp
    tests/unit/test_patterns.cpp
    tests/unit/test_bmp.cpp
    tests/unit/test_overlay_font.cpp
    tests/unit/test_vec.cpp
    tests/unit/test_mat.cpp
    tests/unit/test_transform.cpp
    tests/unit/test_plane_eq.cpp
    tests/unit/test_raster_scalar.cpp
    tests/unit/test_mesh.cpp
    tests/unit/test_bbox.cpp
    tests/unit/test_viewport.cpp
    tests/unit/test_clipping.cpp
    tests/unit/test_triangle_setup.cpp
    src/app/cmdline.cpp
    tests/unit/test_cmdline.cpp
  )
  target_link_libraries(soft_raster_tests PRIVATE soft_raster_core)
  target_include_directories(soft_raster_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

  if(MSVC)
    target_compile_options(soft_raster_tests PRIVATE /W4 /utf-8 /MP /EHsc)
  endif()

  # g++ 侧把运行时静态链进测试可执行文件。原因：这台机器上 PATH 同时有 Git 的
  # /mingw64/bin 与 MSYS2 的 /c/msys64/ucrt64/bin，动态链接出来的 exe 会去找
  # libstdc++-6.dll / libgcc_s_*-1.dll，而 CTest 起进程用的是 Windows PATH —— 找不到时
  # 症状是 CTest 报 **Timeout**（进程根本没能启动），非常容易误判成测试里死了循环。
  # -static 之后没有外部运行时 DLL，两侧都能跑。
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(soft_raster_tests PRIVATE -static)
  endif()

  add_test(NAME unit COMMAND soft_raster_tests)

  # golden 测试在 Task 3.5 注册（届时 assets/golden/ 才有参考图）。
endif()

# ---------------------------------------------------------------------------
# 应用（Win32 + GDI）。只有 src/app/main.cpp 存在时才创建目标 —— 它在 Task 0.6
# 出现，这样前几个任务可以独立配置通过。
# ---------------------------------------------------------------------------
if(SR_BUILD_APP AND WIN32 AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/app/main.cpp)
  file(GLOB SR_APP_SOURCES CONFIGURE_DEPENDS src/app/*.cpp)
  add_executable(soft-raster ${SR_APP_SOURCES})
  target_link_libraries(soft-raster PRIVATE soft_raster_core)
  target_compile_definitions(soft-raster PRIVATE SR_VERSION="${PROJECT_VERSION}")
  set_target_properties(soft-raster PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
  if(MSVC)
    target_compile_options(soft-raster PRIVATE /W4 /utf-8 /MP /EHsc /Zi)
  endif()
  target_link_libraries(soft-raster PRIVATE user32 gdi32)
endif()
