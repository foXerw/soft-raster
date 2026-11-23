// M5.7：三处方向约定的锚 1（资产版）与锚 3（合成，零容差）。
//
// 设计 §1/§10 的论点：M5 把三条**独立**的方向约定翻译进管线，其中两条都是"翻一行"
// （OBJ 的 vt：v=0 是图片**下**边缘；TGA 的 origin bit）。两条同时做错会精确抵消 ——
// 画面看起来完全正确，合成检查全绿。所以每条翻转各有一个独立的锚：
//   锚 1（本文件）：parseObj 的 v = 1 - v，对着 makeCube 的 uv 约定钉死（资产版；
//                   内联版在 test_obj.cpp 的 obj_flips_vt_because_obj_v_zero_is_the_image_bottom）。
//   锚 2（test_tga_writer.cpp，M5.3 已提交）：我们 TGA 读取器的 origin bit 处理，
//                   对着 makeCheckerImage 的字节钉死。
//   锚 3（本文件）：OBJ 渲染的场景与 M4 参考图**逐字节相同**（HUD 条除外）。
// 变异验证（M5.7 Step 7）实测：双翻转同时做错时，抵消在**实数算术与 level 0 上精确**
// （关 mip 渲染的两帧逐字节相同，md5 一致），但 mip 路径的浮点舍入留下 3 个 ±1 像素 ——
// 零容差的锚 3 连"抵消帧"都抓得住，而 ±1 容差会放过它；锚 1/2 则各自红得干脆。
// 这就是"合成检查不充分"的实证，也是本文件与零容差存在的理由。
// 但抵消的判别只能靠锚 1/2；锚 3 在 ③ 下的红是浮点舍入残差，换编译器或 M9 SIMD 后可能消失，不可依赖。
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "golden/bmp_read.h"
#include "harness.h"
#include "loader/model.h"
#include "loader/obj.h"
#include "loader/obj_scene.h"
#include "scene/golden_scene.h"
#include "scene/mesh.h"
#include "test_util.h"   // Canvas

#ifndef SR_SOURCE_DIR
#define SR_SOURCE_DIR "."
#endif

namespace {

constexpr char kCubeObj[] = SR_SOURCE_DIR "/assets/obj/cube.obj";
constexpr char kRefM4[] = SR_SOURCE_DIR "/assets/golden/m4_textured_cube.bmp";

bool readText(const char* path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// ---- 锚 1 的规范化三角形 ----------------------------------------------------
// 一个角 = (pos.xyz, normal.xyz, uv.xy) 共 8 个 float；一个三角形 = 3 个角 24 个 float。
using Tri = std::array<float, 24>;

// 取角序的 3 种旋转中字典序最小者作代表：吸收"从哪个角起步"的选择，
// 但**不吸收绕向**——镜像三角形的旋转集合与原来不同，代表元几乎必然不同 ⇒ 镜像必红。
// （std::array 的比较是逐元素字典序，C++20 下由 <=> 合成。）
Tri canonicalTri(const sr::Mesh& m, size_t i0, size_t i1, size_t i2) {
    const size_t idx[3] = {i0, i1, i2};
    std::array<Tri, 3> rot{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            const size_t vi = idx[(r + c) % 3];
            float* dst = rot[static_cast<size_t>(r)].data() + static_cast<size_t>(c) * 8;
            dst[0] = m.positions[vi].x;
            dst[1] = m.positions[vi].y;
            dst[2] = m.positions[vi].z;
            dst[3] = m.normals[vi].x;
            dst[4] = m.normals[vi].y;
            dst[5] = m.normals[vi].z;
            dst[6] = m.uvs[vi].x;
            dst[7] = m.uvs[vi].y;
        }
    }
    return *std::min_element(rot.begin(), rot.end());
}

std::vector<Tri> canonicalTriangles(const sr::Mesh& m) {
    std::vector<Tri> tris;
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3)
        tris.push_back(canonicalTri(m, m.indices[i], m.indices[i + 1], m.indices[i + 2]));
    std::sort(tris.begin(), tris.end());
    return tris;
}

