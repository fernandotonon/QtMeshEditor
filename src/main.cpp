#include <QApplication>
#include <QCoreApplication>
#include <QMessageBox>
#include "WelcomeDialog.h"
#include <QPalette>
#include <QDebug>
#include <QTimer>
#include <QStyleFactory>
#include <QSettings>
#include <QSysInfo>
#include <QLibraryInfo>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QCommandLineParser>
#include <QJsonObject>
#include <QScopeGuard>
#include <QtQml/qqmlengine.h>
#include <QtQml/qjsengine.h>
#include <QtQuickControls2/QQuickStyle>
#include <QQuickWindow>
#include "mainwindow.h"
#include "MaterialEditorQML.h"
#include "QMLMaterialHighlighter.h"
#include "LLMManager.h"
#include "AIModelCatalog.h"
#ifdef ENABLE_STABLE_DIFFUSION
#include "SDManager.h"
#endif
#include "ModelDownloader.h"
#include "PropertiesPanelController.h"
#include "ThemeManager.h"
#include "MCPServer.h"
#include "SentryReporter.h"
#include "CLIPipeline.h"
#include "AppConsoleLog.h"
#include "AppLaunchHandler.h"
#ifdef ENABLE_AUTO_UPDATER
#include "updater/UpdaterController.h"
#include "updater/UpdaterTelemetry.h"
#endif

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#include <cstdio>
#endif

#ifdef Q_OS_LINUX
static void forceX11PlatformIfNeeded()
{
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "xcb");
    }
}
#endif

static void setSentrySessionTags(const QString& launchMode)
{
    SentryReporter::configureSession(launchMode);
}

#ifdef Q_OS_MACOS
// When the app is launched via a SYMLINK that lives OUTSIDE the .app bundle
// (Homebrew installs `/opt/homebrew/bin/qtmesheditor -> …/QtMeshEditor.app/
// Contents/MacOS/QtMeshEditor`), Qt resolves applicationDirPath() to the symlink
// directory, so it never finds `Contents/Resources/qt.conf` and can't locate the
// bundled Qt plugins / QML modules under `Contents/PlugIns`. The GUI opens but
// every QQuickWidget (Inspector, mode bar, Context panel, menus) renders BLANK
// WHITE because its QML fails to load. Double-clicking the .app works because
// Launch Services runs the binary with the bundle as its context.
//
// Fix: resolve the REAL executable path (follow the symlink), and if it sits in
// `…/X.app/Contents/MacOS`, point Qt's plugin + QML import search at the bundle's
// `PlugIns` / `PlugIns/qml` explicitly. Must run before QApplication so the env
// vars take effect before Qt probes its paths. No-op when already launched inside
// the bundle (the paths just get re-affirmed).
// Returns the bundle PlugIns dir it pointed Qt at, or an empty string if it did
// nothing (already inside a bundle / not a bundle layout / no PlugIns).
static QString fixBundlePathsForSymlinkLaunch(const char* argv0)
{
    QString token = QString::fromLocal8Bit(argv0);
    if (token.isEmpty())
        return {};

    // A bare command name (no '/') means the shell ran us via PATH lookup
    // (`qtmesheditor`), so argv0 is just "qtmesheditor" — QFileInfo would resolve
    // it against $PWD and miss the bundle. Resolve it against PATH first.
    if (!token.contains(QLatin1Char('/'))) {
        const QString onPath = QStandardPaths::findExecutable(token);
        if (!onPath.isEmpty())
            token = onPath;
    }

    // Resolve the token (and any symlinks) to an absolute canonical path.
    QFileInfo exeInfo(token);
    QString exePath = exeInfo.canonicalFilePath();   // follows symlinks
    if (exePath.isEmpty())
        exePath = exeInfo.absoluteFilePath();
    if (exePath.isEmpty())
        return {};

    // …/Contents/MacOS/<binary>  →  bundle Contents dir is two levels up.
    QDir macosDir = QFileInfo(exePath).absoluteDir();          // Contents/MacOS
    if (macosDir.dirName() != QLatin1String("MacOS"))
        return {};                                             // not a bundle layout
    QDir contentsDir = macosDir;
    if (!contentsDir.cdUp())                                   // → Contents
        return {};

    const QString plugins = contentsDir.absoluteFilePath(QStringLiteral("PlugIns"));
    if (!QDir(plugins).exists())
        return {};                                             // nothing to point at
    const QString qmlDir = QDir(plugins).absoluteFilePath(QStringLiteral("qml"));

    // Only override when Qt would otherwise look in the WRONG place — i.e. the
    // running dir (applicationDirPath equivalent from argv0's own, unresolved
    // location) differs from the real bundle. Setting them unconditionally is
    // harmless, but we gate on existence above to avoid clobbering a dev-SDK run.
    qputenv("QT_PLUGIN_PATH", plugins.toLocal8Bit());
    qputenv("QML_IMPORT_PATH", qmlDir.toLocal8Bit());
    qputenv("QML2_IMPORT_PATH", qmlDir.toLocal8Bit());
    return plugins;
}
#endif

