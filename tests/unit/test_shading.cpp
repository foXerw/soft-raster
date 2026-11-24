// M6.2：shading.h 光照本体的解析单测（D20：先有可测的数学，再有 shader）。
// 所有夹具都是"给定向量/标量 ⇒ 断言闭式期望"，不存在"整帧里找一个满足条件的像素"。
//
// 命名：简报用例名原样保留，但统一加 shading_ 前缀 —— 任务验收命令是
// `--only shading_`（harness 按用例名子串过滤，harness.cpp:44-47），无前缀则一条都选不中。
//
// 夹具里两处与简报字面不同的地方（数学核对后修正，报告里注明）：
//  1) Step 3 的 "P == pointPosWorld + N*dist ⇒ N·L == 1" 符号反了：
//     L_point = normalize(pointPosWorld - P)，P 在光源**上方** N*dist 处时 L = -N ⇒ N·L = -1。
//     N·L == 1 的正确夹具是 P = pointPosWorld - N*dist（表面在光源下方、法线朝向光源）。
//  2) Step 7 建议的 `SR_ASSERT(!(v > 0.0f || v < 0.0f))` 作为断言**不检测 NaN**：
//     NaN 时 (false||false) ⇒ !false = true ⇒ 通过；非零有限值反而红。
//     那是生产代码里守卫条件的写法（plane_eq.h:9-14 的 `!(...)` 守卫），不是断言。
//     这里用同风格的比较式断言 `v > 0 || v < 0 || v == 0`（NaN 三者全假 ⇒ 红）+ std::isfinite
//     （test_plane_eq.cpp:37 的既有测试风格）。
#include <cmath>

#include "harness.h"
#include "math/vec.h"
#include "render/color.h"
#include "render/lights.h"
#include "render/shading.h"

namespace {

using sr::LinearColor;
using sr::LightRig;
using sr::MaterialShade;
using sr::Vec3;

void expectColorNear(const LinearColor& c, float r, float g, float b, float tol) {
    SR_ASSERT_NEAR(c.r, r, tol);
    SR_ASSERT_NEAR(c.g, g, tol);
    SR_ASSERT_NEAR(c.b, b, tol);
}

// 基础 rig：白色方向光沿 +y 照来（keyDirWorld 是"照向"，取反），点光默认关（intensity=0，D24），
// ambient 默认 0。各用例按需打开/改动字段，改动处都有注释。
LightRig baseRig() {
    LightRig rig;
    rig.keyDirWorld = {0.0f, -1.0f, 0.0f};   // 照向 -y ⇒ 朝光源方向 L_key = (0,1,0)
    rig.keyColor = {1.0f, 1.0f, 1.0f};
    rig.keyIntensity = 1.0f;
    rig.pointPosWorld = {0.0f, 0.0f, 0.0f};
    rig.pointColor = {1.0f, 1.0f, 1.0f};
    rig.pointIntensity = 0.0f;               // 关：把方向光项隔离出来
    rig.attenuate = false;
    rig.attenuateRadius = 10.0f;
    rig.ambient = {0.0f, 0.0f, 0.0f};
    rig.eyeWorld = {0.0f, 5.0f, 0.0f};
    return rig;
}

MaterialShade baseMat() {
    MaterialShade mat;
    mat.diffuse = {1.0f, 1.0f, 1.0f};
    mat.ambient = {0.0f, 0.0f, 0.0f};
    mat.specular = {0.0f, 0.0f, 0.0f};
    mat.shininess = 0.0f;
    return mat;
}

// NaN 检测（比较式，plane_eq.h 守卫的同款思路）：NaN 与任何数比较都是 false。
void expectNotNaN(float v) {
    SR_ASSERT(v > 0.0f || v < 0.0f || v == 0.0f);
    SR_ASSERT(std::isfinite(v));
}

}  // namespace

