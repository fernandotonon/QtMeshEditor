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
#include <QCommandLineParser>
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
    SentryReporter::setTag("os", QSysInfo::prettyProductName());
    SentryReporter::setTag("arch", QSysInfo::currentCpuArchitecture());
    SentryReporter::setTag("qt_version", qVersion());
    SentryReporter::setTag("launch_mode", launchMode);
}

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

        MCPServer server;
        // Note: In standalone MCP mode, we don't have a MainWindow
        server.setOutputFd(savedStdoutFd);
        server.start();
        server.startHttp(httpPort);

        return a.exec();
    }

    // Normal GUI mode (optionally with MCP server)
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
    // automatically. Add both the app-local qml directory and the system QML path.
    QString appDir = QCoreApplication::applicationDirPath();
    QStringList qmlImportPaths;
    qmlImportPaths << appDir + "/qml";
    qmlImportPaths << QLibraryInfo::path(QLibraryInfo::QmlImportsPath);
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
