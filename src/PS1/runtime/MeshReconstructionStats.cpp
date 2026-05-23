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
