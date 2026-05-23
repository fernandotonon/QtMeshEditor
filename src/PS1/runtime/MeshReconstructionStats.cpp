#include "MeshReconstructionStats.h"

#include <algorithm>
#include <cmath>

int MeshReconstructionStats::gteInversePercent() const
{
    if (totalVertices <= 0)
        return 0;
    return (gteInverseVertices * 100) / totalVertices;
}

bool MeshReconstructionStats::hasBounds() const
{
    return boundsMaxX >= boundsMinX && boundsMaxY >= boundsMinY && boundsMaxZ >= boundsMinZ
           && (boundsMaxX > boundsMinX || boundsMaxY > boundsMinY || boundsMaxZ > boundsMinZ);
}

void MeshReconstructionStats::finalizeSlabMetric()
{
    if (!hasBounds() || totalVertices < 3)
        return;
    const float ex = boundsMaxX - boundsMinX;
    const float ey = boundsMaxY - boundsMinY;
    const float ez = boundsMaxZ - boundsMinZ;
    const float maxExtent = std::max({ex, ey, ez, 1e-6f});
    const float minExtent = std::min({ex, ey, ez});
    slabLike = (minExtent / maxExtent) < 0.12f;
}
