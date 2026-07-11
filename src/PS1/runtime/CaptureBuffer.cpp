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
    if (found != m_matrixIndexByHash.constEnd()) {
        const uint32_t existingId = found.value();
        if (existingId < static_cast<uint32_t>(m_matrices.size())
            && GteCapture::matricesEqual(m_matrices[static_cast<int>(existingId)], matrix))
            return existingId;
    }

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

uint32_t CaptureBuffer::addGteRecord(const GteRecordEntry &record)
{
    // 256k entries ≈ 22 MB — roomy for multi-frame scene captures while
    // bounding a long-running live session (#814). Prim ingest is unaffected;
    // vertices that lose their record degrade to DepthOnly at resolve time.
    constexpr int kMaxGteRecords = 256 * 1024;
    if (m_gteRecords.size() >= kMaxGteRecords) {
        ++m_droppedGteRecords;
        return UINT32_MAX;
    }
    m_gteRecords.append(record);
    return static_cast<uint32_t>(m_gteRecords.size() - 1);
}

bool CaptureBuffer::addModelMesh(const CapturedModelMesh &mesh)
{
    if (mesh.mesh.isEmpty())
        return false;
    if (mesh.contentHash != 0 && m_modelMeshHashes.contains(mesh.contentHash))
        return false;
    m_modelMeshes.append(mesh);
    if (mesh.contentHash != 0)
        m_modelMeshHashes.insert(mesh.contentHash);
    return true;
}

void CaptureBuffer::clear()
{
    m_prims.clear();
    m_matrices.clear();
    m_drawModes.clear();
    m_modelMeshes.clear();
    m_gteRecords.clear();
    m_droppedGteRecords = 0;
    m_modelMeshHashes.clear();
    m_matrixIndexByHash.clear();
    m_matrixUseCount.clear();
    m_cameraMatrixId = UINT32_MAX;
}
