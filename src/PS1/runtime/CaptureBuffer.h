#ifndef CAPTUREBUFFER_H
#define CAPTUREBUFFER_H

#include "CaptureTypes.h"

#include <QHash>
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

    const QVector<PrimRecord> &prims() const { return m_prims; }
    const QVector<MatrixRecord> &matrices() const { return m_matrices; }
    const QVector<DrawModeRecord> &drawModes() const { return m_drawModes; }

    uint32_t cameraMatrixId() const { return m_cameraMatrixId; }
    bool hasCameraMatrix() const { return m_cameraMatrixId != UINT32_MAX; }

    void clear();

private:
    QVector<PrimRecord> m_prims;
    QVector<MatrixRecord> m_matrices;
    QVector<DrawModeRecord> m_drawModes;
    QHash<uint64_t, uint32_t> m_matrixIndexByHash;
    QHash<uint32_t, int> m_matrixUseCount;
    uint32_t m_cameraMatrixId = UINT32_MAX;
};

#endif // CAPTUREBUFFER_H
