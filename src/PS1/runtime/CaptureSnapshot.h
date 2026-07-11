#ifndef CAPTURESNAPSHOT_H
#define CAPTURESNAPSHOT_H

#include "CapturedModelMesh.h"
#include "CaptureTypes.h"

#include <QMetaType>
#include <QVector>

#include <cstdint>

class CaptureBuffer;

/** Thread-safe copy of a frame capture for main-thread reconstruction (#422). */
struct CaptureSnapshot
{
    QVector<PrimRecord> prims;
    QVector<MatrixRecord> matrices;
    /** Model-space meshes from format-aware RAM scanners (TMD/HMD/..., #674). */
    QVector<CapturedModelMesh> modelMeshes;
    /** In-core GTE transform records; PsxVertex::gteRecordIndex points here (#814). */
    QVector<GteRecordEntry> gteRecords;
    uint32_t cameraMatrixId = UINT32_MAX;
    /** Live VRAM cells (1024×512) copied at capture time for texture decode. */
    QVector<uint16_t> vramCells;

    bool hasVram() const
    {
        return vramCells.size() == 1024 * 512;
    }

    bool hasCameraMatrix() const { return cameraMatrixId != UINT32_MAX; }

    static CaptureSnapshot fromBuffer(const CaptureBuffer &buffer,
                                      const QVector<uint16_t> &vramCells = {});
};

Q_DECLARE_METATYPE(CaptureSnapshot)

#endif // CAPTURESNAPSHOT_H
