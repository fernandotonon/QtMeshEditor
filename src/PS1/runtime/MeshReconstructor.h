#ifndef MESHRECONSTRUCTOR_H
#define MESHRECONSTRUCTOR_H

#include "CaptureSnapshot.h"
#include "MeshReconstructionStats.h"
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
};

/** Deduplicated capture output: unique meshes + instance transforms (#423). */
struct ReconstructedCaptureSet {
    QVector<ReconstructedMesh> uniqueMeshes;
    QVector<ReconstructedInstance> instances;
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
};

#endif // MESHRECONSTRUCTOR_H
