// M6.8：lit golden 的量化断言（设计 §8.2 #7a/#7b/#7c/#8）与参考图守卫。
//
// C1′：凡涉及逐像素 pow 的断言只在 MSVC 主路径判定，g++ 侧打印可见 [skip]。
// 唯一例外是 rig 漂移守卫（纯数据比较，无 pow）——两侧都跑。
//
// 夹具 = Step 1 的生产夹具本身：makeUVSphere(1, kGoldenLitStacks, kGoldenLitSlices)
//（slices 奇数 ⇒ 无顶点正对眼，§8.2 约束 2）+ goldenModel/View/Projection（D15 固定
// 姿态，不用 fit）+ goldenLitRig()（与参考图**同一个** static 单例）+
// litDefaultMaterialShade()。两条着色模式（Phong/Gouraud）共用同一份几何/相机/rig。
//
// 高光 isolation：spec(x,y) = linear(full) − linear(Ks=0 对照)。背景渐变在两张图里
// 逐字节相同 ⇒ 差值在球外恒 0，重心/半高宽天然只统计球面，不需要掩码或排除矩形。
// 8-bit 量化半步长 ≤ 0.00446 线性（推导见 test_lit_shaders 的 textured_model_keeps_
// the_texture_under_phong），下面所有容差都从它出发。
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "app/scene.h"   // buildLightRig / OrbitCamera（rig 漂移守卫）
#include "golden/bmp_read.h"
#include "harness.h"
#include "math/transform.h"
#include "render/builtin_shaders.h"
#include "render/color.h"
#include "render/pipeline.h"
#include "render/render_backend.h"
#include "scene/golden_scene.h"
#include "scene/mesh.h"
#include "test_util.h"   // Canvas

#ifndef SR_SOURCE_DIR
#define SR_SOURCE_DIR "."
#endif

#ifdef _MSC_VER

namespace {

constexpr char kRefM3[] = SR_SOURCE_DIR "/assets/golden/m3_cube.bmp";
constexpr char kRefM4[] = SR_SOURCE_DIR "/assets/golden/m4_textured_cube.bmp";
constexpr int kW = sr::kGoldenWidth;
constexpr int kH = sr::kGoldenHeight;

// 世界点 → 像素。**故意手写** NDC→像素（含 y 翻转），不调 sr::Viewport：
// 本文件判的是"高光重心落在物理推导的位置"，拿被测管线自己的视口算期望值
// 等于让它给自己打分（test_golden_scene_textured 的同款分工；视口由 test_viewport
// 与 M3 golden 守）。
sr::Vec2 pixelOf(const sr::Vec3& world) {
    const sr::Vec4 clip = sr::goldenProjection(kW, kH) * (sr::goldenView() * sr::makeVec4(world, 1.0f));
    if (!(std::fabs(clip.w) > 1e-6f)) return sr::Vec2{-1e9f, -1e9f};
    const float invW = 1.0f / clip.w;
    return sr::Vec2{(clip.x * invW * 0.5f + 0.5f) * static_cast<float>(kW),
                    (1.0f - (clip.y * invW * 0.5f + 0.5f)) * static_cast<float>(kH)};
}

}  // namespace

#endif  // _MSC_VER

// ---- rig 漂移守卫（无 pow，两侧都跑）-----------------------------------------

SR_TEST(golden_lit_rig_matches_the_app_default_rig_at_the_golden_eye) {
    // goldenLitRig() 是 D19 app 默认在 core 层的镜像（core 不能 include app/cmdline.h）。
    // 镜像不许悄悄漂：这里用**生产的** buildLightRig(全默认 AppOptions, 球包围盒,
    // yaw0/pitch0/dist5 相机) 逐字段对账。漂的症状是"参考图与窗口画面各改各的"。
    app::AppOptions o;   // 全默认
    sr::OrbitCamera cam;
    cam.yaw = 0.0f;
    cam.pitch = 0.0f;
    cam.distance = sr::kGoldenCamZ;   // eye = (0,0,5) = goldenView 的眼
    const sr::Mesh sphere = sr::makeUVSphere(1.0f, sr::kGoldenLitStacks, sr::kGoldenLitSlices);
    const sr::LightRig want = app::buildLightRig(o, sphere.bounds, cam);
    const sr::LightRig& got = sr::goldenLitRig();
    SR_ASSERT(&sr::goldenLitRig() == &got);   // 单例（C5：一个实例）
    SR_ASSERT_NEAR(got.keyDirWorld.x, want.keyDirWorld.x, 1e-6f);
    SR_ASSERT_NEAR(got.keyDirWorld.y, want.keyDirWorld.y, 1e-6f);
    SR_ASSERT_NEAR(got.keyDirWorld.z, want.keyDirWorld.z, 1e-6f);
    SR_ASSERT_NEAR(got.keyColor.r, want.keyColor.r, 1e-6f);
    SR_ASSERT_NEAR(got.keyIntensity, want.keyIntensity, 1e-6f);
    SR_ASSERT_NEAR(got.pointColor.r, want.pointColor.r, 1e-6f);
    SR_ASSERT_NEAR(got.pointIntensity, want.pointIntensity, 1e-6f);
    SR_ASSERT_NEAR(got.pointPosWorld.x, want.pointPosWorld.x, 1e-6f);
    SR_ASSERT_NEAR(got.pointPosWorld.y, want.pointPosWorld.y, 1e-6f);
    SR_ASSERT_NEAR(got.pointPosWorld.z, want.pointPosWorld.z, 1e-6f);
    SR_ASSERT_NEAR(got.eyeWorld.x, want.eyeWorld.x, 1e-6f);
    SR_ASSERT_NEAR(got.eyeWorld.y, want.eyeWorld.y, 1e-6f);
    SR_ASSERT_NEAR(got.eyeWorld.z, want.eyeWorld.z, 1e-6f);
    SR_ASSERT(got.attenuate == want.attenuate);
    SR_ASSERT_NEAR(got.attenuateRadius, want.attenuateRadius, 1e-6f);
    SR_ASSERT_NEAR(got.ambient.r, want.ambient.r, 1e-6f);
    SR_ASSERT_NEAR(got.ambient.g, want.ambient.g, 1e-6f);
    SR_ASSERT_NEAR(got.ambient.b, want.ambient.b, 1e-6f);
}

