#ifndef STUBCAPTURESYNTH_H
#define STUBCAPTURESYNTH_H

class EmuHooks;

/** Emits one primitive of each GP0 flavor for capture regression tests (#418). */
void stubEmitCaptureSample(EmuHooks *hooks);

#endif // STUBCAPTURESYNTH_H
