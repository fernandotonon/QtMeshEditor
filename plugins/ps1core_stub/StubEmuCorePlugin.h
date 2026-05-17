#ifndef STUBEMUCOREPLUGIN_H
#define STUBEMUCOREPLUGIN_H

#include <QObject>
#include "IEmuCorePlugin.h"

class StubEmuCorePlugin : public QObject, public IEmuCorePlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID IEmuCorePlugin_iid FILE "ps1core_stub.json")
    Q_INTERFACES(IEmuCorePlugin)

public:
    QString pluginId() const override;
    std::unique_ptr<EmuCore> createCore() override;
};

#endif // STUBEMUCOREPLUGIN_H
