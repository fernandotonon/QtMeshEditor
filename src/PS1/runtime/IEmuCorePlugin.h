#ifndef IEMUCOREPLUGIN_H
#define IEMUCOREPLUGIN_H

#include "EmuCore.h"

#include <QtPlugin>
#include <QString>
#include <memory>

/**
 * Qt plugin interface for PS1 emulator cores (GPL-isolated shared libraries).
 */
class IEmuCorePlugin
{
public:
    virtual ~IEmuCorePlugin() = default;
    virtual QString pluginId() const = 0;
    virtual std::unique_ptr<EmuCore> createCore() = 0;
};

#define IEmuCorePlugin_iid "com.fernandotonon.qtmesh.EmuCorePlugin/1.0"
Q_DECLARE_INTERFACE(IEmuCorePlugin, IEmuCorePlugin_iid)

#endif // IEMUCOREPLUGIN_H
