#include "CaptureSnapshot.h"
#include "CaptureBuffer.h"

CaptureSnapshot CaptureSnapshot::fromBuffer(const CaptureBuffer &buffer)
{
    CaptureSnapshot snap;
    snap.prims = buffer.prims();
    snap.matrices = buffer.matrices();
    snap.cameraMatrixId = buffer.cameraMatrixId();
    return snap;
}