#ifdef _MSC_VER

namespace {

// 夹具 uniforms：golden 姿态 + 指定 rig/材质（两个阶段挂同一份，Ruling 97 同形）。
sr::VertexUniforms litVu(const sr::LightRig& rig, const sr::MaterialShade& mat) {
    sr::VertexUniforms vu;
    vu.model = sr::goldenModel();
    vu.view = sr::goldenView();
    vu.proj = sr::goldenProjection(kW, kH);
    sr::setNormalMatrix(vu, vu.model, "lit fixture ");
    vu.lights = &rig;
    vu.mat = &mat;
    return vu;
}

sr::FragmentUniforms litFu(const sr::LightRig& rig, const sr::MaterialShade& mat) {
    sr::FragmentUniforms fu;   // 无贴图：texture 保持 nullptr（albedo=白，D26）
    fu.lights = &rig;
    fu.mat = &mat;
    return fu;
}

// 渲夹具（剔除/深度与生产场景一致）。Canvas 就地构造，绝不按值返回。
void renderFixture(Canvas& c, const sr::Mesh& sphere, sr::VertexShaderFn vs, sr::FragmentShaderFn fs,
                   const sr::LightRig& rig, const sr::MaterialShade& mat, sr::FrameStats& s) {
    c.rt.color.clear(0xFF000000u);
    c.rt.depth.clear();
    sr::PipelineState st;
    st.target = &c.rt;
    st.vertexShader = vs;
    st.fragmentShader = fs;
    st.vu = litVu(rig, mat);
    st.fu = litFu(rig, mat);
    st.depthTest = true;
    st.depthWrite = true;
    st.cull.backfaceCull = true;
    st.stats = &s;
    sr::BackendA backend;
    backend.draw(sphere, st);
}

// 高光-only 图：full − Ks=0 对照（线性域）。球外恒 0（背景逐字节相同）。
struct SpecMap {
    std::vector<float> v;   // kW*kH，线性高光强度（r 通道；白光 ⇒ r=g=b）
    float peak = 0.0f;
    int peakX = 0, peakY = 0;
    SpecMap(const Canvas& full, const Canvas& noSpec) : v(static_cast<size_t>(kW) * kH, 0.0f) {
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                const float d = sr::decodeSRGB(full.rt.color.readPacked(x, y)).r -
                                sr::decodeSRGB(noSpec.rt.color.readPacked(x, y)).r;
                v[static_cast<size_t>(y) * kW + x] = d > 0.0f ? d : 0.0f;   // 量化负残差钳 0
                if (d > peak) { peak = d; peakX = x; peakY = y; }
            }
        }
    }
    float at(int x, int y) const { return v[static_cast<size_t>(y) * kW + x]; }
    // 点光瓣的局部半高宽：默认 rig 的高光图是**双瓣**的 —— 点光瓣峰在正对眼的
    // 球面点（Ks·pointInt=0.14），key 瓣峰在 N≈H_key 处（Ks·keyInt=0.1225，
    // 推导见 #7c 注释）。全局峰属点瓣，但 Gouraud 的顶点采样可能让 key 瓣更高
    //（实测 G 全局峰在 key 瓣）⇒ 比"同一瓣"的半高宽必须在解析投影的点瓣中心
    // ±6px 内找局部峰（瓣半径 ≈ 31.2px·sin8.4° ≈ 4.6px，6px 罩住；key 瓣中心
    // 距点瓣中心 ≈19px，其半高区够不到这里）。行取局部峰所在行 ⇒ 与 key 瓣
    //（y≈74）天然分离。
    int pointLobeWidthAtHalfMax(const sr::Vec2& expectCenter) const {
        const int cx = static_cast<int>(expectCenter.x);
        const int cy = static_cast<int>(expectCenter.y);
        float lp = 0.0f;
        int lx = cx, ly = cy;
        for (int y = cy - 6; y <= cy + 6; ++y) {
            for (int x = cx - 6; x <= cx + 6; ++x) {
                if (x < 0 || y < 0 || x >= kW || y >= kH) continue;
                if (at(x, y) > lp) { lp = at(x, y); lx = x; ly = y; }
            }
        }
        if (!(lp > 0.0f)) return 0;
        const float half = lp * 0.5f;
        int x0 = lx, x1 = lx;
        while (x0 > 0 && at(x0 - 1, ly) >= half) --x0;
        while (x1 < kW - 1 && at(x1 + 1, ly) >= half) ++x1;
        return x1 - x0 + 1;
    }
    // 半高面积：≥ peak/2 的像素总数（设计 §8.2 #7b 的"峰值一半所围区域面积"）。
    // 用**全局**峰：两瓣的总面积都受插值方式影响（Gouraud 把两瓣都摊开），
    // 所以总面积比是合法的"摊开"度量，不需要分局部瓣。
    int areaAtHalfMax() const {
        if (!(peak > 0.0f)) return 0;
        const float half = peak * 0.5f;
        int n = 0;
        for (float q : v) if (q >= half) ++n;
        return n;
    }
    // 重心（高光强度加权）。球外权重 0 ⇒ 不需要掩码。
    sr::Vec2 centroid() const {
        double sx = 0, sy = 0, sw = 0;
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                const float w = at(x, y);
                sx += w * (x + 0.5);
                sy += w * (y + 0.5);
                sw += w;
            }
        }
        if (!(sw > 0)) return sr::Vec2{-1e9f, -1e9f};
        return sr::Vec2{static_cast<float>(sx / sw), static_cast<float>(sy / sw)};
    }
};

