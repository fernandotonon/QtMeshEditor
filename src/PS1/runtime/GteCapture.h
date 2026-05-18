#ifndef GTECAPTURE_H
#define GTECAPTURE_H

#include "CaptureTypes.h"

#include <cstdint>

/** Hashing and deduplication for GTE matrix records (#419). */
namespace GteCapture {

uint64_t hashMatrix(const MatrixRecord &matrix);
bool matricesEqual(const MatrixRecord &a, const MatrixRecord &b);

} // namespace GteCapture

#endif // GTECAPTURE_H
