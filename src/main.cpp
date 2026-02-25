#include <QApplication>
#include <QCoreApplication>
#include <QPalette>
#include <QDebug>
#include <QTimer>
#include <QStyleFactory>
#include <QSettings>
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
#include "ModelDownloader.h"
#include "MCPServer.h"
#include "SentryReporter.h"
#include "AnimationMerger.h"
#include "MeshImporterExporter.h"
#include "Manager.h"
#include "SelectionSet.h"
#include <QWidget>

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

int main(int argc, char *argv[])
{
#ifdef Q_OS_LINUX
    // Ogre's externalWindowHandle requires X11 window IDs.
    // Under Wayland, Qt's winId() returns an incompatible surface handle,
    // causing Ogre to render to the wrong target (black viewport).
    forceX11PlatformIfNeeded();
#endif

    // Check for MCP server mode and merge-animations CLI before creating QApplication
    bool mcpOnlyMode = false;
    bool mcpWithGuiMode = false;
    bool mergeMode = false;
    int httpPort = 8080;
    QString mergeBase, mergeOutput;
    QStringList mergeAnimFiles;
    for (int i = 1; i < argc; ++i) {
        QString arg = QString(argv[i]);
        if (arg == "--mcp" || arg == "-mcp") {
            mcpOnlyMode = true;
        } else if (arg == "--with-mcp") {
            mcpWithGuiMode = true;
        } else if (arg == "--http-port" && i + 1 < argc) {
            httpPort = QString(argv[++i]).toInt();
        } else if (arg == "merge-animations") {
            mergeMode = true;
        } else if (mergeMode && arg == "--base" && i + 1 < argc) {
            mergeBase = QString(argv[++i]);
        } else if (mergeMode && arg == "--output" && i + 1 < argc) {
            mergeOutput = QString(argv[++i]);
        } else if (mergeMode && arg == "--animations") {
            while (i + 1 < argc && QString(argv[i + 1]).left(2) != "--") {
                mergeAnimFiles.append(QString(argv[++i]));
            }
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

    if (mergeMode) {
        // CLI merge-animations mode — needs QApplication + a hidden render window
        // because Ogre requires a GL context to create entity hardware buffers.
        QApplication a(argc, argv);
        QCoreApplication::setOrganizationName("QtMeshEditor");
        QCoreApplication::setOrganizationDomain("none");
        QCoreApplication::setApplicationName("QtMeshEditor");
        QCoreApplication::setApplicationVersion(QTMESHEDITOR_VERSION);

        if (mergeBase.isEmpty() || mergeOutput.isEmpty()) {
            qCritical() << "Usage: qtmesheditor merge-animations --base <file> --animations <file1> [file2 ...] --output <file>";
            return 1;
        }

        // Init Ogre and create a hidden render window for the GL context
        Manager::getSingleton();
        QWidget hiddenWidget;
        hiddenWidget.setAttribute(Qt::WA_DontShowOnScreen);
        hiddenWidget.resize(1, 1);
        hiddenWidget.show();

        Ogre::NameValuePairList params;
        params["externalWindowHandle"] = Ogre::StringConverter::toString(hiddenWidget.winId());
#ifdef Q_OS_MACOS
        params["macAPI"] = "cocoa";
        params["macAPICocoaUseNSView"] = "true";
#endif
        Manager::getSingleton()->getRoot()->createRenderWindow(
            "MergeHidden", 1, 1, false, &params);

        // Load base file and verify it loaded
        MeshImporterExporter::importer({mergeBase});
        {
            auto& baseEntities = Manager::getSingleton()->getEntities();
            if (baseEntities.isEmpty()) {
                qCritical().noquote() << "Failed to load base file:" << mergeBase;
                Manager::kill();
                return 1;
            }
        }

        // Load animation files
        for (const auto& f : mergeAnimFiles)
            MeshImporterExporter::importer({f});

        // Get all loaded entities (getEntities() rebuilds the list each call)
        auto& entities = Manager::getSingleton()->getEntities();
        if (entities.size() < 2) {
            qCritical() << "Need at least 2 loaded entities to merge (got" << entities.size() << ")";
            Manager::kill();
            return 1;
        }

        QString err;
        Ogre::Entity* merged = AnimationMerger::mergeAnimations(entities.first(), entities, err);
        if (!merged) {
            qCritical().noquote() << "Merge failed:" << err;
            Manager::kill();
            return 1;
        }

        // Determine export format from file extension
        auto formatForExtension = [](const QString& path) -> QString {
            if (path.endsWith(".glb2")) return "glTF 2.0 Binary (*.glb2)";
            if (path.endsWith(".gltf2")) return "glTF 2.0 (*.gltf2)";
            if (path.endsWith(".dae")) return "Collada (*.dae)";
            if (path.endsWith(".obj")) return "OBJ (*.obj)";
            if (path.endsWith(".stl")) return "STL (*.stl)";
            if (path.endsWith(".mesh.xml")) return "Ogre XML (*.mesh.xml)";
            if (path.endsWith(".mesh")) return "Ogre Mesh (*.mesh)";
            return "Ogre Mesh (*.mesh)";
        };

        auto* node = merged->getParentSceneNode();
        int result = MeshImporterExporter::exporter(node, mergeOutput, formatForExtension(mergeOutput));
        if (result != 0) {
            qCritical() << "Export failed";
            Manager::kill();
            return 1;
        }

        qDebug().noquote() << "Merged" << entities.size() << "files ->" << mergeOutput;
        Manager::kill();
        return 0;
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

    // Register QMLMaterialHighlighter for QML use
    qmlRegisterType<QMLMaterialHighlighter>("MaterialEditorQML", 1, 0, "MaterialHighlighter");

    MainWindow w;
    w.show();

    // Start MCP server alongside GUI if requested
    MCPServer *mcpServer = nullptr;
    if (mcpWithGuiMode) {
        mcpServer = new MCPServer(&w);
        mcpServer->setMainWindow(&w);
        mcpServer->setOutputFd(savedStdoutFd);
        mcpServer->start();
        mcpServer->startHttp(httpPort);
        qDebug() << "MCP Server started alongside GUI";
    }

    int result = a.exec();

    // Cleanup
    if (mcpServer) {
        mcpServer->stop();
        delete mcpServer;
    }

    // SentryReporter::shutdown() is called automatically via qScopeGuard

    return result;
}
