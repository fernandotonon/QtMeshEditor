#pragma once
// Mesh prepared for generic segmentation (#1179): normalised, welded for
// adjacency, with per-face normals/areas and a smooth per-vertex normal.
// Pure data (no Ogre, no Qt).

#include "TriangleBvh.h"

#include <cstdint>
#include <vector>

namespace GenericSeg {

struct SegMesh {
    // Positions moved so the bounding-box centre is the origin and scaled so
    // the farthest vertex is at distance 1 (unit bounding sphere). Same
    // vertex order as the input.
    std::vector<Vec3> positions;
    std::vector<uint32_t> indices;        // input indices, face order kept
    // Welded corner ids (coincident positions share an id): what "adjacent"
    // means for the cleanup passes, since exporters split vertices at UV and
    // normal seams.
    std::vector<uint32_t> weldedIndices;
    int weldedCount = 0;
    std::vector<Vec3> weldedNormals;      // area-weighted, per welded vertex
    std::vector<Vec3> faceNormals;        // unit (zero for degenerate faces)
    std::vector<float> faceAreas;
    std::vector<std::vector<int>> faceNeighbors;   // edge-sharing, welded
    float scale = 1.0f;                   // input units per normalised unit
    Vec3 center;                          // input-space centre

    int faceCount() const { return int(indices.size() / 3); }

    // Returns an empty mesh (faceCount() == 0) for invalid input.
    static SegMesh build(const float* positions, int vertexCount,
                         const uint32_t* indices, int indexCount);
};

} // namespace GenericSeg
