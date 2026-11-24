#include "render/builtin_shaders.h"

namespace sr {

ClipVertex transformVS(const VSIn& in, const VertexUniforms& u) {
    const Vec4 clip = u.proj * (u.view * (u.model * makeVec4(in.position, 1.0f)));

    Varyings v;
    v.color = in.color;
    // transformVS 不加光照（范围），但这两行必须改（D4）：
    // 法线走 normalMatrix（makeNormalMatrix 产出的逆转置），且归一化在乘法**之后** ——
    // scale(2,1,1) 下变换前归一化等于没归一化：乘法会重新改变长度（D4）。
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

}  // namespace sr
