#include "EmuCoreLoader.h"
#include "IEmuCorePlugin.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPluginLoader>

#include <memory>
#include <vector>

namespace {

std::vector<std::unique_ptr<QPluginLoader>> &hostPluginLoaders()
{
    static std::vector<std::unique_ptr<QPluginLoader>> loaders;
    return loaders;
}


QString bundleOrAppDir()
{
#if defined(Q_OS_MACOS)
    const QString appDir = QCoreApplication::applicationDirPath();
    QDir dir(appDir);
    if (dir.dirName() == QLatin1String("MacOS")) {
        dir.cdUp();
        if (dir.dirName() == QLatin1String("Contents"))
            dir.cdUp();
    }
    return dir.absolutePath();
#else
    return QCoreApplication::applicationDirPath();
#endif
}

QStringList hostPluginBaseNames()
{
    const QByteArray forceStub = qgetenv("QTMESH_PS1_FORCE_STUB");
    if (forceStub == "1" || forceStub == "true") {
        return {QStringLiteral("qtmesh_ps1core_stub")};
    }
    return {QStringLiteral("qtmesh_ps1core_libretro"), QStringLiteral("qtmesh_ps1core_stub")};
}

QStringList fileNamesForHost(const QString &baseName)
{
#if defined(Q_OS_WIN)
    return {baseName + QStringLiteral(".dll")};
#elif defined(Q_OS_MACOS)
    return {QStringLiteral("lib") + baseName + QStringLiteral(".dylib"), baseName + QStringLiteral(".dylib")};
#else
    return {QStringLiteral("lib") + baseName + QStringLiteral(".so"), baseName + QStringLiteral(".so")};
#endif
}

} // namespace

QStringList EmuCoreLoader::coreSearchPaths()
{
    QStringList paths;
    // Shipped layout: cores bundled inside the .app (bundleOrAppDir climbs to
    // the .app root). Dev layout: the plugin CMake writes cores to
    // <build>/bin/PS1Cores, i.e. the directory *containing* the .app — so also
    // search one level above the bundle. And the raw-binary dir for good
    // measure (non-bundled builds / Linux / Windows).
    paths << QDir(bundleOrAppDir()).filePath(QStringLiteral("PS1Cores"));
#if defined(Q_OS_MACOS)
    paths << QDir(bundleOrAppDir() + QStringLiteral("/..")).filePath(QStringLiteral("PS1Cores"));
#endif
    paths << QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("PS1Cores"));
    for (QString &p : paths)
        p = QDir(p).absolutePath();
    paths.removeDuplicates();
    return paths;
}

std::unique_ptr<EmuCore> EmuCoreLoader::loadCore(QString *errorOut)
{
    QString lastError;
    bool libretroHostFound = false;
    QString libretroHostDeclineError;

    const QByteArray forceStub = qgetenv("QTMESH_PS1_FORCE_STUB");
    const bool allowStubFallback = (forceStub == "1" || forceStub == "true");

    for (const QString &baseName : hostPluginBaseNames()) {
        for (const QString &dirPath : coreSearchPaths()) {
            const QDir dir(dirPath);
            if (!dir.exists())
                continue;

            for (const QString &fileName : fileNamesForHost(baseName)) {
                const QString pluginPath = dir.filePath(fileName);
                if (!QFileInfo::exists(pluginPath))
                    continue;

                auto loader = std::make_unique<QPluginLoader>(pluginPath);
                QObject *instance = loader->instance();
                if (!instance) {
                    lastError = loader->errorString();
                    continue;
                }

                auto *plugin = qobject_cast<IEmuCorePlugin *>(instance);
                if (!plugin) {
                    lastError = QObject::tr("Plugin does not implement IEmuCorePlugin: %1").arg(pluginPath);
                    continue;
                }

                const bool isLibretroHost =
                    baseName == QStringLiteral("qtmesh_ps1core_libretro");
                if (isLibretroHost)
                    libretroHostFound = true;

                std::unique_ptr<EmuCore> core = plugin->createCore();
                if (core) {
                    hostPluginLoaders().push_back(std::move(loader));
                    return core;
                }

                if (isLibretroHost) {
                    libretroHostDeclineError =
                        QObject::tr("Libretro host loaded but no PSX core was found in %1. "
                                      "Run scripts/install-ps1-libretro-core.sh or set "
                                      "QTMESH_PS1_LIBRETRO_CORE to mednafen_psx_libretro.so.")
                            .arg(dirPath);
                    if (!allowStubFallback) {
                        if (errorOut)
                            *errorOut = libretroHostDeclineError;
                        return nullptr;
                    }
                } else {
                    lastError = QObject::tr("Host plugin %1 declined to create a core").arg(fileName);
                }
            }
        }
    }

    if (errorOut) {
        if (!libretroHostDeclineError.isEmpty() && libretroHostFound)
            *errorOut = libretroHostDeclineError;
        else if (!lastError.isEmpty())
            *errorOut = lastError;
        else {
            *errorOut = QObject::tr(
                "No PS1 emulator host found in PS1Cores/. "
                "Build with ENABLE_PS1_RIP=ON and install a libretro PSX core "
                "(mednafen_psx_libretro) or use the stub for development.");
        }
    }
    return nullptr;
}
