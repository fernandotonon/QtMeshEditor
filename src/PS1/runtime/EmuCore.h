#ifndef EMUCORE_H
#define EMUCORE_H

#include "EmuFramebuffer.h"
#include "EmuHooks.h"
#include "PsxVramMirrorMode.h"

#include <QString>
#include <memory>

/**
 * Abstract PS1 emulator core (#415). Implemented by dynamically loaded plugins
 * under <app>/PS1Cores/ — never linked into the main QtMeshEditor binary.
 */
class EmuCore
{
public:
    virtual ~EmuCore() = default;

    virtual QString coreId() const = 0;
    virtual bool loadBios(const QString &biosPath) = 0;
    virtual bool loadIso(const QString &isoPath) = 0;
    /** Load disc into the core and verify a video frame can be produced. */
    virtual bool boot(QString *errorOut = nullptr) { (void)errorOut; return true; }
    virtual void runFrame() = 0;
    virtual void reset() = 0;
    virtual const EmuFramebuffer &framebuffer() const = 0;
    virtual void setHooks(EmuHooks *hooks) = 0;
    /** Refresh hook-visible mirrors (VRAM, etc.) immediately before capture/dump. */
    virtual void syncCaptureMirrors() {}
    /** Re-scan emulated RAM for GPU packets into the capture buffer (libretro path). */
    virtual void ingestCaptureFrame() {}
    virtual PsxVramMirrorMode lastVramMirrorMode() const { return PsxVramMirrorMode::Unknown; }
    virtual QString lastError() const { return QString(); }

    virtual void setJoypadButton(unsigned port, unsigned buttonId, bool pressed)
    {
        (void)port;
        (void)buttonId;
        (void)pressed;
    }
    virtual void resetJoypad(unsigned port = 0)
    {
        (void)port;
    }
};

#endif // EMUCORE_H
