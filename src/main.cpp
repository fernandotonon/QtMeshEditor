#include <QApplication>
#include <QCoreApplication>
#include <QPalette>
#include <QDebug>
#include <QTimer>
#include <QStyleFactory>
#include <QSettings>
#include <QLibraryInfo>
#include <QCommandLineParser>
#include <QtQml/qqmlengine.h>
#include <QtQml/qjsengine.h>
#include <QtQuickControls2/QQuickStyle>
#include "mainwindow.h"
#include "MaterialEditorQML.h"
#include "QMLMaterialHighlighter.h"
#include "LLMManager.h"
#include "ModelDownloader.h"
#include "MCPServer.h"

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

int main(int argc, char *argv[])
{
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

    return result;
}
