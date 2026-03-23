#include <QApplication>
#include <QCoreApplication>
#include <QPalette>
#include <QDebug>
#include <QTimer>
#include <QStyleFactory>
#include <QSettings>
#include <QSysInfo>
#include <QLibraryInfo>
#include <QCommandLineParser>
#include <QScopeGuard>
#include <QtQml/qqmlengine.h>
#include <QtQml/qjsengine.h>
#include <QtQuickControls2/QQuickStyle>
#include "mainwindow.h"
#include "MaterialEditorQML.h"
#include "QMLMaterialHighlighter.h"
#include "LLMManager.h"
#ifdef ENABLE_STABLE_DIFFUSION
#include "SDManager.h"
#endif
#include "ModelDownloader.h"
#include "PropertiesPanelController.h"
#include "AnimationTimelineController.h"
#include "ThemeManager.h"
#include "MCPServer.h"
#include "SentryReporter.h"
#include "CLIPipeline.h"

#ifndef Q_OS_WIN
#include <unistd.h>
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
    {
        bool cliMode = false;
        QString execName = QFileInfo(QString(argv[0])).fileName().toLower();
        if (execName.startsWith("qtmesh") && !execName.contains("editor")) {
            cliMode = true;
        }
        for (int i = 1; i < argc; ++i) {
            QString arg(argv[i]);
            if (arg == "--cli" || arg == "--help" || arg == "-h" ||
                arg == "--version" || arg == "-v") {
                cliMode = true;
                break;
            }
        }
        if (!cliMode) {
            for (int i = 1; i < argc; ++i) {
                QString arg(argv[i]);
                if (arg.startsWith("-"))
                    continue;  // skip flags like --verbose
                if (arg == "info" || arg == "fix" || arg == "convert" || arg == "anim")
                    cliMode = true;
                break;  // first non-flag arg determines mode
            }
        }
        if (cliMode)
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

    QApplication a(argc, argv);

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
    QByteArray newPaths = qmlImportPaths.join(":").toUtf8();
    if (!existingPaths.isEmpty()) {
        newPaths = newPaths + ":" + existingPaths;
    }
    qputenv("QML2_IMPORT_PATH", newPaths);

    QCoreApplication::setOrganizationName("QtMeshEditor");
    QCoreApplication::setOrganizationDomain("none");
    QCoreApplication::setApplicationName("QtMeshEditor");
    QCoreApplication::setApplicationVersion(QTMESHEDITOR_VERSION);

    a.setStyle(QStyleFactory::create("Fusion"));

    // Sentry crash reporting: show consent dialog on first launch
    // (requires QApplication to exist for QMessageBox)
    if (SentryReporter::isFirstLaunch()) {
        SentryReporter::showConsentDialog();
    }

    // Initialize Sentry and ensure sentry_close() is called on exit
    SentryReporter::initialize();
    auto sentryClose = qScopeGuard([] { SentryReporter::shutdown(); });

    setSentrySessionTags(mcpWithGuiMode ? "gui+mcp" : "gui");

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

    // Register AnimationTimelineController singleton for QML
    qmlRegisterSingletonType<AnimationTimelineController>("AnimationTimeline", 1, 0, "AnimationTimelineController",
        [](QQmlEngine *engine, QJSEngine *scriptEngine) -> QObject* {
            return AnimationTimelineController::qmlInstance(engine, scriptEngine);
        });

    // Register ThemeManager singleton for QML
    qmlRegisterSingletonType<ThemeManager>("ThemeManager", 1, 0, "ThemeManager",
        [](QQmlEngine *engine, QJSEngine *scriptEngine) -> QObject* {
            return ThemeManager::qmlInstance(engine, scriptEngine);
        });

    auto startupTxn = SentryReporter::startTransaction("app.startup", "app.load");
    auto startupTxnClose = qScopeGuard([&] { SentryReporter::finishTransaction(startupTxn); });
    MainWindow w;
    w.show();

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

    int result = a.exec();

    // SentryReporter::shutdown() is called automatically via qScopeGuard

    return result;
}
