#include "LibretroEmuCorePlugin.h"
#include "LibretroEmuCore.h"

QString LibretroEmuCorePlugin::pluginId() const
{
    return QStringLiteral("libretro");
}

std::unique_ptr<EmuCore> LibretroEmuCorePlugin::createCore()
{
    QString err;
    if (LibretroEmuCore::resolveLibretroCorePath(&err).isEmpty())
        return nullptr;
    return std::make_unique<LibretroEmuCore>();
}
