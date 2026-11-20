#pragma once

#include <cstdint>
#include <vector>

#include "math/bbox.h"
#include "math/vec.h"

namespace sr {

// SoA + 索引。SoA 的理由：属性插值与 tile 分箱对"同一种属性连续存放"更友好，
// 而且 M9 的 SIMD 可以一次取 4 个同名字段。代价是顶点访问要写四个下标，
// 所以约定写在这里：indices 每 3 个一组是一个三角形，逆时针（NDC 约定）。
struct Mesh {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> uvs;
    std::vector<uint32_t> indices;
    BBox bounds;

    void recomputeBounds();
    float boundRadius() const { return bounds.radius(); }
    Vec3 boundCenter() const { return bounds.center(); }
    int triangleCount() const { return static_cast<int>(indices.size() / 3); }
};

Mesh makeCube(float size, bool perFaceNormals);
Mesh makeUVSphere(float radius, int stacks, int slices, bool duplicatedPoleVerts = true);
Mesh makeTorus(float majorRadius, float minorRadius, int ringSegments, int sideSegments);

}  // namespace sr
