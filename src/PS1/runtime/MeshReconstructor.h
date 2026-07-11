#ifndef MESHRECONSTRUCTOR_H
#define MESHRECONSTRUCTOR_H

#include "CaptureSnapshot.h"
#include "MeshReconstructionStats.h"
#include "Ps1CoordinateNormalizer.h"
#include "ReconstructedMesh.h"

#include <QString>
#include <QVector>

enum class MeshDedupeMode;

/** One placed copy of a deduplicated mesh (#423). */
struct ReconstructedInstance {
    int uniqueMeshIndex = 0;
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    /** Full GTE matrix of the part's group when it was grouped by a resolved
     *  in-core record (#816). `rot` is the raw GTE rotation, row-major and
     *  unit-normalised from 4.12 fixed (still in the GTE camera convention —
     *  `PS1RipMeshBuilder::editorRotationFromGte` converts axes); `trWorld`
     *  is the GTE translation in editor units (×0.01, Y/Z negated, matching
     *  `GteInverse::modelToEditor`). Defaults keep pre-#816 fixtures valid. */
    float rot[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    float trWorld[3] = {0.0f, 0.0f, 0.0f};
    bool hasMatrix = false;
};

/** Per-`PrimRecord` provenance: where this draw call landed in the
 *  reconstructed scene (#679 / #426 review feedback). Parallel to
 *  `CaptureSnapshot::prims` — entry `i` is the resolution for `prims[i]`.
 *  All fields are `-1` when the prim was culled by the on-screen filter,
 *  carried fewer than 3 / 2 verts, or did not survive dedupe (e.g. only
 *  hit the model-mesh path that has no `PrimRecord`).
 *
 *  This replaces the earlier `materialName → first uniqueMesh` lookup that
 *  collapsed every solid-color row onto a single mesh / instance — the
 *  inspector now resolves each row to the exact submesh and scene node
 *  the prim produced, so highlight / hide / promote target the right
 *  sub-entity even when multiple unique meshes share a material. */
struct PrimProvenance {
    int uniqueMeshIndex = -1;
    int subMeshIndex = -1;
    int instanceIndex = -1;
};

/** Deduplicated capture output: unique meshes + instance transforms (#423). */
struct ReconstructedCaptureSet {
    QVector<ReconstructedMesh> uniqueMeshes;
    QVector<ReconstructedInstance> instances;
    QVector<PrimProvenance> primProvenance; // parallel to CaptureSnapshot::prims
    int capturedPartCount = 0;

    bool isEmpty() const { return uniqueMeshes.isEmpty(); }
    int uniqueCount() const { return uniqueMeshes.size(); }
    int instanceCount() const { return instances.size(); }
};

/** Builds editor-space meshes from a captured primitive stream (#422, #423). */
class MeshReconstructor
{
public:
    static QString textureMaterialName(uint16_t tpage, uint16_t clut, uint8_t semiTrans,
                                       uint32_t drawModeBits);
    static quint64 textureGroupKey(uint16_t tpage, uint16_t clut, uint8_t semiTrans,
                                  uint32_t drawModeBits);

    static ReconstructedMesh reconstruct(const CaptureSnapshot &snapshot);
    static ReconstructedCaptureSet reconstructDeduped(const CaptureSnapshot &snapshot,
                                                      MeshDedupeMode dedupeMode);
    static ReconstructedCaptureSet reconstructDeduped(const CaptureSnapshot &snapshot,
                                                      MeshDedupeMode dedupeMode,
                                                      MeshReconstructionStats *statsOut);
    /** Variant that honours the user's normalizer settings (#424). When
     *  `normalize.perspectiveCorrectUVs` is true, screen-space prims whose
     *  vertex-depth ratio exceeds `normalize.perspectiveTolerance` are
     *  subdivided via midpoint triangulation (recursion capped at
     *  `normalize.perspectiveMaxDepth`). The per-axis flip / userScale fields
     *  are applied via SceneNode scale by `PS1RipMeshBuilder`, not by this
     *  reconstructor — they don't affect the mesh data. */
    static ReconstructedCaptureSet reconstructDeduped(const CaptureSnapshot &snapshot,
                                                      MeshDedupeMode dedupeMode,
                                                      const Ps1NormalizerSettings &normalize,
                                                      MeshReconstructionStats *statsOut);
};

#endif // MESHRECONSTRUCTOR_H