int main(int argc, char *argv[])
{
#ifdef Q_OS_LINUX
    // Ogre's externalWindowHandle requires X11 window IDs.
    // Under Wayland, Qt's winId() returns an incompatible surface handle,
    // causing Ogre to render to the wrong target (black viewport).
    forceX11PlatformIfNeeded();
#endif

    // CLI pipeline mode detection — check before creating QApplication
    if (AppLaunchHandler::isCliInvocation(argc, argv)) {
#ifdef Q_OS_WIN
            // QtMeshEditor.exe is a WIN32 GUI subsystem executable — it has no
            // console by default. Reattach to the parent console (PowerShell/cmd)
            // so that CLI output (--version, --help, subcommands) is visible.
            // Only redirect stdout/stderr if AttachConsole succeeded; otherwise
            // freopen("CONOUT$") fails and leaves stdout in an invalid state,
            // causing a crash on any subsequent write.
            if (AttachConsole(ATTACH_PARENT_PROCESS)) {
                freopen("CONOUT$", "w", stdout);
                freopen("CONOUT$", "w", stderr);
            }
#endif
        return CLIPipeline::run(argc, argv);
    }

    // Check for MCP server mode before creating QApplication
    bool mcpOnlyMode = false;
    bool mcpWithGuiMode = false;
    int httpPort = 8080;
    for (int i = 1; i < argc; ++i) {
        QString arg = QString(argv[i]);
        if (arg == "--mcp" || arg == "-mcp") {
            mcpOnlyMode = true;
        } else if (arg == "--with-mcp") {
            mcpWithGuiMode = true;
        } else if (arg == "--http-port" && i + 1 < argc) {
            httpPort = QString(argv[++i]).toInt();
        }
    }

    // When running in MCP mode, redirect stdout to stderr so that
    // Ogre/Qt debug output doesn't interfere with MCP JSON-RPC protocol.
    // The original stdout fd is saved and passed to MCPServer for responses.
    int savedStdoutFd = -1;
    if (mcpOnlyMode || mcpWithGuiMode) {
#ifndef Q_OS_WIN
        savedStdoutFd = dup(STDOUT_FILENO);
        dup2(STDERR_FILENO, STDOUT_FILENO);
#endif
    }

    if (mcpOnlyMode) {
        // MCP Server mode - runs as console application without GUI
        QCoreApplication a(argc, argv);
        QCoreApplication::setOrganizationName("QtMeshEditor");
        QCoreApplication::setOrganizationDomain("none");
        QCoreApplication::setApplicationName("QtMeshEditor");
        QCoreApplication::setApplicationVersion(QTMESHEDITOR_VERSION);

        // Initialize Sentry using stored consent (no dialog in headless mode)
        SentryReporter::initialize();
        auto sentryClose = qScopeGuard([] { SentryReporter::shutdown(); });

        setSentrySessionTags("mcp");
        SentryReporter::captureTelemetryEvent(QStringLiteral("app.startup"),
            QJsonObject{{QStringLiteral("launch_mode"), QStringLiteral("mcp")}});

        MCPServer server;
        // Note: In standalone MCP mode, we don't have a MainWindow
        server.setOutputFd(savedStdoutFd);
        server.start();
        server.startHttp(httpPort);

        return a.exec();
    }

    // Normal GUI mode (optionally with MCP server)
    QString bundlePluginsApplied;   // non-empty if the macOS symlink fix ran
#ifdef Q_OS_MACOS
    // Make a Homebrew CLI-symlink launch (qtmesheditor) find the bundle's Qt
    // plugins / QML modules, so its QQuickWidgets don't render blank white.
    bundlePluginsApplied = fixBundlePathsForSymlinkLaunch(argv[0]);
#endif

    // Set Qt Quick Controls style before creating QApplication
    // This prevents issues with native macOS style not supporting customization
    QQuickStyle::setStyle("Basic");

    // Force the Qt Quick *software* scene-graph backend BEFORE QApplication.
    // Every QML surface (the Inspector / Context / Material QQuickWidgets in
    // their docks, the ViewCube, etc.) runs software-rendered to avoid GL/Metal
    // conflicts with Ogre's direct-to-native rendering. The MainWindow ctor used
    // to set this, but by then Qt has already probed and locked the default RHI
    // (Metal/GL) on first QQuickWidget init — too late. In a deployed .app the
    // embedded dock QQuickWidgets then fail to composite and render BLANK WHITE
    // (issue: Homebrew build shows white Inspector/Context panels) while a
    // dev-SDK run happened to still paint. `QSGRendererInterface::setGraphicsApi`
    // / `QSG_RHI_BACKEND` only take effect if set before the scene graph
    // initialises, so they belong here, ahead of QApplication.
    qputenv("QSG_RHI_BACKEND", "software");
    qputenv("QT_QUICK_BACKEND", "software");
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);

    QApplication a(argc, argv);

    // Capture qDebug/qWarning/etc. from the rest of startup into the in-app console
    // (MainWindow attaches and drains the backlog when its console exists).
    AppConsoleLog::install();
    (void)AppConsoleLog::installStdioCapture();
    auto appConsoleShutdown = qScopeGuard([] { AppConsoleLog::shutdown(); });

    // Ensure Qt can find QML modules when running from an installed location.
    // When Qt libraries are bundled with the app, Qt may not find system QML modules
    // automatically. Add the app-local qml directory and the system QML path.
    QString appDir = QCoreApplication::applicationDirPath();
    QStringList qmlImportPaths;
    qmlImportPaths << appDir + "/qml";
    qmlImportPaths << QLibraryInfo::path(QLibraryInfo::QmlImportsPath);
