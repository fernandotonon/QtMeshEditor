#include "CaptureSnapshot.h"
#include "CaptureBuffer.h"

CaptureSnapshot CaptureSnapshot::fromBuffer(const CaptureBuffer &buffer,
                                            const QVector<uint16_t> &vramCells)
{
    CaptureSnapshot snap;
    snap.prims = buffer.prims();
    snap.matrices = buffer.matrices();
    snap.cameraMatrixId = buffer.cameraMatrixId();
    snap.vramCells = vramCells;
    return snap;
}
