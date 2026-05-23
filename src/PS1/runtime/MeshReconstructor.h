#ifndef MESHRECONSTRUCTOR_H
#define MESHRECONSTRUCTOR_H

#include "CaptureSnapshot.h"
#include "MeshReconstructionStats.h"

#include <QString>
#include <QVector>

enum class MeshDedupeMode;

/** One vertex in reconstructed editor space (#422). */
struct ReconstructedVertex {
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float nx = 0.0f;
    float ny = 0.0f;
    float nz = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
    uint32_t diffuseArgb = 0xFFFFFFFFu;
};

struct ReconstructedSubMesh {
    QString materialName;
    QVector<ReconstructedVertex> vertices;
    QVector<uint32_t> indices;
};

struct ReconstructedMesh {
    QString meshName;
    QVector<ReconstructedSubMesh> subMeshes;
    int vertexCount = 0;
    int triangleCount = 0;

    bool isEmpty() const { return subMeshes.isEmpty(); }
};

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
