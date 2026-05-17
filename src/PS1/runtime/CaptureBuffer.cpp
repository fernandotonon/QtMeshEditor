#include "CaptureBuffer.h"

#include "GteCapture.h"

void CaptureBuffer::beginFrame()
{
    m_prims.clear();
    m_drawModes.clear();
    m_matrixUseCount.clear();
    m_cameraMatrixId = UINT32_MAX;
}

void CaptureBuffer::endFrame()
{
    int bestCount = 0;
    uint32_t bestId = UINT32_MAX;
    for (auto it = m_matrixUseCount.constBegin(); it != m_matrixUseCount.constEnd(); ++it) {
        if (it.value() > bestCount) {
            bestCount = it.value();
            bestId = it.key();
        }
    }
    m_cameraMatrixId = bestId;
}

uint32_t CaptureBuffer::addMatrix(MatrixRecord matrix)
{
    matrix.hash = GteCapture::hashMatrix(matrix);
    const auto found = m_matrixIndexByHash.constFind(matrix.hash);
    if (found != m_matrixIndexByHash.constEnd())
        return found.value();

    const uint32_t id = static_cast<uint32_t>(m_matrices.size());
    m_matrices.append(matrix);
    m_matrixIndexByHash.insert(matrix.hash, id);
    return id;
}

void CaptureBuffer::addPrim(PrimRecord prim)
{
    if (prim.matrixId < static_cast<uint32_t>(m_matrices.size()))
        m_matrixUseCount[prim.matrixId] = m_matrixUseCount.value(prim.matrixId, 0) + 1;
    m_prims.append(prim);
}

void CaptureBuffer::addDrawMode(const DrawModeRecord &mode)
{
    m_drawModes.append(mode);
}

void CaptureBuffer::clear()
{
    m_prims.clear();
    m_matrices.clear();
    m_drawModes.clear();
    m_matrixIndexByHash.clear();
    m_matrixUseCount.clear();
    m_cameraMatrixId = UINT32_MAX;
}
