#pragma once
// M6.2：光照本体 —— 两个自由纯函数，可解析单测（D20：先有可测的数学，再有 shader）。
// FS/VS 只是薄封装（M6.3/M6.4 接线）；点积、半向量、衰减、pow 全在这里，
// 于是"整帧里找一个满足条件的像素"类不可能夹具（§14 第 7 条）都换成解析断言。
//
// 边界（简报原话）：N 必须是**已归一化**的世界空间法线，函数不替调用方归一化
// （归一化是 M6.0 的责任）。零向量约定（vec.h:67-72）下退化法线 N=(0,0,0) ⇒
// N·L=0 ⇒ 只剩 ambient：暗面不是黑洞，也不是 NaN。
// P 只用于 L_point = normalize(pointPosWorld - P)、V = normalize(eyeWorld - P) 与衰减距离 d。
// C5：全部 const& 收参、无分配、无虚派发、无 std::function —— 之后会被逐像素调用。
#include <cmath>

#include "math/vec.h"
#include "render/color.h"
#include "render/lights.h"

namespace sr {
namespace shading_detail {

// ---- 最小颜色代数（LinearColor 是纯数据，color.h 不带算子）----
inline LinearColor cmul(const LinearColor& a, const LinearColor& b) {
    return {a.r * b.r, a.g * b.g, a.b * b.b};
}
inline LinearColor cscale(const LinearColor& c, float s) { return {c.r * s, c.g * s, c.b * s}; }
inline LinearColor cadd(const LinearColor& a, const LinearColor& b) {
    return {a.r + b.r, a.g + b.g, a.b + b.b};
}

// 每盏灯的几何：朝光源方向、钳过的 N·L、衰减系数。
// **L_key / L_point / d / N·L 的唯一一份实现**（M5 lex.h 判例：逐字重复的逻辑块必须唯一一份）；
// 漫反射核心与高光支都从这里取，不许各算各的。
struct LightGeom {
    Vec3 L;      // 朝光源方向（单位向量；P 与点光重合时为零向量，normalize 的约定）
    float ndl;   // max(0, N·L)：背光钳到 0，绝不为负
    float att;   // 衰减系数（关闭时恒 1）
};

inline LightGeom keyGeom(const Vec3& N, const LightRig& rig) {
    // keyDirWorld 是光"照向"的方向（lights.h:15）⇒ 朝光源的方向取反。
    const Vec3 L = -rig.keyDirWorld;
    const float d = dot(N, L);
    return {L, d > 0.0f ? d : 0.0f, 1.0f};
}

inline LightGeom pointGeom(const Vec3& N, const Vec3& P, const LightRig& rig) {
    const Vec3 toLight = rig.pointPosWorld - P;
    const float d = length(toLight);
    const Vec3 L = normalize(toLight);
    const float ndl = dot(N, L);
    float att = 1.0f;
    if (rig.attenuate) {
        // §6.8 D7：1/(1+(d/r)²)。radius > 0 由调用方保证（默认 = 场景半径 ×3，M6.6）。
        const float t = d / rig.attenuateRadius;
        att = 1.0f / (1.0f + t * t);
    }
    return {L, ndl > 0.0f ? ndl : 0.0f, att};
}

// 一盏灯的 Blinn-Phong 高光。ns 已由调用方夹到 [1,4096] 且 > 0（D3）。
inline LinearColor specForLight(const Vec3& N, const LightGeom& g, const Vec3& V,
                                const LinearColor& lightColor, float intensity,
                                const LinearColor& ks, float ns) {
    // 背光门：N·L <= 0 的灯根本不照这个面，高光也必须为 0。
    // 没有这道门，未照亮的背面会出现高光斑 —— 那是"我的法线翻错了"的经典假阳性
    // （Review Focus：backfacing surfaces）。
    if (!(g.ndl > 0.0f)) return {0.0f, 0.0f, 0.0f};
    const Vec3 H = normalize(g.L + V);   // 半向量：必须归一化，N·H 才是 cosθ
    const float ndh = dot(N, H);
    // ndh <= 0 无贡献；同时挡住 powf(负数, 非整数) 产生 NaN。
    if (!(ndh > 0.0f)) return {0.0f, 0.0f, 0.0f};
    return cscale(cmul(ks, lightColor), intensity * g.att * std::pow(ndh, ns));
}

}  // namespace shading_detail

// ambient + 漫反射的核心。**被 shadeBlinnPhong 组合调用**（Ruling 91/D25）：
// 它不是"Gouraud 那一支" —— 教科书 Gouraud 在顶点算**完整**光照再插值结果；
// 没有高光的 Gouraud 没有高光带可摊开，§8.2 #7b 会退化成"没有 vs 有"的空转对照。
// 这一支的存在是为了让 N·L 只有一份实现，且组合关系给 shadeDiffuseOnly 一个真消费者
// （本章的论点：不许有休眠函数）。
inline LinearColor shadeDiffuseOnly(const Vec3& N, const Vec3& P, const LightRig& rig,
                                    const MaterialShade& mat, const LinearColor& albedo) {
    using namespace shading_detail;
    const LightGeom key = keyGeom(N, rig);
    const LightGeom point = pointGeom(N, P, rig);
    const LinearColor kd = cmul(albedo, mat.diffuse);
    // D9：ambient = Ka × 环境光色。背光面/退化法线上唯一剩下的光（暗，不是黑洞）。
    LinearColor out = cmul(rig.ambient, mat.ambient);
    out = cadd(out, cscale(cmul(kd, rig.keyColor), rig.keyIntensity * key.ndl));
    out = cadd(out, cscale(cmul(kd, rig.pointColor), rig.pointIntensity * point.ndl * point.att));
    return out;
}

// 完整光照 = shadeDiffuseOnly + Blinn-Phong 高光（Ruling 91 的组合等式）。
// lambertVS 与 blinnPhongPerPixelFS 都用这一支（M6.3/M6.4）。
inline LinearColor shadeBlinnPhong(const Vec3& N, const Vec3& P, const LightRig& rig,
                                   const MaterialShade& mat, const LinearColor& albedo) {
    using namespace shading_detail;
    LinearColor out = shadeDiffuseOnly(N, P, rig, mat, albedo);
    // D3/Ruling 90：M6.1 原样存 shininess（含 0），夹取与 0 判断**只在这里**（一处规则）。
    // Ns == 0（以及负数/NaN）⇒ 无高光，整个 pow 跳过 —— powf(x,0)==1 会给整个表面镀一层
    // 均匀白，症状读起来像"材质参数没生效"。`> 0.0f` 的写法对 NaN 天然安全。
    if (mat.shininess > 0.0f) {
        const float ns = mat.shininess < 1.0f ? 1.0f
                       : (mat.shininess > 4096.0f ? 4096.0f : mat.shininess);
        const Vec3 V = normalize(rig.eyeWorld - P);
        // 几何帮手再求值一次（几个点积/sqrt，C5 无碍）；公式本身只有上面那一份。
        const LightGeom key = keyGeom(N, rig);
        const LightGeom point = pointGeom(N, P, rig);
        out = cadd(out, specForLight(N, key, V, rig.keyColor, rig.keyIntensity, mat.specular, ns));
        out = cadd(out, specForLight(N, point, V, rig.pointColor, rig.pointIntensity, mat.specular, ns));
    }
    return out;
}

}  // namespace sr