// Step 1：方向光漫反射 = albedo × Kd × intensity × max(0, N·L)，逐通道。
SR_TEST(shading_directional_lambert_is_albedo_times_NdotL_times_intensity) {
    LightRig rig = baseRig();          // 点光关、ambient 0 ⇒ 输出只剩方向光项
    rig.keyIntensity = 0.7f;
    MaterialShade mat = baseMat();
    mat.diffuse = {0.9f, 0.7f, 0.5f};
    const LinearColor albedo{0.8f, 0.6f, 0.4f};   // 三通道互异：通道对调变异必须红
    const Vec3 N{0.0f, 1.0f, 0.0f};
    const Vec3 P{0.0f, 0.0f, 0.0f};

    // L = (0,1,0) ⇒ N·L = 1
    rig.keyDirWorld = {0.0f, -1.0f, 0.0f};
    LinearColor c = sr::shadeDiffuseOnly(N, P, rig, mat, albedo);
    expectColorNear(c, 0.8f * 0.9f * 0.7f, 0.6f * 0.7f * 0.7f, 0.4f * 0.5f * 0.7f, 1e-6f);

    // L = (0,0,1) ⇒ N·L = 0
    rig.keyDirWorld = {0.0f, 0.0f, -1.0f};
    c = sr::shadeDiffuseOnly(N, P, rig, mat, albedo);
    expectColorNear(c, 0.0f, 0.0f, 0.0f, 1e-6f);

    // L = (0,-1,0) ⇒ N·L = -1 ⇒ **钳到 0 而不是负数**
    rig.keyDirWorld = {0.0f, 1.0f, 0.0f};
    c = sr::shadeDiffuseOnly(N, P, rig, mat, albedo);
    expectColorNear(c, 0.0f, 0.0f, 0.0f, 1e-6f);
    SR_ASSERT(c.r >= 0.0f && c.g >= 0.0f && c.b >= 0.0f);
}

// Step 2：ambient = Ka × 环境光色（D9），且是背光面上唯一的光（暗面不是黑洞）。
// 同时钉住：高光不得照亮背光面（Ns=1 让漏过去的高光足够大，容差内看得见）。
SR_TEST(shading_ambient_term_uses_Ka_and_is_the_only_light_on_a_backfacing_surface) {
    LightRig rig = baseRig();
    rig.ambient = {0.3f, 0.2f, 0.1f};
    // L_key = (√3/2, -1/2, 0)：与 N=(0,1,0) 成 120° ⇒ N·L = -0.5 < 0（背光，但不是正相反，
    // 正相反时 N·H 恒 ≤ 0，去掉高光门也看不见 —— 夹具必须让"漏光"可观察）。
    rig.keyDirWorld = {-std::sqrt(3.0f) / 2.0f, 0.5f, 0.0f};
    rig.keyIntensity = 1.0f;
    rig.pointPosWorld = {0.0f, -5.0f, 0.0f};   // 点光也在背面：L_point = (0,-1,0)
    rig.pointIntensity = 0.8f;
    rig.eyeWorld = {0.0f, 1.0f, 0.0f};         // V = (0,1,0) ⇒ H_key = (√3/2, 1/2, 0)，N·H = 0.5 > 0
    MaterialShade mat = baseMat();
    mat.ambient = {0.5f, 0.4f, 0.3f};          // Ka ≠ Kd：Ka↔Kd 对调变异必须红
    mat.diffuse = {0.9f, 0.7f, 0.5f};
    mat.specular = {0.4f, 0.4f, 0.4f};
    mat.shininess = 1.0f;                      // 无高光门时 spec = 0.4×0.5 = 0.2，远大于容差
    const LinearColor albedo{1.0f, 1.0f, 1.0f};
    const Vec3 N{0.0f, 1.0f, 0.0f};
    const Vec3 P{0.0f, 0.0f, 0.0f};

    const LinearColor d = sr::shadeDiffuseOnly(N, P, rig, mat, albedo);
    expectColorNear(d, 0.15f, 0.08f, 0.03f, 1e-6f);
    SR_ASSERT(d.r > 0.0f);                     // 不是黑洞
    const LinearColor full = sr::shadeBlinnPhong(N, P, rig, mat, albedo);
    expectColorNear(full, 0.15f, 0.08f, 0.03f, 1e-6f);   // 高光被 N·L 门挡在背面之外
}

