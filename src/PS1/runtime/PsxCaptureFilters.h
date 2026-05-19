#ifndef PSXCAPTUREFILTERS_H
#define PSXCAPTUREFILTERS_H

#include "CaptureTypes.h"

/** Shared heuristics for RAM-scanned GPU primitives (#418). */
namespace PsxCaptureFilters {

/** Typical visible PS1 framebuffer bounds with margin (320×240). */
constexpr int kVisibleMinX = -96;
constexpr int kVisibleMaxX = 416;
constexpr int kVisibleMinY = -96;
constexpr int kVisibleMaxY = 336;

bool isPlausiblePrim(const PrimRecord &prim);
bool isOnScreenPrim(const PrimRecord &prim);

} // namespace PsxCaptureFilters

#endif // PSXCAPTUREFILTERS_H
