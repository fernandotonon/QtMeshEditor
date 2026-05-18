#ifndef STUBCAPTURESYNTH_H
#define STUBCAPTURESYNTH_H

#include <cstdint>

class EmuHooks;

/** Emits one primitive of each GP0 flavor for capture regression tests (#418). */
void stubEmitCaptureSample(EmuHooks *hooks);

/** Fills VRAM with CLUT + 4/8/15 bpp test regions via onVramWrite (#420). */
void stubFillVramPattern(EmuHooks *hooks, std::uint64_t frameIndex);

#endif // STUBCAPTURESYNTH_H