// Step 3：点光的 L 随表面位置变化；P 在光源正下方沿法线 dist 处 ⇒ N·L = 1。
SR_TEST(shading_point_light_direction_depends_on_the_surface_position) {
    LightRig rig = baseRig();
    rig.keyIntensity = 0.0f;                   // 关方向光，隔离点光
    rig.pointPosWorld = {0.0f, 8.0f, 0.0f};
    rig.pointIntensity = 0.8f;
    MaterialShade mat = baseMat();             // Kd = 1
    const LinearColor albedo{1.0f, 1.0f, 1.0f};
    const Vec3 N{0.0f, 1.0f, 0.0f};

    // P = pointPos - N*4（简报写的是 +N*dist，符号反了，见文件头注 1）⇒ L = N ⇒ N·L = 1。
    const Vec3 P1{0.0f, 4.0f, 0.0f};
    const LinearColor c1 = sr::shadeDiffuseOnly(N, P1, rig, mat, albedo);
    expectColorNear(c1, 0.8f, 0.8f, 0.8f, 1e-6f);

    // 同一 N、不同 P ⇒ L 不同 ⇒ 输出不同。P2 故意不在 y 轴上：
    // normalize(P) 与 normalize(pointPos - P) 在对称位置会碰巧同值，这里必须分开。
    const Vec3 P2{4.0f, 2.0f, 0.0f};
    const LinearColor c2 = sr::shadeDiffuseOnly(N, P2, rig, mat, albedo);
    const float expect2 = 0.8f * 6.0f / std::sqrt(52.0f);   // N·L = 6/√52
    expectColorNear(c2, expect2, expect2, expect2, 1e-5f);
    SR_ASSERT(std::fabs(c1.r - c2.r) > 0.01f);
}

// Step 4：衰减 = 1/(1+(d/r)²)（§6.8 D7），解析比对；关掉时三点同值。
SR_TEST(shading_attenuation_matches_the_formula_analytically) {
    LightRig rig = baseRig();
    rig.keyIntensity = 0.0f;
    rig.pointPosWorld = {3.0f, 7.0f, -2.0f};   // 不在原点：d=length(P) 的变异必须红
    rig.pointIntensity = 1.0f;
    rig.attenuate = true;
    rig.attenuateRadius = 10.0f;
    MaterialShade mat = baseMat();
    const LinearColor albedo{1.0f, 1.0f, 1.0f};
    const Vec3 N{0.0f, 1.0f, 0.0f};
    // P = pointPos - N*d ⇒ L_point = N ⇒ N·L = 1，输出 = intensity × att，att 可直接读出。
    const Vec3 P0{3.0f, 7.0f, -2.0f};          // d = 0
    const Vec3 Pnear{3.0f, 6.999f, -2.0f};     // d = 1e-3
    const Vec3 P10{3.0f, -3.0f, -2.0f};        // d = 10
    const Vec3 P30{3.0f, -23.0f, -2.0f};       // d = 30

    // d = 0：P 与光源重合 ⇒ L_point = normalize(0) = 0（vec.h 零向量约定）⇒ 点光项为 0。
    // att(0) = 1 在恰好 d=0 处**不可观测**（方向退化），由 d=1e-3 的采样与 10/30 的闭式值钉住。
    const LinearColor c0 = sr::shadeDiffuseOnly(N, P0, rig, mat, albedo);
    expectNotNaN(c0.r);
    expectColorNear(c0, 0.0f, 0.0f, 0.0f, 1e-6f);

    const LinearColor cn = sr::shadeDiffuseOnly(N, Pnear, rig, mat, albedo);
    expectColorNear(cn, 1.0f, 1.0f, 1.0f, 1e-5f);            // 1/(1+1e-8) ≈ 1
    const LinearColor c10 = sr::shadeDiffuseOnly(N, P10, rig, mat, albedo);
    expectColorNear(c10, 0.5f, 0.5f, 0.5f, 1e-5f);           // 1/(1+1)
    const LinearColor c30 = sr::shadeDiffuseOnly(N, P30, rig, mat, albedo);
    expectColorNear(c30, 0.1f, 0.1f, 0.1f, 1e-5f);           // 1/(1+9)

    // attenuate = false ⇒ 与距离无关，三点同值。
    rig.attenuate = false;
    const LinearColor un = sr::shadeDiffuseOnly(N, Pnear, rig, mat, albedo);
    const LinearColor u10 = sr::shadeDiffuseOnly(N, P10, rig, mat, albedo);
    const LinearColor u30 = sr::shadeDiffuseOnly(N, P30, rig, mat, albedo);
    expectColorNear(un, 1.0f, 1.0f, 1.0f, 1e-6f);
    expectColorNear(u10, 1.0f, 1.0f, 1.0f, 1e-6f);
    expectColorNear(u30, 1.0f, 1.0f, 1.0f, 1e-6f);
    SR_ASSERT_NEAR(un.r, u30.r, 1e-6f);
    SR_ASSERT_NEAR(u10.r, u30.r, 1e-6f);
}

