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

    static CaptureSnapshot fromBuffer(const CaptureBuffer &buffer);
};

Q_DECLARE_METATYPE(CaptureSnapshot)

#endif // CAPTURESNAPSHOT_H
