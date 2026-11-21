#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "golden/bmp_read.h"
#include "scene/golden_scene.h"
#include "harness.h"
#include "render/render_target.h"
#include "version.h"

namespace {

// 用绝对路径而不是相对 cwd：CTest 的工作目录是 build/，写成相对路径的话
// "参考图找不到"会被误读成"参考图没提交"。路径由 CMake 注入。
#ifndef SR_SOURCE_DIR
#define SR_SOURCE_DIR "."
#endif
constexpr char kRefM3[] = SR_SOURCE_DIR "/assets/golden/m3_cube.bmp";
constexpr char kRefM4[] = SR_SOURCE_DIR "/assets/golden/m4_textured_cube.bmp";
constexpr int kTolerancePerChannel = 1;   // ±1/255/通道（设计文档 C1′ 的具体化）

struct Buffers {
    std::vector<uint8_t> color;
    std::vector<float> depth;
    sr::RenderTarget rt;
    Buffers(int w, int h)
        : color(static_cast<size_t>(w) * h * 4u, 0), depth(static_cast<size_t>(w) * h, 1.0f) {
        rt.color = sr::ColorTarget{reinterpret_cast<uint32_t*>(color.data()), w, h, w * 4};
        rt.depth = sr::DepthTarget{depth.data(), w, h};
    }
};

// 比对逻辑**只有一份**：两张参考图各写一遍循环的话，容差、报告条数、"缺图怎么办"
// 这三件事会在某一次改动里只被改到一处 —— 而没人会去读那个没被改的测试，因为它一直是绿的。
// scene 以函数指针传入：参考图必须由生产代码里的那一个函数生成（与 app 的 --golden-write* 同源）。
void compareWithReference(const char* refPath, const char* genCommand,
                          void (*scene)(sr::RenderTarget&, sr::FrameStats*)) {
    Buffers bufs(sr::kGoldenWidth, sr::kGoldenHeight);
    sr::FrameStats stats;
    scene(bufs.rt, &stats);

    int refW = 0, refH = 0;
    std::vector<uint32_t> ref;
    std::string err;
    if (!srtest::readBmp24(refPath, refW, refH, ref, err)) {
        // 参考图缺失时**报错而不是静默通过**：静默 SKIP 会让"golden 从没跑过"看起来像"golden 全绿"。
        // 生成方式写在 assets/golden/README.md 里。
        SR_ASSERT_FALSE(err.empty());
        std::printf("[golden] 缺少参考图 %s：%s\n", refPath, err.c_str());
        std::printf("[golden] 生成：%s\n", genCommand);
        SR_ASSERT(false);
        return;
    }
    SR_ASSERT_EQ(refW, sr::kGoldenWidth);
    SR_ASSERT_EQ(refH, sr::kGoldenHeight);

    int diffs = 0, reported = 0;
    int firstX = -1, firstY = -1;
    for (int y = 0; y < refH; ++y) {
        for (int x = 0; x < refW; ++x) {
            const uint32_t got = bufs.rt.color.readPacked(x, y);
            const uint32_t want = ref[static_cast<size_t>(y) * refW + x];
            const int dr = std::abs(static_cast<int>((got >> 16) & 0xFF) - static_cast<int>((want >> 16) & 0xFF));
            const int dg = std::abs(static_cast<int>((got >> 8) & 0xFF) - static_cast<int>((want >> 8) & 0xFF));
            const int db = std::abs(static_cast<int>(got & 0xFF) - static_cast<int>(want & 0xFF));
            if (dr > kTolerancePerChannel || dg > kTolerancePerChannel || db > kTolerancePerChannel) {
                if (diffs == 0) { firstX = x; firstY = y; }
                ++diffs;
                if (reported < 5) {
                    std::printf("[golden] (%d,%d) 得到 %08X 期望 %08X\n", x, y, got, want);
                    ++reported;
                }
            }
        }
    }
    if (diffs > 0) {
        std::printf("[golden] %s 差异像素 %d 个，第一个在 (%d,%d)；构建 %s\n", refPath, diffs,
                    firstX, firstY, sr::buildInfoString());
    }
    SR_ASSERT_EQ(diffs, 0);
}

}  // namespace

SR_TEST(golden_m3_cube_matches_reference) {
#ifndef _MSC_VER
    // C1′：golden 只在 MSVC 主路径判定。g++ 侧的标准库三角函数/pow 与 MSVC 不保证逐位一致，
    // 为跨编译器 bit-stability 自建数学库属于计划外的一大块，且与学渲染无关。
    std::printf("[skip] golden 只在 MSVC 判定（当前不是 MSVC）\n");
    return;
#else
    compareWithReference(kRefM3, "soft-raster --golden-write assets/golden/m3_cube.bmp",
                         &sr::renderGoldenScene);
#endif
}

SR_TEST(golden_m4_textured_cube_matches_reference) {
#ifndef _MSC_VER
    std::printf("[skip] golden 只在 MSVC 判定（当前不是 MSVC）\n");
    return;
#else
    compareWithReference(kRefM4,
                         "soft-raster --golden-write-textured assets/golden/m4_textured_cube.bmp",
                         &sr::renderGoldenSceneTextured);
#endif
}