#ifdef Q_OS_MACOS
    // On macOS the bundled QML lives under Contents/PlugIns/qml (deployed by
    // macdeployqt), NOT <appdir>/qml. Add the bundle's PlugIns + PlugIns/qml so a
    // symlink launch (Homebrew CLI) resolves them even though applicationDirPath()
    // points at the symlink dir — mirrors fixBundlePathsForSymlinkLaunch() above.
    {
        const QByteArray envPlugins = qgetenv("QT_PLUGIN_PATH");
        if (!envPlugins.isEmpty()) {
            const QString plugins = QString::fromLocal8Bit(envPlugins);
            a.addLibraryPath(plugins);
            qmlImportPaths << QDir(plugins).absoluteFilePath(QStringLiteral("qml"));
        }
    }
#endif
    for (const QString &path : qmlImportPaths) {
        a.addLibraryPath(path);
    }
    // Set environment variable as fallback for QML engines
    QByteArray existingPaths = qgetenv("QML2_IMPORT_PATH");
    QByteArray newPaths = qmlImportPaths.join(QDir::listSeparator()).toUtf8();
    if (!existingPaths.isEmpty()) {
        newPaths = newPaths + QDir::listSeparator().toLatin1() + existingPaths;
    }
    qputenv("QML2_IMPORT_PATH", newPaths);

    QCoreApplication::setOrganizationName("QtMeshEditor");
    QCoreApplication::setOrganizationDomain("none");
    QCoreApplication::setApplicationName("QtMeshEditor");
    QCoreApplication::setApplicationVersion(QTMESHEDITOR_VERSION);

    a.setStyle(QStyleFactory::create("Fusion"));
    ThemeManager::applySavedThemeFromSettings();

    // Sentry crash reporting: show consent dialog on first launch
    // (requires QApplication to exist for QMessageBox)
    if (SentryReporter::isFirstLaunch()) {
        SentryReporter::showConsentDialog();
    }

    // Initialize Sentry and ensure sentry_close() is called on exit
    SentryReporter::initialize();
    auto sentryClose = qScopeGuard([] { SentryReporter::shutdown(); });

    setSentrySessionTags(mcpWithGuiMode ? "gui+mcp" : "gui");
    SentryReporter::captureTelemetryEvent(QStringLiteral("app.startup"),
        QJsonObject{{QStringLiteral("launch_mode"), mcpWithGuiMode ? QStringLiteral("gui+mcp") : QStringLiteral("gui")}});

    // Note when the macOS symlink-launch bundle-path fix kicked in (Homebrew CLI
    // launch), so a blank-QML report can be correlated with the plugin path used.
    if (!bundlePluginsApplied.isEmpty())
        SentryReporter::addBreadcrumb(QStringLiteral("app.startup"),
            QStringLiteral("macOS symlink launch — QT_PLUGIN_PATH set to %1").arg(bundlePluginsApplied));