// Step 5：Blinn-Phong 高光用半向量 H = normalize(L + V)，N = H 时取峰值 Ks × intensity。
SR_TEST(shading_specular_uses_the_half_vector_and_peaks_when_N_equals_H) {
    LightRig rig = baseRig();                  // L_key = (0,1,0)，点光关，ambient 0
    MaterialShade mat = baseMat();
    mat.diffuse = {0.0f, 0.0f, 0.0f};          // Kd = 0 ⇒ 输出 = 纯高光，闭式期望不被漫反射污染
    mat.specular = {0.35f, 0.35f, 0.35f};
    mat.shininess = 64.0f;
    const LinearColor albedo{1.0f, 1.0f, 1.0f};
    const Vec3 P{0.0f, 0.0f, 0.0f};

    // A：eye 在 (0,5,0) ⇒ V = (0,1,0) = L ⇒ H = (0,1,0)；N = H ⇒ N·H = 1 ⇒ 峰值 = Ks × 1。
    const Vec3 N{0.0f, 1.0f, 0.0f};
    const LinearColor peak = sr::shadeBlinnPhong(N, P, rig, mat, albedo);
    expectColorNear(peak, 0.35f, 0.35f, 0.35f, 1e-6f);       // pow(1,64)=1，两侧精确
    // 核心支不含高光（组合关系 Ruling 91 的另一半）
    const LinearColor core = sr::shadeDiffuseOnly(N, P, rig, mat, albedo);
    expectColorNear(core, 0.0f, 0.0f, 0.0f, 1e-6f);

    // B：N 转 30°（朝向 x）⇒ N·H = cos30° ⇒ 高光 = Ks × cos30°^64。
    // C1′ 实测常数：MSVC 19.40.33812.0（VS2022 17.10）Debug 实测 spec = 3.51583076e-05，
    // ratio = 0.000100452307；MSYS2 g++ 16.1.0（-ffp-contract=off）实测 9 位有效数字内同值。
    // 容差 1e-7（≈0.3% 相对）：比两侧实测差（<1e-13）宽 6 个数量级，又远小于值本身 ——
    // 不能用 1e-4 绝对容差，那比这个值还大 3 倍，Ns×2 变异（ratio²≈1e-8）都杀不死。
    const Vec3 N30{0.5f, std::sqrt(3.0f) / 2.0f, 0.0f};
    const LinearColor tilt = sr::shadeBlinnPhong(N30, P, rig, mat, albedo);
    expectColorNear(tilt, 3.51583076e-05f, 3.51583076e-05f, 3.51583076e-05f, 1e-7f);
    SR_ASSERT_NEAR(tilt.r / peak.r, 0.000100452307, 1e-7);

    // C：V ≠ L 的偏轴夹具 —— "V 误用光方向"的变异在 A/B 里不可见（V=L），必须在这里红。
    // eye=(5,5,0) ⇒ V=(√2/2,√2/2,0)，H 与 N 成 22.5° ⇒ 高光 = Ks × cos22.5°^64。
    // C1′ 实测常数：MSVC 19.40.33812.0 Debug 实测 0.00220521586；g++ 16.1.0 同值（9 位内）。
    // 容差 1e-7（≈0.005% 相对）；V←光方向变异给出 0.35，差 0.348 ≫ 容差。
    rig.eyeWorld = {5.0f, 5.0f, 0.0f};
    const LinearColor off = sr::shadeBlinnPhong(N, P, rig, mat, albedo);
    expectColorNear(off, 0.00220521586f, 0.00220521586f, 0.00220521586f, 1e-7f);
    SR_ASSERT(std::fabs(off.r - peak.r) > 1e-4f);   // 高光确实随视点动
}

