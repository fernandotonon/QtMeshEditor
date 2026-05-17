#include "EmuCoreLoader.h"
#include "IEmuCorePlugin.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPluginLoader>

namespace {

QString bundleOrAppDir()
{
    return QCoreApplication::applicationDirPath();
}

QStringList pluginFileNames()
{
#if defined(Q_OS_WIN)
    return {QStringLiteral("qtmesh_ps1core_stub.dll")};
#elif defined(Q_OS_MACOS)
    return {QStringLiteral("libqtmesh_ps1core_stub.dylib"),
            QStringLiteral("qtmesh_ps1core_stub.dylib")};
#else
    return {QStringLiteral("libqtmesh_ps1core_stub.so"),
            QStringLiteral("qtmesh_ps1core_stub.so")};
#endif
}

} // namespace

QStringList EmuCoreLoader::coreSearchPaths()
{
    QStringList paths;
    const QString base = bundleOrAppDir();
    paths << QDir(base).filePath(QStringLiteral("PS1Cores"));
    paths << QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("PS1Cores"));
    paths.removeDuplicates();
    return paths;
}

std::unique_ptr<EmuCore> EmuCoreLoader::loadCore(QString *errorOut)
{
    const QStringList names = pluginFileNames();
    for (const QString &dirPath : coreSearchPaths()) {
        const QDir dir(dirPath);
        if (!dir.exists())
            continue;

        for (const QString &fileName : names) {
            const QString pluginPath = dir.filePath(fileName);
            if (!QFileInfo::exists(pluginPath))
                continue;

            QPluginLoader loader(pluginPath);
            QObject *instance = loader.instance();
            if (!instance) {
                if (errorOut)
                    *errorOut = loader.errorString();
                continue;
            }

            auto *plugin = qobject_cast<IEmuCorePlugin *>(instance);
            if (!plugin) {
                if (errorOut)
                    *errorOut = QObject::tr("Plugin does not implement IEmuCorePlugin: %1").arg(pluginPath);
                continue;
            }

            return plugin->createCore();
        }
    }

    if (errorOut) {
        *errorOut = QObject::tr(
            "No PS1 emulator core found in PS1Cores/. "
            "Build with ENABLE_PS1_RIP=ON to install the stub core.");
    }
    return nullptr;
}
