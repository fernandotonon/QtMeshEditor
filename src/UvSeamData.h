#ifndef UV_SEAM_DATA_H
#define UV_SEAM_DATA_H

#include "EditableMesh.h"

#include <OgreMesh.h>

#include <cstdint>
#include <unordered_set>
#include <vector>

/// Seam / pin metadata for UV workflows (issue #462).
///
/// Stored per submesh on `EditableSubMesh::seamEdges` and
/// `EditableSubMesh::pinnedVertices`, mirrored to Ogre::Mesh
/// UserObjectBindings as:
///   - `qtme.seams.<submeshIndex>`  → vector of packed edge keys (uint64)
///   - `qtme.uv_pins.<submeshIndex>` → vector of local vertex indices
namespace UvSeamData {

using EdgeKey = uint64_t;

EdgeKey makeEdgeKey(unsigned int a, unsigned int b);

void writeBindingsToMesh(Ogre::Mesh* mesh, const std::vector<EditableSubMesh>& subMeshes);
void readBindingsFromMesh(const Ogre::Mesh* mesh, std::vector<EditableSubMesh>& subMeshes);

bool isSeam(const EditableSubMesh& sub, unsigned int a, unsigned int b);
void setSeam(EditableSubMesh& sub, unsigned int a, unsigned int b, bool seam);

bool isPinned(const EditableSubMesh& sub, unsigned int localVert);
void setPinned(EditableSubMesh& sub, unsigned int localVert, bool pinned);

/// Map a mesh-global vertex index (Edit Mode convention) to submesh + local.
bool globalVertToSubLocal(const EditableMesh& mesh, int globalVert,
                          size_t& subMeshIndex, unsigned int& localVert);

EdgeKey globalEdgeToLocalKey(const EditableMesh& mesh, int gv0, int gv1,
                             size_t& subMeshIndex, EdgeKey& localKey);

} // namespace UvSeamData

#endif // UV_SEAM_DATA_H