#ifdef ENABLE_AUTO_UPDATER
    for (int i = 1; i < argc; ++i) {
        if (QString::fromUtf8(argv[i]) == QLatin1String("--no-update-check")) {
            UpdaterController::setSessionBackgroundChecksDisabled(true);
            UpdaterTelemetry::breadcrumb(QStringLiteral("updater.background.skip"),
                                         QStringLiteral("session_disabled"));
        }
    }
#endif

    // Register QML types
    qmlRegisterSingletonType<MaterialEditorQML>("MaterialEditorQML", 1, 0, "MaterialEditorQML",
        [](QQmlEngine *engine, QJSEngine *scriptEngine) -> QObject* {
            return MaterialEditorQML::qmlInstance(engine, scriptEngine);
        });

    // Register LLMManager singleton for QML
    qmlRegisterSingletonType<LLMManager>("MaterialEditorQML", 1, 0, "LLMManager",
        [](QQmlEngine *engine, QJSEngine *scriptEngine) -> QObject* {
            return LLMManager::qmlInstance(engine, scriptEngine);
        });

    // Register ModelDownloader singleton for QML
    qmlRegisterSingletonType<ModelDownloader>("MaterialEditorQML", 1, 0, "ModelDownloader",
        [](QQmlEngine *engine, QJSEngine *scriptEngine) -> QObject* {
            return ModelDownloader::qmlInstance(engine, scriptEngine);
        });

    qmlRegisterSingletonType<AIModelCatalog>("MaterialEditorQML", 1, 0, "AIModelCatalog",
        [](QQmlEngine *engine, QJSEngine *scriptEngine) -> QObject* {
            return AIModelCatalog::qmlInstance(engine, scriptEngine);
        });

#ifdef ENABLE_STABLE_DIFFUSION
    // Register SDManager singleton for QML
    qmlRegisterSingletonType<SDManager>("MaterialEditorQML", 1, 0, "SDManager",
        [](QQmlEngine *engine, QJSEngine *scriptEngine) -> QObject* {
            return SDManager::qmlInstance(engine, scriptEngine);
        });

    // Eagerly create SDManager so it scans for models at startup
    SDManager::instance();
