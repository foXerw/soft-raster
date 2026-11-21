#include "render/raster_scalar.h"

#include "render/triangle_geom.h"

namespace sr {

void rasterizeFlatTriangle(ColorTarget& target, const FlatTriangle& tri, RasterStats* stats) {
    TriangleGeom g;
    if (!buildTriangleGeometry(tri.v, target.width, target.height, g)) {
        if (stats != nullptr) ++stats->trianglesRejected;
        return;
    }

    int written = 0;
    const int covered = forEachCoveredPixel(g, [&](int x, int y) {
        target.writePacked(x, y, tri.packedColor);
        ++written;
    });

    if (stats != nullptr) {
        stats->pixelsCovered += covered;
        stats->pixelsWritten += written;
    }
}

}  // namespace sr

