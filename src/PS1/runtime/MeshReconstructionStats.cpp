#include "MeshReconstructionStats.h"

#include <algorithm>
#include <cmath>

int MeshReconstructionStats::gteInversePercent() const
{
    if (totalVertices <= 0)
        return 0;
    // Model-mesh verts are trusted-by-construction (already in model space), so they
    // count toward the "quality" numerator the same way GTE-inverted verts do (#674).
    const int trusted = gteInverseVertices + modelMeshVertices;
    return (trusted * 100) / totalVertices;
}

bool MeshReconstructionStats::hasBounds() const
{
    return boundsMaxX >= boundsMinX && boundsMaxY >= boundsMinY && boundsMaxZ >= boundsMinZ
           && (boundsMaxX > boundsMinX || boundsMaxY > boundsMinY || boundsMaxZ > boundsMinZ);
}

void MeshReconstructionStats::finalizeSlabMetric()
{
    slabLike = false;
    if (!hasBounds() || totalVertices < 3)
        return;
    const float ex = boundsMaxX - boundsMinX;
    const float ey = boundsMaxY - boundsMinY;
    const float ez = boundsMaxZ - boundsMinZ;
    const float maxExtent = std::max({ex, ey, ez, 1e-6f});
    const float minExtent = std::min({ex, ey, ez});
    slabLike = (minExtent / maxExtent) < 0.12f;
}
