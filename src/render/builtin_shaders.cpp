#include "render/builtin_shaders.h"

namespace sr {

ClipVertex transformVS(const VSIn& in, const VertexUniforms& u) {
    const Vec4 clip = u.proj * (u.view * (u.model * makeVec4(in.position, 1.0f)));

    Varyings v;
    v.color = in.color;
    // 法线用 normalMatrix（M6 才有非均匀缩放，但矩阵已经准备好了）
    v.worldNormal = xyz(u.normalMatrix * makeVec4(in.normal, 0.0f));
    v.uv = in.uv;
    v.worldPos = in.position;

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