// 漫反射重心：Ks=0 帧的亮度加权，深度掩码取球面（背景渐变比球亮，不掩会反客为主）。
sr::Vec2 diffuseCentroid(const Canvas& c) {
    double sx = 0, sy = 0, sw = 0;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            if (!(c.rt.depth.read(x, y) < 1.0f)) continue;
            const float w = sr::decodeSRGB(c.rt.color.readPacked(x, y)).r;
            sx += w * (x + 0.5);
            sy += w * (y + 0.5);
            sw += w;
        }
    }
    if (!(sw > 0)) return sr::Vec2{-1e9f, -1e9f};
    return sr::Vec2{static_cast<float>(sx / sw), static_cast<float>(sy / sw)};
}

// 指定扫描线上的三角形边交点（解析投影，不用光栅化结果）：
// 每条边跨过 y = y0+0.5 时记一个 x。kink 断言用它们定位"导数不连续应当出现在哪"。
std::vector<float> edgeCrossings(const sr::Mesh& m, int y0) {
    const sr::Mat4 mvp = sr::goldenProjection(kW, kH) * (sr::goldenView() * sr::goldenModel());
    std::vector<sr::Vec2> px(m.positions.size());
    for (size_t i = 0; i < m.positions.size(); ++i) {
        const sr::Vec4 clip = mvp * sr::makeVec4(m.positions[i], 1.0f);
        const float invW = 1.0f / clip.w;
        px[i] = sr::Vec2{(clip.x * invW * 0.5f + 0.5f) * static_cast<float>(kW),
                         (1.0f - (clip.y * invW * 0.5f + 0.5f)) * static_cast<float>(kH)};
    }
    const float y = static_cast<float>(y0) + 0.5f;
    std::vector<float> xs;
    // 共享边会被相邻两个三角形各报一次 ⇒ 按 0.5px 网格去重（不去重会让边窗口
    // 翻倍覆盖，"内部"样本被挤没，比值退化 —— 实测踩过的坑）。
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        const uint32_t idx[3] = {m.indices[i], m.indices[i + 1], m.indices[i + 2]};
        for (int e = 0; e < 3; ++e) {
            const sr::Vec2 a = px[idx[e]];
            const sr::Vec2 b = px[idx[(e + 1) % 3]];
            if ((a.y <= y && b.y > y) || (b.y <= y && a.y > y)) {
                const float t = (y - a.y) / (b.y - a.y);
                xs.push_back(a.x + t * (b.x - a.x));
            }
        }
    }
    std::sort(xs.begin(), xs.end());
    std::vector<float> uniq;
    for (float x : xs) {
        if (uniq.empty() || x - uniq.back() > 0.5f) uniq.push_back(x);
    }
    return uniq;
}

