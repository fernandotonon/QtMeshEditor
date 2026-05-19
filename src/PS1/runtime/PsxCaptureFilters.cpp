#include "PsxCaptureFilters.h"

#include <cmath>

namespace PsxCaptureFilters {

bool isPlausiblePrim(const PrimRecord &prim)
{
    if (prim.vertexCount < 2)
        return false;

    for (int v = 0; v < 4; ++v) {
        if (v >= prim.vertexCount)
            break;
        const PsxVertex &vert = prim.verts[v];
        if (std::abs(vert.x) > 2048 || std::abs(vert.y) > 2048)
            return false;
        if (std::abs(vert.z) > 0x100000)
            return false;
    }
    return true;
}

bool isOnScreenPrim(const PrimRecord &prim)
{
    if (!isPlausiblePrim(prim))
        return false;

    // Keep primitives that touch the visible area (at least one vertex on-screen).
    // Reject only when every vertex is outside the margin bounds.
    for (int v = 0; v < 4; ++v) {
        if (v >= prim.vertexCount)
            break;
        const PsxVertex &vert = prim.verts[v];
        if (vert.x >= kVisibleMinX && vert.x <= kVisibleMaxX && vert.y >= kVisibleMinY
            && vert.y <= kVisibleMaxY)
            return true;
    }
    return false;
}

} // namespace PsxCaptureFilters
