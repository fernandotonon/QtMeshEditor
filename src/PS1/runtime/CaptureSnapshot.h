#ifndef CAPTURESNAPSHOT_H
#define CAPTURESNAPSHOT_H

#include "CaptureTypes.h"

#include <QMetaType>
#include <QVector>

class CaptureBuffer;

/** Thread-safe copy of a frame capture for main-thread reconstruction (#422). */
struct CaptureSnapshot
{
    QVector<PrimRecord> prims;
    QVector<MatrixRecord> matrices;
    uint32_t cameraMatrixId = UINT32_MAX;
    /** Live VRAM cells (1024×512) copied at capture time for texture decode. */
    QVector<uint16_t> vramCells;

    bool hasVram() const
    {
        return vramCells.size() == 1024 * 512;
    }

    static CaptureSnapshot fromBuffer(const CaptureBuffer &buffer,
                                      const QVector<uint16_t> &vramCells = {});
};

Q_DECLARE_METATYPE(CaptureSnapshot)

#endif // CAPTURESNAPSHOT_H
