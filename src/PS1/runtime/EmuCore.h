#ifndef EMUCORE_H
#define EMUCORE_H

#include "EmuFramebuffer.h"
#include "EmuHooks.h"

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
    virtual void runFrame() = 0;
    virtual void reset() = 0;
    virtual const EmuFramebuffer &framebuffer() const = 0;
    virtual void setHooks(EmuHooks *hooks) = 0;
};

#endif // EMUCORE_H
