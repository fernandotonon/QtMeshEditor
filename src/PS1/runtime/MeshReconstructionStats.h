#ifndef MESHRECONSTRUCTIONSTATS_H
#define MESHRECONSTRUCTIONSTATS_H

/** Post-reconstruction quality metrics for PS1 capture (#658, #674). */
struct MeshReconstructionStats {
    int totalVertices = 0;
    int gteInverseVertices = 0;
    int screenFallbackVertices = 0;
    /** Vertices that arrived already in model space via PsxTmdRamScanner/PsxHmdRamScanner. */
    int modelMeshVertices = 0;
    int primsTotal = 0;
    int primsWithMatrixId = 0;
    float boundsMinX = 0.0f;
    float boundsMaxX = 0.0f;
    float boundsMinY = 0.0f;
    float boundsMaxY = 0.0f;
    float boundsMinZ = 0.0f;
    float boundsMaxZ = 0.0f;
    bool slabLike = false;

    /** % of vertices that are "trusted" (GTE-inverted or model-mesh) out of all. */
    int gteInversePercent() const;
    bool hasBounds() const;
    /** Sets slabLike from axis extents (min/max ratio below 0.12). Requires hasBounds(). */
    void finalizeSlabMetric();
};

#endif // MESHRECONSTRUCTIONSTATS_H
