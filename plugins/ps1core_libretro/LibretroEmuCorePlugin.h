#ifndef LIBRETROEMUCOREPLUGIN_H
#define LIBRETROEMUCOREPLUGIN_H

#include "IEmuCorePlugin.h"

#include <QObject>

class LibretroEmuCorePlugin : public QObject, public IEmuCorePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID IEmuCorePlugin_iid FILE "ps1core_libretro.json")
    Q_INTERFACES(IEmuCorePlugin)

public:
    QString pluginId() const override;
    std::unique_ptr<EmuCore> createCore() override;
};

#endif // LIBRETROEMUCOREPLUGIN_H