// kink 度量（三个互补视角，同一事实：Gouraud 逐三角形仿射，逐像素内部也弯曲）：
//   ratio      = 边窗口（交点 ±1px）max|D2| 均值 ÷ 非边位置 |D2| 均值（边/内对比）
//   interior   = 非边位置 |D2| 均值（Gouraud 内部只剩量化噪声 ⇒ 显著更小）
//   slopeNorm  = 每条边 max|D2| / 窗口 max|D1| 的均值（"kink 相对局部斜率"，
//                设计 §8.2 #8 的原话；对亮度尺度不变）
// 这正是 §8.2 #8 的措辞：测的是**一阶导的不连续**，不是感知意义上的 Mach band。
// L 由调用方给（全帧亮度行；高光-only 相减会把量化噪声翻倍，实测分不开，见测试注释）。
struct KinkMetrics {
    float ratio = -1.0f;
    float interior = -1.0f;
    float slopeNorm = -1.0f;
};
KinkMetrics kinkMetrics(const float* L, int xMin, int xMax, const std::vector<float>& edgeXs) {
    KinkMetrics out;
    if (xMax - xMin < 8) return out;   // 扫描线没穿过球 ⇒ 让调用方断言红
    std::vector<bool> isEdge(static_cast<size_t>(kW), false);
    double edgeSum = 0;
    int edgeN = 0;
    for (float xe : edgeXs) {
        const int xi = static_cast<int>(std::lround(xe));
        if (xi < xMin + 3 || xi > xMax - 3) continue;   // 轮廓边的深度跳变不是插值 kink
        float best = 0;
        for (int x = xi - 1; x <= xi + 1; ++x) {
            isEdge[static_cast<size_t>(x)] = true;
            const float d2 = std::fabs(L[x + 1] - 2.0f * L[x] + L[x - 1]);
            if (d2 > best) best = d2;
        }
        edgeSum += best;
        ++edgeN;
    }
    double interiorSum = 0;
    int interiorN = 0;
    for (int x = xMin + 2; x <= xMax - 2; ++x) {
        if (isEdge[static_cast<size_t>(x)]) continue;
        interiorSum += std::fabs(L[x + 1] - 2.0f * L[x] + L[x - 1]);
        ++interiorN;
    }
    if (edgeN == 0 || interiorN == 0) return out;
    const double edgeMean = edgeSum / edgeN;
    const double interiorMean = interiorSum / interiorN;
    double slopeNormSum = 0;
    for (float xe : edgeXs) {
        const int xi = static_cast<int>(std::lround(xe));
        if (xi < xMin + 3 || xi > xMax - 3) continue;
        float bestD2 = 0, bestD1 = 0;
        for (int x = xi - 1; x <= xi + 1; ++x) {
            const float d2 = std::fabs(L[x + 1] - 2.0f * L[x] + L[x - 1]);
            const float d1 = std::fabs(L[x + 1] - L[x - 1]) * 0.5f;
            if (d2 > bestD2) bestD2 = d2;
            if (d1 > bestD1) bestD1 = d1;
        }
        slopeNormSum += bestD2 / (bestD1 + 1e-9f);
    }
    out.ratio = static_cast<float>(edgeMean / (interiorMean + 1e-9));
    out.interior = static_cast<float>(interiorMean);
    out.slopeNorm = static_cast<float>(slopeNormSum / edgeN);
    return out;
}

}  // namespace

// ---- #7a 前提：确定性、不饱和、拿错参考图不可能静默通过 ------------------------

SR_TEST(lit_golden_is_deterministic) {
    // 参考图的前提（M4 先例）：同一生产函数两次调用逐像素相同。
    Canvas a{kW, kH}, b{kW, kH};
    sr::FrameStats sa, sb;
    sr::renderGoldenSceneLit(a.rt, &sa);
    sr::renderGoldenSceneLit(b.rt, &sb);
    int diffs = 0;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            if (a.rt.color.readPacked(x, y) != b.rt.color.readPacked(x, y)) ++diffs;
    SR_ASSERT_EQ(diffs, 0);
    SR_ASSERT_EQ(sa.fragmentsWritten, sb.fragmentsWritten);
    SR_ASSERT(sa.fragmentsWritten > 1000);   // 球真的画了（不是空帧的平凡确定性）
}

SR_TEST(lit_golden_has_a_highlight_that_does_not_saturate) {
    // §8.2 #7a / D19：峰值通道 < 255 且高光像素 > 0。没有这条，坏重生成（过曝白饼）
    // 会在下一次重生成后通过普通像素比对 —— 它是"坏重生成必红某条"的保险。
    // **区域说明**：HUD 标签的墨迹是故意的纯白（encodeSRGB(1)=255），光度断言只针对
    // 被照亮的表面 ⇒ 统计 y≥12（标签条 y∈[3,10]）。这不是比对用的排除矩形：
    // 参考图比对（golden.cpp）一个像素都不跳过。
    Canvas c{kW, kH};
    sr::FrameStats s;
    sr::renderGoldenSceneLit(c.rt, &s);
    int maxByte = 0;
    for (int y = 12; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const uint32_t p = c.rt.color.readPacked(x, y);
            const int r = static_cast<int>((p >> 16) & 0xFFu);
            const int g = static_cast<int>((p >> 8) & 0xFFu);
            const int b = static_cast<int>(p & 0xFFu);
            if (r > maxByte) maxByte = r;
            if (g > maxByte) maxByte = g;
            if (b > maxByte) maxByte = b;
        }
    }
    std::printf("[lit7a] 表面区最大通道 %d（须 <255）\n", maxByte);
    SR_ASSERT(maxByte < 255);

    // 高光像素 = 比 Ks=0 对照亮 > 0.01 线性（M6.6 同款定义，不靠魔法阈值）。
    const sr::Mesh sphere = sr::makeUVSphere(1.0f, sr::kGoldenLitStacks, sr::kGoldenLitSlices);
    const sr::MaterialShade mat = sr::litDefaultMaterialShade();
    sr::MaterialShade noSpec = mat;
    noSpec.specular = sr::LinearColor{0.0f, 0.0f, 0.0f};
    Canvas full{kW, kH}, base{kW, kH};
    sr::FrameStats sf, sb;
    renderFixture(full, sphere, sr::transformVS, sr::blinnPhongPerPixelFS, sr::goldenLitRig(), mat, sf);
    renderFixture(base, sphere, sr::transformVS, sr::blinnPhongPerPixelFS, sr::goldenLitRig(), noSpec, sb);
    const SpecMap spec(full, base);
    int highlight = 0;
    for (float q : spec.v) if (q > 0.01f) ++highlight;
    std::printf("[lit7a] 高光像素 %d 个，峰值线性 %.4f（字节 %d）\n", highlight,
                static_cast<double>(spec.peak), maxByte);
    SR_ASSERT(highlight > 0);
    SR_ASSERT(spec.peak > 0.05f);   // 不是噪声级的高光（量化半步长 0.00446 的 10 倍）
}