void printCorner(const char* tag, const float* c) {
    std::printf("[anchor1]   %s pos(%.6g,%.6g,%.6g) n(%.6g,%.6g,%.6g) uv(%.6g,%.6g)\n", tag,
                static_cast<double>(c[0]), static_cast<double>(c[1]), static_cast<double>(c[2]),
                static_cast<double>(c[3]), static_cast<double>(c[4]), static_cast<double>(c[5]),
                static_cast<double>(c[6]), static_cast<double>(c[7]));
}

}  // namespace

SR_TEST(anchor1_cube_obj_parses_to_the_same_triangles_as_makeCube) {
    // 锚 1 的资产版：committed 的 cube.obj 解析出来必须与 makeCube(1.6f, true) 是
    // **同一组三角形**（规范化后逐元素比对，容差 1e-5f）。它守的是 parseObj 出口那一次
    // v = 1 - v（D9）：cube.obj 的 vt 按 OBJ 惯例写（v=0 = 图片下边缘），若解析器不翻，
    // 每个面的 uv 上下对调 ⇒ 本条红。变异①（删 1.0f - v）的红必须落在这里。
    // 本测试同时是 cube.obj 的**编写工具**：失败信息打印第一个不一致的三角形下标与三对角值。
    std::string text;
    const bool read = readText(kCubeObj, text);
    SR_ASSERT(read);
    if (!read) {
        std::printf("[anchor1] 读不到 %s\n", kCubeObj);
        return;
    }
    sr::Model parsed;
    std::vector<std::string> warnings;
    sr::ObjStats stats;
    std::string err;
    // 材质表与几何无关，给空 span（parseObj 兜底一个 defaultMaterial）。
    const bool ok = sr::parseObj(text, {}, parsed, warnings, stats, err);
    SR_ASSERT(ok);
    if (!ok) {
        std::printf("[anchor1] 解析失败: %s\n", err.c_str());
        return;
    }

    const sr::Mesh want = sr::makeCube(1.6f, true);
    // positions.size() == 24（== 面角数）：立方体每面的 vt/vn 都不同，(v,vt,vn) 三元组
    // 跨面必然不同 ⇒ **这个文件不会发生跨面共享**。这是立方体的固有形状，不是"去重没生效"
    // 的证据 —— 共享的情形由 M5.2 的内联夹具（obj_dedups_vertices_by_the_v_vt_vn_triple）钉。
    SR_ASSERT_EQ(parsed.mesh.positions.size(), 24);
    SR_ASSERT_EQ(parsed.mesh.indices.size(), 36);
    SR_ASSERT_EQ(stats.triangles, 12);
    SR_ASSERT_EQ(want.positions.size(), 24);
    SR_ASSERT_EQ(want.indices.size(), 36);
    // SR_ASSERT 失败不中止：尺寸不对时继续索引就是越界读（trap (b)），先守住再比。
    if (parsed.mesh.positions.size() != 24 || parsed.mesh.indices.size() != 36 ||
        want.positions.size() != 24 || want.indices.size() != 36)
        return;

    const std::vector<Tri> got = canonicalTriangles(parsed.mesh);
    const std::vector<Tri> expect = canonicalTriangles(want);
    SR_ASSERT_EQ(got.size(), expect.size());
    if (got.size() != expect.size()) return;

    int diffs = 0;
    for (size_t t = 0; t < got.size(); ++t) {
        bool same = true;
        for (size_t k = 0; k < got[t].size(); ++k) {
            if (std::fabs(got[t][k] - expect[t][k]) > 1e-5f) {
                same = false;
                break;
            }
        }
        if (!same) {
            if (diffs == 0) {
                std::printf("[anchor1] 第一个不一致的三角形 #%zu（两边都已规范化+排序）：\n", t);
                for (int c = 0; c < 3; ++c) {
                    const size_t off = static_cast<size_t>(c) * 8;
                    printCorner("got ", got[t].data() + off);
                    printCorner("want", expect[t].data() + off);
                }
            }
            ++diffs;
        }
    }
    SR_ASSERT_EQ(diffs, 0);
}

