#ifndef GTECAPTURE_H
#define GTECAPTURE_H

#include "CaptureTypes.h"

#include <cstdint>

/** Hashing, dedup, and structural validation for GTE matrix records (#419 / #675). */
namespace GteCapture {

uint64_t hashMatrix(const MatrixRecord &matrix);
bool matricesEqual(const MatrixRecord &a, const MatrixRecord &b);

/**
 * Returns true when @p matrix's RT block looks like a 12.4 fixed-point
 * orthonormal rotation (#675). Used to gate the RT^T fast path in
 * GteInverse::screenToModel and to reject false-positive matrix candidates
 * surfaced by PsxGteRamScanner.
 *
 * The three rows of an orthonormal rotation R satisfy:
 *  - |row_r|^2 == 4096^2 (each row has unit length in 12.4 fixed),
 *  - dot(row_i, row_j) == 0 for i != j (rows are mutually orthogonal),
 *  - det(R) == +4096^3 (proper rotation, not a reflection).
 *
 * Each invariant is checked with a 5% slack so 12-bit fixed-point rounding
 * noise in real PS1 games (`gte_ucos`/`gte_usin` quantised to 12.4) does not
 * cause false rejections.
 */
bool looksOrthonormalRotation(const MatrixRecord &matrix);

} // namespace GteCapture

#endif // GTECAPTURE_H