SR_TEST(lit_golden_frame_is_not_the_m3_or_m4_frame) {
    // M4 先例（textured_golden_frame_is_not_the_m3_frame）的 M6.8 对应物：
    // 本任务新增参考图 ⇒ "拿错参考图"必须有独立的红。若 renderGoldenSceneLit 哪天
    // 退化成 M3/M4 场景的别名（或 --golden-write-lit 接错函数），对着**自己的**参考图
    // 的比对会在重生成后静默全绿，而这条会红 —— 它比较的对象是另外两张参考图。
    Canvas lit{kW, kH};
    sr::FrameStats s;
    sr::renderGoldenSceneLit(lit.rt, &s);
    const char* refs[] = {kRefM3, kRefM4};
    for (const char* path : refs) {
        int rw = 0, rh = 0;
        std::vector<uint32_t> ref;
        std::string err;
        const bool have = srtest::readBmp24(path, rw, rh, ref, err);
        SR_ASSERT(have);
        if (!have) { std::printf("[lit-not] 读不到 %s: %s\n", path, err.c_str()); return; }
        SR_ASSERT_EQ(rw, kW);
        SR_ASSERT_EQ(rh, kH);
        if (rw != kW || rh != kH) return;
        int diffs = 0, hudDiffs = 0;
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                if (lit.rt.color.readPacked(x, y) == ref[static_cast<size_t>(y) * kW + x]) continue;
                ++diffs;
                if (y < 12 && x < 200) ++hudDiffs;   // 标签条（墨迹到 x=127）
            }
        }
        std::printf("[lit-not] 与 %s 相差 %d 像素（HUD 区 %d）\n", path, diffs, hudDiffs);
        SR_ASSERT(diffs > 1500);   // 球 vs 立方体：几乎整个画面都不同
        SR_ASSERT(hudDiffs > 0);   // 标签也不是 M3/M4 那一行
    }
}

// ---- #7b：Gouraud 摊开逐像素收紧的峰 ------------------------------------------

SR_TEST(gouraud_smears_the_specular_peak_that_per_pixel_localizes) {
    // 同一几何/相机/rig/材质，两模式各渲一次，比**高光-only**图的半高宽与半高面积。
    // 推导（§8.2 #7b）：Gouraud 的高光是顶点值的凸组合 —— 顶点采样够不到 N·H→1
    //（slices 奇数 ⇒ 无顶点正对眼），能量被线性摊到整片三角形；逐像素在每个像素重算
    // H ⇒ 集中。Ruling 91（Gouraud 算完整光照）是这条有意义的前提：若 Gouraud 只有
    // 漫反射，这里测的就是"没有 vs 有"而不是"摊开 vs 收紧"。
    const sr::Mesh sphere = sr::makeUVSphere(1.0f, sr::kGoldenLitStacks, sr::kGoldenLitSlices);
    const sr::MaterialShade mat = sr::litDefaultMaterialShade();
    sr::MaterialShade noSpec = mat;
    noSpec.specular = sr::LinearColor{0.0f, 0.0f, 0.0f};
    const sr::LightRig& rig = sr::goldenLitRig();

    Canvas pf{kW, kH}, pb{kW, kH}, gf{kW, kH}, gb{kW, kH};
    sr::FrameStats s1, s2, s3, s4;
    renderFixture(pf, sphere, sr::transformVS, sr::blinnPhongPerPixelFS, rig, mat, s1);
    renderFixture(pb, sphere, sr::transformVS, sr::blinnPhongPerPixelFS, rig, noSpec, s2);
    renderFixture(gf, sphere, sr::lambertVS, sr::lambertPerVertexFS, rig, mat, s3);
    renderFixture(gb, sphere, sr::lambertVS, sr::lambertPerVertexFS, rig, noSpec, s4);
    SR_ASSERT(s1.fragmentsWritten > 1000);
    SR_ASSERT(s3.fragmentsWritten > 1000);
    const SpecMap P(pf, pb), G(gf, gb);
    const sr::Vec2 lobeCenter = pixelOf(sr::normalize(rig.eyeWorld));   // 点瓣解析中心 (160,90)
    const int wP = P.pointLobeWidthAtHalfMax(lobeCenter);
    const int wG = G.pointLobeWidthAtHalfMax(lobeCenter);
    const int aP = P.areaAtHalfMax(), aG = G.areaAtHalfMax();
    std::printf("[lit7b] 全局峰 P=%.4f@(%d,%d) G=%.4f@(%d,%d)；点瓣半高宽 P=%d G=%d；半高面积 P=%d G=%d\n",
                static_cast<double>(P.peak), P.peakX, P.peakY,
                static_cast<double>(G.peak), G.peakX, G.peakY, wP, wG, aP, aG);
    // 阈值推导（12/15 夹具实测，MSVC Debug 确定性渲染 ⇒ 数值可复现）：
    // 点瓣半高宽 P=10 G=13（比值 1.30），半高面积 P=121 G=181（比值 1.496）。
    // 量化半步长 0.00446 对半高阈值的影响 ≤1px/侧 ⇒ 宽度比阈值 1.25（留 0.05，
    // 约半像素当量）、面积比阈值 1.3（留 0.2，约 ±12px 面积当量）。
    // 24/31 细网格上实测宽 10 vs 10（比值 1.0）——夹具密度是这条断言的前提，
    // 不是可调参数（见 golden_scene.h 的常量注释）。
    SR_ASSERT(wP >= 3);
    SR_ASSERT(wG >= wP + 2);
    SR_ASSERT(aG >= aP + 4);
    SR_ASSERT(static_cast<float>(wG) / static_cast<float>(wP) >= 1.25f);
    SR_ASSERT(static_cast<float>(aG) / static_cast<float>(aP) >= 1.3f);
    // 两模式真的不同（不是"都没高光"的对称夹具）。
    int diff = 0;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            if (pf.rt.color.readPacked(x, y) != gf.rt.color.readPacked(x, y)) ++diff;
    SR_ASSERT(diff > 100);
}

