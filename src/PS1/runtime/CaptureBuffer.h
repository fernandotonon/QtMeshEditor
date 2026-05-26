#ifndef CAPTUREBUFFER_H
#define CAPTUREBUFFER_H

#include "CapturedModelMesh.h"
#include "CaptureTypes.h"

#include <QHash>
#include <QSet>
#include <QVector>

#include <cstdint>

/**
 * Per-frame capture arena for GPU primitives and GTE matrices (#418, #419).
 */
class CaptureBuffer
{
public:
    void beginFrame();
    void endFrame();

    uint32_t addMatrix(MatrixRecord matrix);
    void addPrim(PrimRecord prim);
    void addDrawMode(const DrawModeRecord &mode);

    /** Append a model-space mesh produced by a format-aware scanner (#674). Deduplicates by
     *  content hash so the same TMD encountered repeatedly only emits once per session. */
    bool addModelMesh(const CapturedModelMesh &mesh);

    const QVector<PrimRecord> &prims() const { return m_prims; }
    const QVector<MatrixRecord> &matrices() const { return m_matrices; }
    const QVector<DrawModeRecord> &drawModes() const { return m_drawModes; }
    const QVector<CapturedModelMesh> &modelMeshes() const { return m_modelMeshes; }

    uint32_t cameraMatrixId() const { return m_cameraMatrixId; }
    bool hasCameraMatrix() const { return m_cameraMatrixId != UINT32_MAX; }

    void clear();

private:
    QVector<PrimRecord> m_prims;
    QVector<MatrixRecord> m_matrices;
    QVector<DrawModeRecord> m_drawModes;
    QVector<CapturedModelMesh> m_modelMeshes;
    QSet<uint64_t> m_modelMeshHashes;
    QHash<uint64_t, uint32_t> m_matrixIndexByHash;
    QHash<uint32_t, int> m_matrixUseCount;
    uint32_t m_cameraMatrixId = UINT32_MAX;
};

#endif // CAPTUREBUFFER_H
