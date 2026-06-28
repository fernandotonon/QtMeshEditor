#ifndef UV_SEAM_OPS_H
#define UV_SEAM_OPS_H

#include "EditableMesh.h"
#include "UvSeamData.h"

#include <OgreVector2.h>
#include <QString>

#include <vector>

/// Topology / UV operations for seams (issue #462).
namespace UvSeamOps {

struct EdgeSplitResult {
    bool applied = false;
    int  vertsAdded = 0;
    int  edgesSplit = 0;
    QString error;
};

struct EdgeSewResult {
    bool applied = false;
    int  edgesSewn = 0;
    QString error;
};

/// Duplicate vertices along `edges` so each edge becomes a UV boundary.
EdgeSplitResult splitEdges(EditableMesh& mesh, size_t subMeshIndex,
                           const std::vector<UvSeamData::EdgeKey>& edges);

/// Average UVs of both sides of duplicate seam verts on `edges`.
EdgeSewResult sewEdges(EditableMesh& mesh, size_t subMeshIndex,
                       const std::vector<UvSeamData::EdgeKey>& edges);

/// Convert global Edit Mode edge pairs to per-submesh local keys.
std::vector<UvSeamData::EdgeKey> localEdgeKeysFromGlobal(
    const EditableMesh& mesh,
    const std::vector<std::pair<int, int>>& globalEdges,
    size_t& outSubMeshIndex);

} // namespace UvSeamOps

#endif // UV_SEAM_OPS_H