// ---- #7c：高光重心随眼移动（漫反射不随眼，随光向）------------------------------

SR_TEST(the_per_pixel_highlight_center_moves_with_the_eye) {
    // 高光图是双瓣的（推导见 SpecMap::pointLobeWidthAtHalfMax 注释）：点瓣峰
    // Ks·pointInt=0.14 在正对眼的球面点 S=normalize(eyeWorld)（该点 N=V=L ⇒ N·H=1），
    // key 瓣峰 Ks·keyInt=0.1225 在 N≈H_key 处。0.14 > 0.1225（差 14%，量化半步长
    // 0.00446 只有差的 1/4）⇒ **全局峰必属点瓣**，它的解析位置可以钉死。
    // 这条绝对位置钉正是 #14（worldPos 退回物体空间）的红点：P 错成 R⁻¹P_world 时
    // 峰条件变成 N ∥ normalize(E−R⁻¹P)，数值上峰位偏 ~5px（R=rotX0.43·rotY0.61，
    // R⁻¹(0,0,1)≈(−0.52,0.42,0.74) ⇒ E−P_obj 方向偏 +z 约 7°⇒ 31.2px·sin7°≈3.8px，
    // 加上 key 瓣同向偏移，重心/峰位都移出 2.5px 容差）。
    const sr::Mesh sphere = sr::makeUVSphere(1.0f, sr::kGoldenLitStacks, sr::kGoldenLitSlices);
    const sr::MaterialShade mat = sr::litDefaultMaterialShade();
    sr::MaterialShade noSpec = mat;
    noSpec.specular = sr::LinearColor{0.0f, 0.0f, 0.0f};
    const sr::LightRig rig0 = sr::goldenLitRig();

    Canvas f0{kW, kH}, b0{kW, kH};
    sr::FrameStats s1, s2;
    renderFixture(f0, sphere, sr::transformVS, sr::blinnPhongPerPixelFS, rig0, mat, s1);
    renderFixture(b0, sphere, sr::transformVS, sr::blinnPhongPerPixelFS, rig0, noSpec, s2);
    const SpecMap spec0(f0, b0);
    const sr::Vec2 c0 = spec0.centroid();
    const sr::Vec2 want0 = pixelOf(sr::normalize(rig0.eyeWorld));   // S0=(0,0,1) ⇒ (160,90)
    std::printf("[lit7c] 峰0=(%d,%d) 期望 (%.2f,%.2f)；重心0=(%.2f,%.2f)\n",
                spec0.peakX, spec0.peakY,
                static_cast<double>(want0.x), static_cast<double>(want0.y),
                static_cast<double>(c0.x), static_cast<double>(c0.y));
    // 峰位容差 2.5px 的推导：峰像素中心与解析 S 的投影差 ≤0.5px（像素量化）+
    // 网格离散化（最近顶点 6° 外，但逐像素 FS 在每个像素重算 H ⇒ 峰不受顶点位置
    // 约束，残差来自球面折线近似轮廓，<1px）+ 量化对峰位的影响 ≤1px。实测 1.4px。
    // 2.5px 留余量，又远小于 #14 变异的 ~4-5px 偏移（红得干脆）。
    SR_ASSERT(std::fabs(static_cast<float>(spec0.peakX) + 0.5f - want0.x) < 2.5f);
    SR_ASSERT(std::fabs(static_cast<float>(spec0.peakY) + 0.5f - want0.y) < 2.5f);

    // 只改 eyeWorld（点光跟随，保持头灯）：峰与重心都移到新的解析位置。相机/几何不动。
    sr::LightRig rig1 = rig0;
    const float a = 0.35f;   // 绕 Y 转 0.35rad：S 的投影 x 移 ≈13.2px（透视放大，见下）
    rig1.eyeWorld = sr::Vec3{sr::kGoldenCamZ * std::sin(a), 0.0f, sr::kGoldenCamZ * std::cos(a)};
    rig1.pointPosWorld = rig1.eyeWorld;
    Canvas f1{kW, kH}, b1{kW, kH};
    sr::FrameStats s3, s4;
    renderFixture(f1, sphere, sr::transformVS, sr::blinnPhongPerPixelFS, rig1, mat, s3);
    renderFixture(b1, sphere, sr::transformVS, sr::blinnPhongPerPixelFS, rig1, noSpec, s4);
    const SpecMap spec1(f1, b1);
    const sr::Vec2 c1 = spec1.centroid();
    const sr::Vec2 want1 = pixelOf(sr::normalize(rig1.eyeWorld));
    const float peakMoved = std::fabs(static_cast<float>(spec1.peakX - spec0.peakX)) +
                            std::fabs(static_cast<float>(spec1.peakY - spec0.peakY));
    const float centroidMoved = std::fabs(c1.x - c0.x) + std::fabs(c1.y - c0.y);
    std::printf("[lit7c] 峰1=(%d,%d) 期望 (%.2f,%.2f)；峰位移 %.1fpx 重心位移 %.2fpx\n",
                spec1.peakX, spec1.peakY,
                static_cast<double>(want1.x), static_cast<double>(want1.y),
                static_cast<double>(peakMoved), static_cast<double>(centroidMoved));
    SR_ASSERT(std::fabs(static_cast<float>(spec1.peakX) + 0.5f - want1.x) < 2.5f);
    SR_ASSERT(std::fabs(static_cast<float>(spec1.peakY) + 0.5f - want1.y) < 2.5f);
    // 位移下限推导：want1.x−want0.x = 13.2px（解析投影，0.35rad 的弦在 d=5、
    // fov60°、180px 高下被透视放大）；峰位移实测 13px，10px 留 3px 余量。
    // 重心位移实测 ~10.2px（点瓣移 13px、key 瓣随 V 移 ~7px，加权平均），8px 下限。
    SR_ASSERT(peakMoved > 10.0f);
    SR_ASSERT(centroidMoved > 8.0f);

    // 漫反射不随眼：点光**固定**在 E0、只动 eyeWorld ⇒ Ks=0 帧逐字节不变
    //（shadeDiffuseOnly 无视点项）。这是"视点相关 vs 光向相关"的分离钉。
    sr::LightRig rigEyeOnly = rig0;
    rigEyeOnly.eyeWorld = rig1.eyeWorld;   // pointPosWorld 保持 E0
    Canvas d0{kW, kH}, d1{kW, kH};
    sr::FrameStats s5, s6;
    renderFixture(d0, sphere, sr::transformVS, sr::blinnPhongPerPixelFS, rig0, noSpec, s5);
    renderFixture(d1, sphere, sr::transformVS, sr::blinnPhongPerPixelFS, rigEyeOnly, noSpec, s6);
    int diffuseDiff = 0;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x)
            if (d0.rt.color.readPacked(x, y) != d1.rt.color.readPacked(x, y)) ++diffuseDiff;
    SR_ASSERT_EQ(diffuseDiff, 0);

    // 只改 keyDir ⇒ 漫反射重心移动（光向相关）。key 瓣高光峰虽然也随 keyDir 动，
    // 但"高光重心随 keyDir"的可测部分在这里是漫反射：key 瓣面积小（同 Ns 但
    // 强度低 14%），且本断言要分离的是"光向相关"这一事实本身。两个方向是
    // **不同的合法值**（Ruling 67：{-0.3,-0.8,-0.6} → {-0.8,-0.3,-0.6}）。
    sr::LightRig rigKey = rig0;
    rigKey.keyDirWorld = sr::normalize(sr::Vec3{-0.8f, -0.3f, -0.6f});
    Canvas d2{kW, kH};
    sr::FrameStats s7;
    renderFixture(d2, sphere, sr::transformVS, sr::blinnPhongPerPixelFS, rigKey, noSpec, s7);
    const sr::Vec2 dc0 = diffuseCentroid(d0);
    const sr::Vec2 dc2 = diffuseCentroid(d2);
    const float dMoved = std::fabs(dc2.x - dc0.x) + std::fabs(dc2.y - dc0.y);
    std::printf("[lit7c] 漫反射重心 (%.2f,%.2f) → (%.2f,%.2f)，keyDir 改变位移 %.2fpx\n",
                static_cast<double>(dc0.x), static_cast<double>(dc0.y),
                static_cast<double>(dc2.x), static_cast<double>(dc2.y),
                static_cast<double>(dMoved));
    // 下限 1px：key 方向改了 ~25°，照亮半球跟着转，重心实测移 4.4px；
    // 1px 是量化抖动（重心对逐像素 ±0.00446 的响应 <0.1px）的 10 倍。
    SR_ASSERT(dMoved > 1.0f);
}

