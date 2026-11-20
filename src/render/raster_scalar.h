#pragma once
#include <cstdint>

#include "math/vec.h"
#include "render/render_target.h"

namespace sr {

struct FlatTriangle {
    Vec2 v[3];
    uint32_t packedColor = 0u;
};

struct RasterStats {
    int64_t pixelsCovered = 0;
    int64_t pixelsWritten = 0;
    int64_t trianglesRejected = 0;
};

void rasterizeFlatTriangle(ColorTarget& target, const FlatTriangle& tri, RasterStats* stats = nullptr);

}  // namespace sr
