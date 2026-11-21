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

}  // namespace sr