// ---- #8：三角形边上的插值 kink，Gouraud 更强 -----------------------------------

SR_TEST(interpolation_kink_at_triangle_edges_is_stronger_under_gouraud) {
    // §8.2 #8（改名后的措辞）：沿指定扫描线取二阶差分，比"Gouraud 在三角形边界处的
    // kink 相对局部斜率"与逐像素图。**不叫 Mach band**：那是感知的过冲（侧抑制），
    // 是图要说明的东西；二阶差分能测的是一阶导的不连续（notes 用图说明感知部分）。
    // 逐像素 Blinn-Phong 的法线在球面网格上也只是 C0 ⇒ 两边都有 kink，断言只比强弱。
    // 度量喂的是**高光-only**行（full − Ks=0）：漫反射底在两种图里都光滑弯曲，
    // 会稀释比值；高光带才是"顶点凸组合 vs 逐像素重算"差别最大的地方。
    const sr::Mesh sphere = sr::makeUVSphere(1.0f, sr::kGoldenLitStacks, sr::kGoldenLitSlices);
    const sr::MaterialShade mat = sr::litDefaultMaterialShade();
    sr::MaterialShade noSpec = mat;
    noSpec.specular = sr::LinearColor{0.0f, 0.0f, 0.0f};
    const sr::LightRig& rig = sr::goldenLitRig();
    Canvas pf{kW, kH}, pb{kW, kH}, gf{kW, kH}, gb{kW, kH};
    sr::FrameStats s1, s2, s3, s4;
    renderFixture(pf, sphere, sr::transformVS, sr::blinnPhongPerPixelFS, rig, mat, s1);
    renderFixture(pb, sphere, sr::transformVS, sr::blinnPhongPerPixelFS, rig, noSpec, s2);
    renderFixture(gf, sphere, sr::lambertVS, sr::lambertPerVertexFS, rig, mat, s3);
    renderFixture(gb, sphere, sr::lambertVS, sr::lambertPerVertexFS, rig, noSpec, s4);
    // 指定扫描线 = Phong **全帧**最亮行（key 瓣峰所在，y≈75）：那里 Gouraud 的
    // 顶点摊开最显著。度量用全帧亮度而不是高光-only：spec 相减把量化噪声翻倍
    //（两张量化帧之差），淹没 Gouraud 内部的"仿射 ⇒ 二阶差分≈0"信号（实测
    // spec-only 比值 1.05 vs 0.76，分不开；全帧 3.6 vs 2.1）。
    int y0 = kH / 2;
    float best = -1.0f;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            if (!(pf.rt.depth.read(x, y) < 1.0f)) continue;
            const float v = sr::decodeSRGB(pf.rt.color.readPacked(x, y)).r;
            if (v > best) { best = v; y0 = y; }
        }
    }
    int xMin = kW, xMax = -1;
    for (int x = 0; x < kW; ++x) {
        if (pf.rt.depth.read(x, y0) < 1.0f) {
            if (x < xMin) xMin = x;
            if (x > xMax) xMax = x;
        }
    }
    float LP[kW], LG[kW];
    for (int x = 0; x < kW; ++x) {
        LP[x] = sr::decodeSRGB(pf.rt.color.readPacked(x, y0)).r;
        LG[x] = sr::decodeSRGB(gf.rt.color.readPacked(x, y0)).r;
    }
    const std::vector<float> edges = edgeCrossings(sphere, y0);
    const KinkMetrics mP = kinkMetrics(LP, xMin, xMax, edges);
    const KinkMetrics mG = kinkMetrics(LG, xMin, xMax, edges);
    std::printf("[lit8] 扫描线 y=%d，边交点 %zu 个；P ratio=%.3f int=%.5f sn=%.3f；"
                "G ratio=%.3f int=%.5f sn=%.3f\n",
                y0, edges.size(),
                static_cast<double>(mP.ratio), static_cast<double>(mP.interior),
                static_cast<double>(mP.slopeNorm),
                static_cast<double>(mG.ratio), static_cast<double>(mG.interior),
                static_cast<double>(mG.slopeNorm));
    SR_ASSERT(mP.ratio > 0.0f);
    SR_ASSERT(mG.ratio > 0.0f);
    SR_ASSERT(edges.size() >= 4);   // 扫描线真的穿过多个三角形（不是空夹具）
    // 阈值推导（12/15 夹具、y=75 实测，MSVC Debug 确定性渲染 ⇒ 数值可复现）：
    //   ratio：P=2.034 G=3.022 ⇒ G/P=1.486，阈值 1.4（6% 余量）；
    //   interior：P=0.00452 G=0.00254 ⇒ P/G=1.78，阈值 1.5（18% 余量）——
    //     Gouraud 内部仿射 ⇒ D2 只剩量化噪声，这是"摊开"的直接证据；
    //   slopeNorm：P=0.430 G=0.555 ⇒ G/P=1.29，阈值 1.2（7% 余量）——
    //     设计原话"kink 相对局部斜率"。
    // 三个视角测同一事实；任一 Gouraud↔逐像素退化变异都让三者同时坍到 1。
    // 选行规则是**原则性的**（全帧最亮行 = key 瓣峰行，Gouraud 摊开最显眼处），
    // 不是逐行挑最大值：实测相邻行（73/74/76）分离度 1.7-1.8×，同量级。
    SR_ASSERT(mG.ratio >= 1.4f * mP.ratio);
    SR_ASSERT(mP.interior >= 1.5f * mG.interior);
    SR_ASSERT(mG.slopeNorm >= 1.2f * mP.slopeNorm);
}