#endif

    // Register QMLMaterialHighlighter for QML use
    qmlRegisterType<QMLMaterialHighlighter>("MaterialEditorQML", 1, 0, "MaterialHighlighter");

    // Register PropertiesPanelController singleton for QML
    qmlRegisterSingletonType<PropertiesPanelController>("PropertiesPanel", 1, 0, "PropertiesPanelController",
        [](QQmlEngine *engine, QJSEngine *scriptEngine) -> QObject* {
            return PropertiesPanelController::qmlInstance(engine, scriptEngine);
        });

    // Register ThemeManager singleton for QML
    qmlRegisterSingletonType<ThemeManager>("ThemeManager", 1, 0, "ThemeManager",
        [](QQmlEngine *engine, QJSEngine *scriptEngine) -> QObject* {
            return ThemeManager::qmlInstance(engine, scriptEngine);
        });

    const QStringList launchPaths = AppLaunchHandler::collectGuiLaunchPaths(a.arguments());
    AppLaunchHandler launchHandler;
    if (!launchPaths.isEmpty() && launchHandler.tryForwardToRunningInstance(launchPaths))
        return 0;
    launchHandler.startSingleInstanceServer();

    // Buffer OS file requests until MainWindow exists (welcome dialog is modal).
    QStringList queuedLaunchPaths = launchPaths;
    QObject::connect(&launchHandler, &AppLaunchHandler::filesRequested, &a,
                     [&queuedLaunchPaths](const QStringList& paths) {
                         queuedLaunchPaths.append(paths);
                     });

    // Show welcome dialog before creating MainWindow (skip when OS opened a file)
    QString welcomeOpenFile;
    bool welcomeNewScene = false;
    if (launchPaths.isEmpty() && WelcomeDialog::shouldShow()) {
        WelcomeDialog welcome;
        welcome.exec();
        if (welcome.userAction() == WelcomeDialog::OpenFile ||
            welcome.userAction() == WelcomeDialog::OpenRecent) {
            welcomeOpenFile = welcome.selectedFile();
        } else if (welcome.userAction() == WelcomeDialog::NewScene) {
            welcomeNewScene = true;
        }
    }

    int result = 0;
    try {
        auto startupTxn = SentryReporter::startTransaction("app.startup", "app.load");
        auto startupTxnClose = qScopeGuard([&] { SentryReporter::finishTransaction(startupTxn); });
        MainWindow w;
        w.show();

        QObject::disconnect(&launchHandler, &AppLaunchHandler::filesRequested, nullptr, nullptr);
        QObject::connect(&launchHandler, &AppLaunchHandler::filesRequested, &w,
                         &MainWindow::openLaunchFiles);
        QObject::connect(&launchHandler, &AppLaunchHandler::cloudProjectOpenRequested, &w,
                         &MainWindow::openCloudProjectFromDeepLink);

        if (!queuedLaunchPaths.isEmpty()) {
            QTimer::singleShot(0, &w, [&w, queuedLaunchPaths]() {
                w.openLaunchFiles(queuedLaunchPaths);
            });
        } else if (!welcomeOpenFile.isEmpty()) {
            QTimer::singleShot(0, &w, [&w, welcomeOpenFile]() {
                w.loadFile(welcomeOpenFile);
            });
        }

        // Start MCP server alongside GUI if requested
        if (mcpWithGuiMode) {
            auto *mcpServer = new MCPServer(&w);
            mcpServer->setMainWindow(&w);
            mcpServer->setOutputFd(savedStdoutFd);
            mcpServer->start();
            mcpServer->startHttp(httpPort);
            w.setMCPServer(mcpServer);  // MainWindow takes ownership
            qDebug() << "MCP Server started alongside GUI";
        }

        result = a.exec();
    } catch (const std::exception& e) {
        QMessageBox::critical(nullptr, "QtMeshEditor - Startup Error",
            QString("QtMeshEditor failed to start:\n\n%1\n\n"
                    "Please ensure your GPU drivers are up to date and OpenGL is available.")
                .arg(QString::fromStdString(e.what())));
        result = 1;
    } catch (...) {
        QMessageBox::critical(nullptr, "QtMeshEditor - Startup Error",
            "QtMeshEditor failed to start due to an unexpected error.\n\n"
            "Please ensure your GPU drivers are up to date and OpenGL is available.");
        result = 1;
    }

    // SentryReporter::shutdown() is called automatically via qScopeGuard

    return result;
}
