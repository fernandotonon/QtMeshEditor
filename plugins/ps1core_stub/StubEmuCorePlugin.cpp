#include "StubEmuCorePlugin.h"
#include "StubEmuCore.h"

QString StubEmuCorePlugin::pluginId() const
{
    return QStringLiteral("stub");
}

std::unique_ptr<EmuCore> StubEmuCorePlugin::createCore()
{
    return std::make_unique<StubEmuCore>();
}