#else   // !_MSC_VER

// C1′：以上每条都含逐像素 pow 的渲染 ⇒ g++ 侧逐条可见 [skip]。
// 用一组同名用例打印 skip，保证 g++ 侧的测试计数与 MSVC 侧一致（不是静默消失）。
SR_TEST(lit_golden_is_deterministic) {
    std::printf("[skip] lit golden 只在 MSVC 判定（pow，C1′）\n");
}
SR_TEST(lit_golden_has_a_highlight_that_does_not_saturate) {
    std::printf("[skip] lit golden 只在 MSVC 判定（pow，C1′）\n");
}
SR_TEST(lit_golden_frame_is_not_the_m3_or_m4_frame) {
    std::printf("[skip] lit golden 只在 MSVC 判定（pow，C1′）\n");
}
SR_TEST(gouraud_smears_the_specular_peak_that_per_pixel_localizes) {
    std::printf("[skip] lit golden 只在 MSVC 判定（pow，C1′）\n");
}
SR_TEST(the_per_pixel_highlight_center_moves_with_the_eye) {
    std::printf("[skip] lit golden 只在 MSVC 判定（pow，C1′）\n");
}
SR_TEST(interpolation_kink_at_triangle_edges_is_stronger_under_gouraud) {
    std::printf("[skip] lit golden 只在 MSVC 判定（pow，C1′）\n");
}

#endif  // _MSC_VER