SR_TEST(anchor3_obj_golden_is_the_m4_golden_except_the_hud_strip) {
#ifndef _MSC_VER
    // C1′：golden 只在 MSVC 主路径判定（与 tests/golden/golden.cpp 同一条理由）。
    std::printf("[skip] golden 只在 MSVC 判定（当前不是 MSVC）\n");
    return;
#else
    // 锚 3（收紧 2：**不新增参考图**）：OBJ 场景在进程内渲染，与既有的 M4 参考图
    // 逐像素**精确相等**（HUD 条除外）。这条不是数值回归，是**约定一致性**检查 ——
    // 容差是 0，C1′ 的 ±1 规则不适用（反正只在 MSVC 判定）。
    //
    // 它为什么有意义（Ruling 63）：cube.mtl 的 map_Kd ../tga/checker.tga 按 .tga 后缀
    // 被 loadModel 路由到**我们自己的** loadTgaFile —— 正是锚 2 守着的那个解析器。
    // 于是这张合成图同时经过两条"翻一行"的翻译（OBJ vt 与 TGA origin bit），
    // 任何一条单独做错都会让画面上下翻转 ⇒ 本条红（变异①/②实测各 4239 像素）。
    // 两条同时做错时抵消在实数算术上精确、在浮点上不精确：mip 路径的舍入留下
    // 3 个 ±1 像素（变异③实测），零容差仍红 —— 若按 ±1 容差比，抵消帧会静默通过，
    // 只有锚 1/2 抓得住。设计 §1 的"抵消后画面完全正确"按此修订：肉眼与 ±1 容差下
    // 正确，逐字节不正确。抵消的判别只能靠锚 1/2；锚 3 在 ③ 下的红是舍入残差，不可依赖。
    //
    // 姿态用 M4 的固定 pose（Ruling D15：spec §7.5 的"golden 用 fit 相机"条款对本锚
    // 明确豁免 —— fit 出来的相机是另一张毫不相干的图）。
    sr::LoadedModel m;
    std::string err;
    const bool ok = sr::loadModel(std::string(kCubeObj), m, err);
    SR_ASSERT(ok);
    if (!ok) {
        std::printf("[anchor3] 载入失败: %s\n", err.c_str());
        return;   // trap (b)：载入失败时继续渲染/索引没有意义
    }
    Canvas canvas(sr::kGoldenWidth, sr::kGoldenHeight);
    sr::FrameStats stats;
    sr::renderObjGoldenScene(m, canvas.rt, &stats);

    int refW = 0, refH = 0;
    std::vector<uint32_t> ref;
    std::string rerr;
    const bool haveRef = srtest::readBmp24(kRefM4, refW, refH, ref, rerr);
    SR_ASSERT(haveRef);
    if (!haveRef) {
        std::printf("[anchor3] 读不到参考图 %s: %s\n", kRefM4, rerr.c_str());
        return;
    }
    SR_ASSERT_EQ(refW, sr::kGoldenWidth);
    SR_ASSERT_EQ(refH, sr::kGoldenHeight);
    if (refW != sr::kGoldenWidth || refH != sr::kGoldenHeight) return;

    // 跳过且只跳过 HUD 矩形：文字从 (3,3) 起，17 字符 × kGlyphAdvancePx(6) = 102 宽、
    // kGlyphHeightPx(8) 高 ⇒ 字形最远到 x=104、y=10，取 x<110 && y<12 恰好罩住。
    // 差异出现在矩形外时**不许**加宽矩形、不许引容差、不许重生成 M4 参考图。
    int diffs = 0, reported = 0;
    for (int y = 0; y < refH; ++y) {
        for (int x = 0; x < refW; ++x) {
            if (x < 110 && y < 12) continue;
            const uint32_t got = canvas.rt.color.readPacked(x, y);
            const uint32_t want = ref[static_cast<size_t>(y) * refW + x];
            if (got != want) {
                if (reported < 5) {
                    std::printf("[anchor3] (%d,%d) 得到 %08X 期望 %08X\n", x, y, got, want);
                    ++reported;
                }
                ++diffs;
            }
        }
    }
    if (diffs > 0) {
        std::printf("[anchor3] 差异像素 %d 个（HUD 条已跳过）。先看锚 1、锚 2 是否同时红 —— "
                    "只有这里红而它们绿，说明差异不在翻转上。\n", diffs);
    }
    SR_ASSERT_EQ(diffs, 0);
#endif
}