// Step 6a：Ns = 0 ⇒ 无高光，且与 Ks = 0（Ns=64）逐通道相等（§8.2 #10）。
// 绝不写成"与 keyIntensity=0 相等"：两灯 rig 下那两个差的项不同（§14 第 8 条）。
SR_TEST(shading_Ns_zero_disables_specular_and_is_equivalent_to_Ks_zero) {
    LightRig rig = baseRig();                  // 峰值几何：N = L = V = (0,1,0)
    const Vec3 N{0.0f, 1.0f, 0.0f};
    const Vec3 P{0.0f, 0.0f, 0.0f};
    const LinearColor albedo{1.0f, 1.0f, 1.0f};
    MaterialShade mat = baseMat();
    mat.diffuse = {0.6f, 0.5f, 0.4f};          // 非零漫反射：Ns=0 必须保留它（不是整灯关掉）
    mat.specular = {0.35f, 0.35f, 0.35f};

    mat.shininess = 0.0f;
    const LinearColor ns0 = sr::shadeBlinnPhong(N, P, rig, mat, albedo);
    mat.shininess = 64.0f;
    mat.specular = {0.0f, 0.0f, 0.0f};
    const LinearColor ks0 = sr::shadeBlinnPhong(N, P, rig, mat, albedo);

    SR_ASSERT_NEAR(ns0.r, ks0.r, 1e-6f);
    SR_ASSERT_NEAR(ns0.g, ks0.g, 1e-6f);
    SR_ASSERT_NEAR(ns0.b, ks0.b, 1e-6f);
    // 绝对期望：两者都 = 纯漫反射。powf(x,0)==1 的白饼变异会在 ns0 上加 0.35 ⇒ 红。
    expectColorNear(ns0, 0.6f, 0.5f, 0.4f, 1e-6f);
    expectColorNear(ks0, 0.6f, 0.5f, 0.4f, 1e-6f);
}

