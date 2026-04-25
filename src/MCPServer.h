#ifndef MCPSERVER_H
#define MCPSERVER_H

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QFile>
#include <QSocketNotifier>
#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QMap>
#include <QTimer>
#include <functional>
#include <memory>

namespace Ogre { class SceneNode; class Entity; }

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
     * @brief Start the HTTP REST API server
     * @param port TCP port to listen on (default 8080)
     * @return true if the server started successfully
     */
    bool startHttp(int port = 8080);

    /**
     * @brief Call a tool by name with arguments (public API for HTTP and AI Chat)
     */
    QJsonObject callTool(const QString &name, const QJsonObject &args);

    /**
     * @brief Build the list of available tools (public API for AI Chat)
     */
    QJsonArray buildToolsList();

    /**
     * @brief Set the main window reference for accessing editor functionality
     */
    void setMainWindow(QObject *mainWindow);

    /**
     * @brief Set the file descriptor for MCP output (instead of stdout)
     * Used when stdout is redirected to stderr to avoid mixing with Ogre output
     */
    void setOutputFd(int fd);

    /**
     * @brief Stop the HTTP REST API server
     */
    void stopHttp();

    /**
     * @brief Check if the HTTP server is currently running
     */
    bool isHttpRunning() const;

    /**
     * @brief Get the configured HTTP port
     */
    int httpPort() const;

    /**
     * @brief Check if server is running
     */
    bool isRunning() const { return m_running; }

    /**
     * @brief Mark Ogre initialization as failed (for testing without GL context)
     */
    void setOgreInitFailed(bool failed) { m_ogreInitFailed = failed; }

signals:
    void messageReceived(const QJsonObject &message);
    void errorOccurred(const QString &error);

private slots:
    void onReadyRead();
    void onHttpConnection();

private:
    using ToolHandler = QJsonObject (MCPServer::*)(const QJsonObject &);

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
    QJsonObject toolTransformSubMesh(const QJsonObject &args);
    QJsonObject toolListTextures(const QJsonObject &args);
    QJsonObject toolSetTexture(const QJsonObject &args);
    QJsonObject toolExportMesh(const QJsonObject &args);
    QJsonObject toolGetSceneInfo(const QJsonObject &args);
    QJsonObject toolTakeScreenshot(const QJsonObject &args);
    QJsonObject toolCreatePrimitive(const QJsonObject &args);
    QJsonObject toolAnimate(const QJsonObject &args);
    QJsonObject toolListSkeletalAnimations(const QJsonObject &args);
    QJsonObject toolGetAnimationInfo(const QJsonObject &args);
    QJsonObject toolSetAnimationLength(const QJsonObject &args);
    QJsonObject toolSetAnimationTime(const QJsonObject &args);
    QJsonObject toolAddKeyframe(const QJsonObject &args);
    QJsonObject toolRemoveKeyframe(const QJsonObject &args);
    QJsonObject toolPlayAnimation(const QJsonObject &args);
    QJsonObject toolToggleSkeletonDebug(const QJsonObject &args);
    QJsonObject toolToggleBoneWeights(const QJsonObject &args);
    QJsonObject toolToggleNormals(const QJsonObject &args);
    QJsonObject toolToggleMeshInfo(const QJsonObject &args);
    QJsonObject toolMergeAnimations(const QJsonObject &args);
    QJsonObject toolResampleAnimation(const QJsonObject &args);
    QJsonObject toolSimplifyAnimation(const QJsonObject &args);
    QJsonObject toolAnalyzeAnimation(const QJsonObject &args);
    QJsonObject toolSaveScene(const QJsonObject &args);
    QJsonObject toolOpenScene(const QJsonObject &args);
    QJsonObject toolValidateMesh(const QJsonObject &args);
    QJsonObject toolGenerateLods(const QJsonObject &args);
    QJsonObject toolGenerateAutoLods(const QJsonObject &args);
    QJsonObject toolRemoveLods(const QJsonObject &args);
    QJsonObject toolGetLodInfo(const QJsonObject &args);
    QJsonObject toolListFiles(const QJsonObject &args);
    QJsonObject toolSearchFiles(const QJsonObject &args);
    QJsonObject toolReadFile(const QJsonObject &args);
    QJsonObject toolCameraControl(const QJsonObject &args);
    QJsonObject toolGetCameraInfo(const QJsonObject &args);
    QJsonObject toolDeleteEntity(const QJsonObject &args);
    QJsonObject toolDuplicateEntity(const QJsonObject &args);
    QJsonObject toolSetSnapSettings(const QJsonObject &args);
    QJsonObject toolGetSnapSettings(const QJsonObject &args);
    QJsonObject toolExportPose(const QJsonObject &args);
    QJsonObject toolGroupNodes(const QJsonObject &args);
    QJsonObject toolUngroupNode(const QJsonObject &args);
    QJsonObject toolReparentNode(const QJsonObject &args);
    QJsonObject toolSetPivotMode(const QJsonObject &args);
    QJsonObject toolGetPivotMode(const QJsonObject &args);

    // Animation
    struct NodeAnimation {
        Ogre::SceneNode* node;
        float yawSpeed;   // degrees per second
        float pitchSpeed;
        float rollSpeed;
    };
    QMap<QString, NodeAnimation> m_animations;
    QTimer *m_animationTimer = nullptr;
    void onAnimationTick();

    // Skeleton entity resolution helper (shared by toggle_skeleton_debug / toggle_bone_weights)
    struct SkeletonEntityResult {
        Ogre::Entity* entity = nullptr;
        class AnimationWidget* animWidget = nullptr;
        QJsonObject error; // populated if validation failed
    };
    SkeletonEntityResult resolveSkeletonEntity(const QString &entityName);

    // Helper methods
    static QJsonObject makeErrorResult(const QString &message);
    static QJsonObject makeSuccessResult(const QString &message);
    static const QMap<QString, ToolHandler>& toolHandlers();
    static bool isHeavyTool(const QString &name);
    bool ensureOgreInitialized();
    QJsonObject buildToolDefinition(const QString &name, const QString &description,
                                     const QJsonObject &inputSchema);

    bool m_ogreInitialized = false;
    bool m_ogreInitFailed = false;

    // HTTP server
    void handleHttpRequest(QTcpSocket *socket);
    QTcpServer *m_httpServer = nullptr;
    int m_httpPort = 8080;
    QMap<QTcpSocket*, QByteArray> m_httpBuffers;

    // Member variables
    QObject *m_mainWindow = nullptr;
    QSocketNotifier *m_stdinNotifier = nullptr;
    int m_stdinFd = -1;
    int m_stdoutFd = -1;
    bool m_running = false;
    bool m_initialized = false;
    bool m_httpBusy = false;  // Guard against re-entrant tool execution
    QByteArray m_buffer;

    // MCP Protocol version
    static constexpr const char* MCP_VERSION = "2024-11-05";
    static constexpr const char* SERVER_NAME = "QtMeshEditor";
    static constexpr const char* SERVER_VERSION = "1.3.0";
};

#endif // MCPSERVER_H
