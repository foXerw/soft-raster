#include "render/builtin_shaders.h"

#include "render/shading.h"

namespace sr {
namespace {

// MVP + 非光照 varying 的**唯一一份**实现（M6.3；M5 lex.h 判例：逐字重复的逻辑块
// 必须唯一一份）。transformVS 与 lambertVS 共用它，光照的差别发生在这之后、
// 各自对 v.color 槽的写入上（D6：不新增槽位）。
// 表达式顺序与原 transformVS 逐字一致 —— 两份 golden 是零容差比对，
// 改动结合/求值顺序就是改字节（M6.3 的验收正是逐字节不变）。
ClipVertex clipFromPosition(const VSIn& in, const VertexUniforms& u) {
    const Vec4 clip = u.proj * (u.view * (u.model * makeVec4(in.position, 1.0f)));

    Varyings v;
    v.color = in.color;
    // 法线走 normalMatrix（makeNormalMatrix 产出的逆转置），且归一化在乘法**之后** ——
    // scale(2,1,1) 下变换前归一化等于没归一化：乘法会重新改变长度（D4，M6.0）。
    v.worldNormal = normalize(xyz(u.normalMatrix * makeVec4(in.normal, 0.0f)));
    v.uv = in.uv;
    // worldPos 是真世界空间（M6.0）。此前这里错赋成物体空间的 in.position，
    // 而没有任何 FS 读它，所以错了也没人发现 —— 本章主题就是这个失效模式。
    v.worldPos = xyz(u.model * makeVec4(in.position, 1.0f));

    ClipVertex out;
    out.clip = clip;
    out.v = v;   // 未除 w：裁剪对它线性（见 vertex.h）
    return out;
}

}  // namespace

ClipVertex transformVS(const VSIn& in, const VertexUniforms& u) {
    return clipFromPosition(in, u);
}

bool flatColorFS(const FragmentInput&, const FragmentUniforms& u, FragmentOutput& out) {
    out.color = u.baseColor;
    return true;
}

bool vertexColorFS(const FragmentInput& in, const FragmentUniforms&, FragmentOutput& out) {
    out.color = LinearColor{in.v.color.x, in.v.color.y, in.v.color.z};
    return true;
}

bool texturedFS(const FragmentInput& in, const FragmentUniforms& u, FragmentOutput& out) {
    if (u.texture == nullptr) {
        out.color = u.baseColor;
        return true;
    }
    // uv 已经是透视正确插值的结果（M3 的 attr/w 那一套），所以贴图在斜视角下不会扭曲 ——
    // 那正是 spec §8 M4 行"旋转不扭曲"的含义。
    // gradU/gradV 直接来自 TriangleSetup 的 dudx/dudy/dvdx/dvdy（见 pipeline.h 的 Ruling B）。
    const TexelF c = sample(*u.texture, u.sampler, in.v.uv.x, in.v.uv.y, in.gradU, in.gradV);
    out.color = LinearColor{c.r * u.baseColor.r, c.g * u.baseColor.g, c.b * u.baseColor.b};
    return true;
}

ClipVertex lambertVS(const VSIn& in, const VertexUniforms& u) {
    ClipVertex out = clipFromPosition(in, u);
    // lights == nullptr ⇒ unlit（D15 的早退语义）：v.color 保持 in.color，别写死白。
    // mat == nullptr 同样早退：没有材质可读，不发明默认材质（不是第四种模式、不加 bool）。
    if (u.lights == nullptr || u.mat == nullptr) return out;
    // D17：**有效** albedo = 顶点色 × Kd（两者都已是线性）。这兑现 main.cpp 老路径
    // 那句"M6 的光照 shader 还要它"的承诺：顶点色从此有真消费者。
    // VS 不采样贴图（也绝不为它加采样）—— 顶点色是它唯一的逐顶点 albedo 细节来源。
    // 注意 Kd 的乘法在 shadeBlinnPhong **内部**（shadeDiffuseOnly 的 kd = cmul(albedo,
    // mat.diffuse)，M6.2 的契约，test_shading.cpp 钉着）：这里只传顶点色。
    // 若按简报字面再预乘一次 Kd，结果是 Kd²（0.5×0.5×0.5=0.125 而不是 0.25）——
    // 简报那行公式描述的是有效 albedo，不是实参（M6.3 实施记录注明了这处出入）。
    const LinearColor albedo{in.color.x, in.color.y, in.color.z};
    // Ruling 91：**完整**光照（含高光），不是 shadeDiffuseOnly。教科书 Gouraud =
    // 在顶点处算完整光照再插值结果；没有高光的 Gouraud 没有高光带可摊开，
    // §8.2 #7b 会退化成"没有 vs 有"而不是"摊开 vs 收紧"。
    // N/P 用 clipFromPosition 填好的世界空间值（N 已归一化 —— shading.h 的边界，M6.0 的责任）。
    const LinearColor lit =
        shadeBlinnPhong(out.v.worldNormal, out.v.worldPos, *u.lights, *u.mat, albedo);
    // D6：写进已有的 v.color 槽（rgb = 线性光色），kVaryingFloatCount 仍是 12。
    out.v.color = makeVec4(Vec3{lit.r, lit.g, lit.b}, 1.0f);
    return out;
}

bool lambertPerVertexFS(const FragmentInput& in, const FragmentUniforms&, FragmentOutput& out) {
    // 直读插值后的顶点光照结果，**一行**。不重算光照（那是 M6.4 的逐像素模式），
    // 也不读 texture/baseColor（D17：Gouraud 下贴图"消失"是设计，解释见 builtin_shaders.h）。
    out.color = LinearColor{in.v.color.x, in.v.color.y, in.v.color.z};
    return true;
}

}  // namespace sr
