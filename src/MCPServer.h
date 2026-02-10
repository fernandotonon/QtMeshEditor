#ifndef MCPSERVER_H
#define MCPSERVER_H

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTextStream>
#include <QFile>
#include <QSocketNotifier>
#include <QCoreApplication>
#include <functional>
#include <memory>

class MainWindow;

/**
 * @brief MCP (Model Context Protocol) Server for QtMeshEditor
 *
 * This server allows AI agents (Claude, Cursor, etc.) to interact with
 * QtMeshEditor programmatically via the MCP protocol.
 *
 * MCP uses JSON-RPC 2.0 over stdio for communication.
 */
class MCPServer : public QObject
{
    Q_OBJECT

public:
    explicit MCPServer(QObject *parent = nullptr);
    ~MCPServer();

    /**
     * @brief Start the MCP server in stdio mode
     * Reads from stdin, writes to stdout
     */
    void start();

    /**
     * @brief Stop the MCP server
     */
    void stop();

    /**
     * @brief Set the main window reference for accessing editor functionality
     */
    void setMainWindow(MainWindow *mainWindow);

    /**
     * @brief Check if server is running
     */
    bool isRunning() const { return m_running; }

signals:
    void messageReceived(const QJsonObject &message);
    void errorOccurred(const QString &error);

private slots:
    void onReadyRead();

private:
    // Message handling
    void processMessage(const QByteArray &data);
    void sendResponse(const QJsonObject &response);
    void sendError(const QJsonValue &id, int code, const QString &message);
    void sendNotification(const QString &method, const QJsonObject &params);

    // MCP Protocol handlers
    QJsonObject handleInitialize(const QJsonObject &params);
    QJsonObject handleToolsList();
    QJsonObject handleToolsCall(const QJsonObject &params);
    QJsonObject handleResourcesList();
    QJsonObject handleResourcesRead(const QJsonObject &params);

    // Tool implementations
    QJsonObject toolCreateMaterial(const QJsonObject &args);
    QJsonObject toolModifyMaterial(const QJsonObject &args);
    QJsonObject toolGetMaterial(const QJsonObject &args);
    QJsonObject toolListMaterials(const QJsonObject &args);
    QJsonObject toolApplyMaterial(const QJsonObject &args);
    QJsonObject toolLoadMesh(const QJsonObject &args);
    QJsonObject toolGetMeshInfo(const QJsonObject &args);
    QJsonObject toolTransformMesh(const QJsonObject &args);
    QJsonObject toolListTextures(const QJsonObject &args);
    QJsonObject toolSetTexture(const QJsonObject &args);
    QJsonObject toolExportMesh(const QJsonObject &args);
    QJsonObject toolGetSceneInfo(const QJsonObject &args);
    QJsonObject toolTakeScreenshot(const QJsonObject &args);

    // Helper methods
    QJsonArray buildToolsList();
    QJsonObject buildToolDefinition(const QString &name, const QString &description,
                                     const QJsonObject &inputSchema);

    // Member variables
    MainWindow *m_mainWindow = nullptr;
    QSocketNotifier *m_stdinNotifier = nullptr;
    QFile m_stdin;
    QTextStream m_stdout;
    bool m_running = false;
    bool m_initialized = false;
    QByteArray m_buffer;

    // MCP Protocol version
    static constexpr const char* MCP_VERSION = "2024-11-05";
    static constexpr const char* SERVER_NAME = "QtMeshEditor";
    static constexpr const char* SERVER_VERSION = "1.0.0";
};

#endif // MCPSERVER_H