// Step 6b：夹取到 [1,4096]（D3/Ruling 90，规则只在 shading.h 一处）。
SR_TEST(shading_shininess_is_clamped_before_the_pow) {
    LightRig rig = baseRig();
    const Vec3 P{0.0f, 0.0f, 0.0f};
    const LinearColor albedo{1.0f, 1.0f, 1.0f};
    MaterialShade mat = baseMat();
    mat.diffuse = {0.0f, 0.0f, 0.0f};
    mat.specular = {0.35f, 0.35f, 0.35f};

    // 上界：N·H = 0.9999（N 偏离 H 约 0.81°）。这个夹具是挑过的：
    // 0.9999^4096 ≈ 0.664 可观测，而 0.9999^1e6 ≈ 3.7e-44 下溢到 ~0 ——
    // 不夹取时两者差异巨大；若用 N·H = cos30°，两个指数都下溢到 0，夹取变异就杀不死。
    const float s = std::sqrt(1.0f - 0.9999f * 0.9999f);
    const Vec3 Nhi = sr::normalize(Vec3{s, 0.9999f, 0.0f});
    mat.shininess = 1e6f;
    const LinearColor big = sr::shadeBlinnPhong(Nhi, P, rig, mat, albedo);
    mat.shininess = 4096.0f;
    const LinearColor cap = sr::shadeBlinnPhong(Nhi, P, rig, mat, albedo);
    SR_ASSERT_NEAR(big.r, cap.r, 1e-6f);
    SR_ASSERT_NEAR(big.g, cap.g, 1e-6f);
    SR_ASSERT_NEAR(big.b, cap.b, 1e-6f);
    // 绝对期望钉住夹取目标**就是 4096**（只断言 big==cap 的话，两边同夹到 1024 也绿）。
    // C1′ 实测常数：MSVC 19.40.33812.0 Debug 实测 0.232349962（float 路径 N·H = 0.999899983，
    // 0.35 × pow(0.999899983, 4096)）；MSYS2 g++ 16.1.0 同值（9 位内）。
    // 容差 1e-6（≈4e-6 相对）：两侧 pow 差 <1e-9；不夹取 ⇒ ~0（差 0.232）、夹到 1024 ⇒ 0.316
    // （差 0.084），都远大于容差。注意 float 的 0.9999 与 double 差 ~1.7e-8，经 4096 次幂
    // 放大成 ~1.6e-5 的值差（实测 0.232349962 vs double 解析 0.232365757）——
    // 所以期望必须取 float 路径的实测值，不能用 double 解析值。
    expectColorNear(big, 0.232349962f, 0.232349962f, 0.232349962f, 1e-6f);

    // 下界：Ns = 0.5 夹到 1。N·H = 0.5（N 与 H 成 60°）：0.5^0.5 ≈ 0.707 ≠ 0.5^1。
    const Vec3 Nlo{std::sqrt(3.0f) / 2.0f, 0.5f, 0.0f};
    mat.shininess = 0.5f;
    const LinearColor frac = sr::shadeBlinnPhong(Nlo, P, rig, mat, albedo);
    mat.shininess = 1.0f;
    const LinearColor one = sr::shadeBlinnPhong(Nlo, P, rig, mat, albedo);
    SR_ASSERT_NEAR(frac.r, one.r, 1e-6f);
    SR_ASSERT_NEAR(frac.g, one.g, 1e-6f);
    SR_ASSERT_NEAR(frac.b, one.b, 1e-6f);
    expectColorNear(frac, 0.175f, 0.175f, 0.175f, 1e-6f);    // 0.35 × 0.5^1，精确
}

// Step 7：退化法线 N=(0,0,0)（零面积面/重合顶点经 normalize 的约定）⇒ 只剩 ambient，
// 有限、暗而不是黑洞、不含 NaN。
SR_TEST(shading_degenerate_normal_falls_back_to_ambient_not_black_or_nan) {
    LightRig rig = baseRig();
    rig.ambient = {0.3f, 0.2f, 0.1f};
    rig.keyIntensity = 1.0f;
    rig.pointPosWorld = {0.0f, 8.0f, 0.0f};
    rig.pointIntensity = 0.8f;
    MaterialShade mat = baseMat();
    mat.ambient = {0.5f, 0.4f, 0.3f};
    mat.specular = {0.35f, 0.35f, 0.35f};
    mat.shininess = 64.0f;
    const LinearColor albedo{0.8f, 0.6f, 0.4f};
    const Vec3 P{0.0f, 0.0f, 0.0f};
    // 前提：零向量约定本身（vec.h:67-72）—— N 就是 normalize 会给出的那个值。
    const Vec3 N = sr::normalize(Vec3{0.0f, 0.0f, 0.0f});
    SR_ASSERT(N.x == 0.0f && N.y == 0.0f && N.z == 0.0f);

    const LinearColor d = sr::shadeDiffuseOnly(N, P, rig, mat, albedo);
    const LinearColor full = sr::shadeBlinnPhong(N, P, rig, mat, albedo);
    expectNotNaN(d.r); expectNotNaN(d.g); expectNotNaN(d.b);
    expectNotNaN(full.r); expectNotNaN(full.g); expectNotNaN(full.b);
    expectColorNear(d, 0.15f, 0.08f, 0.03f, 1e-6f);
    expectColorNear(full, 0.15f, 0.08f, 0.03f, 1e-6f);
    SR_ASSERT(full.r > 0.0f);                  // 暗面不是黑洞
}
