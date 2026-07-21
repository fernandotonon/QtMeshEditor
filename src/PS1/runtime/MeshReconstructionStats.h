#ifndef MESHRECONSTRUCTIONSTATS_H
#define MESHRECONSTRUCTIONSTATS_H

/** Post-reconstruction quality metrics for PS1 capture (#658, #674). */
struct MeshReconstructionStats {
    int totalVertices = 0;
    int gteInverseVertices = 0;
    int screenFallbackVertices = 0;
    /** Vertices that arrived already in model space via PsxTmdRamScanner/PsxHmdRamScanner. */
    int modelMeshVertices = 0;
    /** Tier 0 (#816): exact object-space vertices from resolved in-core GTE records. */
    int gteTrackedVertices = 0;
    /** Tier 1 (#816): PGXP subpixel screen coords + view depth inverted per draw matrix. */
    int depthOnlyVertices = 0;
    /** Tier 0/1 vertices dropped by the per-part outlier policy (> 8 × p99 radius, #816). */
    int outlierDroppedVertices = 0;
    /** Triangles dropped by the zero-area cleanup cull (#428 cleanup pipeline). */
    int zeroAreaTrianglesDropped = 0;
    /** Tracked groups merged into an earlier same-object group by the
     *  cross-frame object merge's RIGID stage (#412, exact vertex overlap —
     *  triangles union). */
    int mergedPartGroups = 0;
    /** Tracked groups of vertex-ANIMATED objects whose prims were dropped in
     *  favour of the chain's representative frame (#412 non-rigid stage —
     *  texture/count/draw-order continuity matching). */
    int nonRigidMergedGroups = 0;
    /** Repeated triangles (same position+UV, one copy per captured frame)
     *  dropped by the duplicate cull that runs with the object merge (#412). */
    int duplicateTrianglesDropped = 0;
    /** Prims whose tracked vertices resolved to more than one GTE matrix (skinned, #816). */
    int mixedMatrixPrims = 0;
    int primsTotal = 0;
    int primsWithMatrixId = 0;
    float boundsMinX = 0.0f;
    float boundsMaxX = 0.0f;
    float boundsMinY = 0.0f;
    float boundsMaxY = 0.0f;
    float boundsMinZ = 0.0f;
    float boundsMaxZ = 0.0f;
    /** True once the first vertex seeded the AABB. Distinct from hasBounds()
     *  (which also demands a non-zero extent): without it the fold helper
     *  re-initialised the bounds on every vertex — min==max after seeding
     *  never satisfies hasBounds() — so the AABB collapsed to the last
     *  folded vertex and the slab canary could never fire (#816 fix). */
    bool boundsSeeded = false;
    bool slabLike = false;

    /** % of vertices that are "trusted" (GTE-tracked, GTE-inverted or model-mesh) out of all. */
    int gteInversePercent() const;
    /** % of vertices placed exactly from in-core GTE records (Tier 0, #816). */
    int gteTrackedPercent() const;
    /** % of vertices placed via PGXP depth inversion (Tier 1, #816). */
    int depthOnlyPercent() const;
    bool hasBounds() const;
    /** Sets slabLike from axis extents (min/max ratio below 0.12). Requires hasBounds(). */
    void finalizeSlabMetric();
};

#endif // MESHRECONSTRUCTIONSTATS_H
