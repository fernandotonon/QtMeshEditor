#include "MCPServer.h"
#include "mainwindow.h"
#include "Manager.h"
#include "MaterialEditorQML.h"
#include "MaterialPresetLibrary.h"
#include "TextureChannelPacker.h"
#include "TextureAtlasPacker.h"
#include "ApplyAtlas.h"
#include "NormalMapGenerator.h"
#include "VATBaker.h"
#include "MorphAnimationManager.h"
#include "NodeAnimationManager.h"
#include "PoseLibrary.h"
#include "PrimitiveObject.h"
#include "SelectionSet.h"
#include "TransformOperator.h"
#include "MeshImporterExporter.h"
#include "CLIPipeline.h"
#include "OgreWidget.h"
#include "SpaceCamera.h"
#include "AnimationWidget.h"
#include "NormalVisualizer.h"
#include "MeshInfoOverlay.h"
#include "MeshValidator.h"
#include "MeshLodController.h"
#include "MemoryEstimator.h"
#include "DrawCallAnalyzer.h"
#include "VertexCacheOptimizer.h"
#include "MeshDecimator.h"
#include "UvUnwrap.h"
#include "AssetScanController.h"
#include "CloudCredentialStore.h"
#include "DependencyResolver.h"
#include "ProjectPackager.h"
#include "QtMeshCloudClient.h"
#include "QtMeshCloudSession.h"
#include "ScanConfig.h"
#include "ScanEngine.h"
#include "QuadRetopo.h"
#include "SkinWeights.h"
#include "MeshDepthRenderer.h"
#include "ModelIsometricRenderer.h"
#ifdef ENABLE_STABLE_DIFFUSION
#include "SDManager.h"
#endif
#ifdef ENABLE_ONNX
#include "AIAssistManager.h"
#include "PbrMapSynth.h"
#include "RTShaderHelper.h"
#endif
#include <QEventLoop>
#include <QDebug>
#include <QFile>
#include <QDir>
#include <QScopeGuard>
#include <QTemporaryFile>
#include <QImage>
#include <QBuffer>
#include "SentryReporter.h"
#include <QTimer>
#include <QDateTime>
#include <QMetaObject>
#include <QPixmap>
#include <QSet>
#include <OgreException.h>
#include <OgreMaterialManager.h>
#include <OgreMaterial.h>
#include <OgreTechnique.h>
#include <OgrePass.h>
#include <OgreMaterialSerializer.h>
#include <OgreTextureManager.h>
#include <OgreEntity.h>
#include <OgreSubEntity.h>
#include <OgreSubMesh.h>
#include <OgreMesh.h>
#include <cmath>
#include <OgreSkeleton.h>
#include <OgreAnimation.h>
#include <OgreAnimationState.h>
#include <OgreKeyFrame.h>
#include <OgreBone.h>
#include "AnimationMerger.h"
#include "SubMeshTransform.h"
#include "UndoManager.h"
#include "commands/TransformCommands.h"

#ifdef Q_OS_WIN
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#endif

static Ogre::SceneNode* findSceneNodeByName(const QString &nodeName);
static Ogre::Entity* findEntityByName(const QString &entityName);
static Ogre::NodeAnimationTrack* findTrackByBoneName(Ogre::Animation* anim, const QString &boneName);
static bool hasSelectedEntities();
static QString captureLodControllerError(const std::function<void()> &operation);

MCPServer::MCPServer(QObject *parent)
    : QObject(parent)
{
#ifdef Q_OS_WIN
    // Set stdin/stdout to binary mode on Windows
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

MCPServer::~MCPServer()
{
    stop();
}

void MCPServer::setMainWindow(QObject *mainWindow)
{
    m_mainWindow = mainWindow;
}

void MCPServer::setOutputFd(int fd)
{
    m_stdoutFd = fd;
}

void MCPServer::start()
{
    if (m_running) return;

    m_stdinFd = fileno(stdin);

    // Create notifier for stdin using the raw file descriptor
    m_stdinNotifier = new QSocketNotifier(m_stdinFd, QSocketNotifier::Read, this);
    connect(m_stdinNotifier, &QSocketNotifier::activated, this, &MCPServer::onReadyRead);

    m_running = true;
    qDebug() << "MCP Server started";
}

void MCPServer::stop()
{
    stopHttp();

    if (!m_running) return;

    if (m_stdinNotifier) {
        m_stdinNotifier->setEnabled(false);
        delete m_stdinNotifier;
        m_stdinNotifier = nullptr;
    }

    m_running = false;
    qDebug() << "MCP Server stopped";
}

void MCPServer::stopHttp()
{
    if (m_httpServer) {
        m_httpServer->close();
        delete m_httpServer;
        m_httpServer = nullptr;
        qDebug() << "HTTP REST API stopped";
    }
}

bool MCPServer::isHttpRunning() const
{
    return m_httpServer && m_httpServer->isListening();
}

int MCPServer::httpPort() const
{
    return m_httpPort;
}

void MCPServer::onReadyRead()
{
    // Read available data directly from file descriptor (not C FILE*)
    char buf[4096];
    ssize_t bytesRead = read(m_stdinFd, buf, sizeof(buf));
    if (bytesRead <= 0) {
        // EOF or error - disable notifier to prevent busy loop
        if (m_stdinNotifier)
            m_stdinNotifier->setEnabled(false);
        // In headless MCP mode (no GUI), quit when the client disconnects
        if (!m_mainWindow) {
            qDebug() << "MCP: stdin closed, shutting down";
            QCoreApplication::quit();
        }
        return;
    }
    QByteArray data(buf, bytesRead);

    m_buffer.append(data);

    // MCP uses Content-Length header like LSP
    // Format: Content-Length: <length>\r\n\r\n<json>
    while (!m_buffer.isEmpty()) {
        // Look for Content-Length header
        int headerEnd = m_buffer.indexOf("\r\n\r\n");
        if (headerEnd == -1) break;

        QString header = QString::fromUtf8(m_buffer.left(headerEnd));
        if (!header.startsWith("Content-Length:")) {
            // Invalid header, try to recover
            m_buffer.remove(0, 1);
            continue;
        }

        bool ok;
        int contentLength = header.mid(16).trimmed().toInt(&ok);
        if (!ok || contentLength <= 0) {
            m_buffer.remove(0, headerEnd + 4);
            continue;
        }

        int messageStart = headerEnd + 4;
        int totalLength = messageStart + contentLength;

        if (m_buffer.size() < totalLength) {
            // Wait for more data
            break;
        }

        QByteArray messageData = m_buffer.mid(messageStart, contentLength);
        m_buffer.remove(0, totalLength);

        processMessage(messageData);
    }
}

void MCPServer::processMessage(const QByteArray &data)
{
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        sendError(QJsonValue::Null, -32700, "Parse error: " + parseError.errorString());
        return;
    }

    if (!doc.isObject()) {
        sendError(QJsonValue::Null, -32600, "Invalid Request: expected object");
        return;
    }

    QJsonObject request = doc.object();
    QString method = request["method"].toString();
    QJsonValue id = request["id"];
    QJsonObject params = request["params"].toObject();

    qDebug() << "MCP Request:" << method;

    // MCP notifications (no "id" field) must not receive a response.
    bool isNotification = !request.contains("id");

    // Handle MCP notifications (no response sent)
    if (method == "initialized" || method == "notifications/initialized") {
        // Post-handshake notification — nothing to do
        return;
    }
    if (method == "notifications/cancelled") {
        return;
    }

    // All other notifications are silently ignored per MCP spec
    if (isNotification) {
        qDebug() << "MCP: ignoring unknown notification:" << method;
        return;
    }

    QJsonObject result;

    // Handle MCP request methods
    if (method == "initialize") {
        result = handleInitialize(params);
    } else if (method == "tools/list") {
        result = handleToolsList();
    } else if (method == "tools/call") {
        result = handleToolsCall(params);
    } else if (method == "resources/list") {
        result = handleResourcesList();
    } else if (method == "resources/read") {
        result = handleResourcesRead(params);
    } else if (method == "ping") {
        result = QJsonObject();
    } else {
        sendError(id, -32601, "Method not found: " + method);
        return;
    }

    // Send response (only for requests with an id)
    QJsonObject response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["result"] = result;

    sendResponse(response);
}

void MCPServer::sendResponse(const QJsonObject &response)
{
    QJsonDocument doc(response);
    QByteArray data = doc.toJson(QJsonDocument::Compact);

    QByteArray header = QString("Content-Length: %1\r\n\r\n").arg(data.size()).toUtf8();
    QByteArray output = header + data;

    // Write to the saved stdout fd (not current stdout which may be redirected to stderr)
    const char *ptr = output.constData();
    qint64 remaining = output.size();
    while (remaining > 0) {
        ssize_t written = write(m_stdoutFd, ptr, remaining);
        if (written <= 0) break;
        ptr += written;
        remaining -= written;
    }
}

void MCPServer::sendError(const QJsonValue &id, int code, const QString &message)
{
    QJsonObject error;
    error["code"] = code;
    error["message"] = message;

    QJsonObject response;
    response["jsonrpc"] = "2.0";
    response["id"] = id;
    response["error"] = error;

    sendResponse(response);
}

void MCPServer::sendNotification(const QString &method, const QJsonObject &params)
{
    QJsonObject notification;
    notification["jsonrpc"] = "2.0";
    notification["method"] = method;
    notification["params"] = params;

    sendResponse(notification);
}

QJsonObject MCPServer::handleInitialize(const QJsonObject &params)
{
    Q_UNUSED(params);

    m_initialized = true;

    QJsonObject capabilities;
    capabilities["tools"] = QJsonObject();
    capabilities["resources"] = QJsonObject();

    QJsonObject serverInfo;
    serverInfo["name"] = SERVER_NAME;
    serverInfo["version"] = SERVER_VERSION;

    QJsonObject result;
    result["protocolVersion"] = MCP_VERSION;
    result["capabilities"] = capabilities;
    result["serverInfo"] = serverInfo;

    return result;
}

QJsonObject MCPServer::handleToolsList()
{
    QJsonObject result;
    result["tools"] = buildToolsList();
    return result;
}

QJsonObject MCPServer::handleToolsCall(const QJsonObject &params)
{
    QString toolName = params["name"].toString();
    QJsonObject args = params["arguments"].toObject();
    return callTool(toolName, args);
}

bool MCPServer::ensureOgreInitialized()
{
    if (m_ogreInitialized) return true;
    if (m_ogreInitFailed) return false;

    try {
        if (!Manager::getSingletonPtr()) {
            Manager::getSingleton();
        }
        m_ogreInitialized = true;
        return true;
    } catch (const Ogre::Exception &e) {
        qWarning() << "MCP: Ogre init failed:" << e.getFullDescription().c_str();
    }
    m_ogreInitFailed = true;
    return false;
}

QJsonObject MCPServer::makeErrorResult(const QString &message)
{
    QJsonObject textContent;
    textContent["type"] = "text";
    textContent["text"] = message;
    QJsonArray content;
    content.append(textContent);
    QJsonObject result;
    result["isError"] = true;
    result["content"] = content;
    return result;
}

QJsonObject MCPServer::makeSuccessResult(const QString &message)
{
    QJsonObject textContent;
    textContent["type"] = "text";
    textContent["text"] = message;
    QJsonArray content;
    content.append(textContent);
    QJsonObject result;
    result["content"] = content;
    return result;
}

namespace {

/// RAII helper: tools that need to temporarily import a mesh
/// without touching the user's live scene state.
///
/// On construction:
///   - Snapshots the current entity list (filtered to actual
///     `Ogre::Entity` objects — `Manager::getEntities()` returns
///     `MovableObject*` which includes ManualObjects for gizmos and
///     the like).
///   - Snapshots the SelectionSet's three lists. This matters
///     because `MeshImporterExporter::importer` auto-selects the
///     entities it creates; without restoring afterward the
///     SelectionSet would dangle at destroyed pointers as soon as
///     the destructor runs (CodeRabbit/Codex finding on PR #571).
///
/// `runImporter()` calls `MeshImporterExporter::importer` with
/// exception guards and returns true/false. After it runs,
/// `importedEntities()` lists the entities created by this import
/// only (the diff against the pre-import snapshot).
///
/// On destruction:
///   - Destroys every scene node parent of the imported entities.
///   - `selSet->clearList()` (not `clear()`) — wipes the importer-
///     added selection entries without dereferencing them
///     (they're freed memory by this point).
///   - Re-appends the snapshotted pre-import selection.
class TransientImportSession {
public:
    explicit TransientImportSession(Manager* mgr)
        : m_mgr(mgr)
        , m_selSet(SelectionSet::getSingleton())
    {
        if (!m_mgr) return;
        for (Ogre::Entity* e : collectEntitiesSafe()) m_beforeSet.insert(e);
        if (m_selSet) {
            m_prevNodes = m_selSet->getNodesSelectionList();
            m_prevEnts  = m_selSet->getEntitiesSelectionList();
            m_prevSubs  = m_selSet->getSubEntitiesSelectionList();
        }
    }

    /// Run the importer for `filePath`. Returns the error message
    /// on exception, or empty on success.
    ///
    /// Even on the error path, any entities the importer managed to
    /// create before throwing are recorded into `m_imported` — so
    /// the destructor still cleans up the partial state. Without
    /// this, a half-successful import would leak scene nodes into
    /// the user's live scene.
    QString runImporter(const QString& filePath)
    {
        if (!m_mgr) return QStringLiteral("no Manager singleton");
        try {
            MeshImporterExporter::importer({filePath});
        } catch (const std::exception& e) {
            captureImportedEntities();
            return QStringLiteral("Importer threw: %1").arg(QString::fromUtf8(e.what()));
        } catch (...) {
            captureImportedEntities();
            return QStringLiteral("Importer threw (unknown exception)");
        }
        captureImportedEntities();
        return {};
    }

    const QList<Ogre::Entity*>& importedEntities() const { return m_imported; }

    ~TransientImportSession()
    {
        if (!m_mgr) return;
        // Destroy newly-created scene-node parents (one per imported
        // mesh; siblings sharing a parent are uncommon for raw imports
        // but the set dedupes anyway).
        try {
            std::set<Ogre::SceneNode*> nodes;
            for (Ogre::Entity* e : m_imported) {
                if (e && e->getParentSceneNode()) nodes.insert(e->getParentSceneNode());
            }
            for (Ogre::SceneNode* sn : nodes) m_mgr->destroySceneNode(sn);
        } catch (...) {}

        // Restore selection — see class doc for rationale.
        if (m_selSet) {
            try {
                m_selSet->clearList();
                for (auto* n : m_prevNodes) if (n) m_selSet->append(n);
                for (auto* e : m_prevEnts)  if (e) m_selSet->append(e);
                for (auto* s : m_prevSubs)  if (s) m_selSet->append(s);
            } catch (...) {}
        }
    }

    TransientImportSession(const TransientImportSession&) = delete;
    TransientImportSession& operator=(const TransientImportSession&) = delete;

private:
    /// Append every entity that's currently in the scene but wasn't
    /// in `m_beforeSet` (i.e. created by this session) to `m_imported`.
    /// Idempotent + dedup'd against `m_imported`, so calling it from
    /// both the success path and the exception paths is safe.
    void captureImportedEntities()
    {
        for (Ogre::Entity* e : collectEntitiesSafe()) {
            if (!m_beforeSet.contains(e) && !m_imported.contains(e))
                m_imported.append(e);
        }
    }

    QList<Ogre::Entity*> collectEntitiesSafe() const
    {
        QList<Ogre::Entity*> out;
        const auto& nodes = m_mgr->getSceneNodes();
        for (Ogre::SceneNode* node : nodes) {
            if (!node) continue;
            for (int i = 0; i < static_cast<int>(node->numAttachedObjects()); ++i) {
                Ogre::MovableObject* obj = node->getAttachedObject(i);
                if (!obj || obj->getMovableType() != "Entity") continue;
                out.append(static_cast<Ogre::Entity*>(obj));
            }
        }
        return out;
    }

    Manager*                m_mgr = nullptr;
    SelectionSet*           m_selSet = nullptr;
    QSet<Ogre::Entity*>     m_beforeSet;
    QList<Ogre::Entity*>    m_imported;
    QList<Ogre::SceneNode*> m_prevNodes;
    QList<Ogre::Entity*>    m_prevEnts;
    QList<Ogre::SubEntity*> m_prevSubs;
};

} // namespace

const QMap<QString, MCPServer::ToolHandler>& MCPServer::toolHandlers()
{
    static const QMap<QString, ToolHandler> handlers = {
        {QStringLiteral("create_material"), &MCPServer::toolCreateMaterial},
        {QStringLiteral("modify_material"), &MCPServer::toolModifyMaterial},
        {QStringLiteral("get_material"), &MCPServer::toolGetMaterial},
        {QStringLiteral("list_materials"), &MCPServer::toolListMaterials},
        {QStringLiteral("apply_material"), &MCPServer::toolApplyMaterial},
        {QStringLiteral("list_material_presets"), &MCPServer::toolListMaterialPresets},
        {QStringLiteral("apply_material_preset"), &MCPServer::toolApplyMaterialPreset},
        {QStringLiteral("describe_material"), &MCPServer::toolDescribeMaterial},
        {QStringLiteral("load_mesh"), &MCPServer::toolLoadMesh},
        {QStringLiteral("get_mesh_info"), &MCPServer::toolGetMeshInfo},
        {QStringLiteral("transform_mesh"), &MCPServer::toolTransformMesh},
        {QStringLiteral("transform_submesh"), &MCPServer::toolTransformSubMesh},
        {QStringLiteral("list_textures"), &MCPServer::toolListTextures},
        {QStringLiteral("set_texture"), &MCPServer::toolSetTexture},
        {QStringLiteral("export_mesh"), &MCPServer::toolExportMesh},
        {QStringLiteral("auto_uv_unwrap"),       &MCPServer::toolAutoUvUnwrap},
        {QStringLiteral("retopologize"),         &MCPServer::toolRetopologize},
        {QStringLiteral("compute_skin_weights"), &MCPServer::toolComputeSkinWeights},
        {QStringLiteral("generate_mesh_texture"), &MCPServer::toolGenerateMeshTexture},
        {QStringLiteral("generate_pbr_maps"), &MCPServer::toolGeneratePbrMaps},
        {QStringLiteral("upscale_texture"), &MCPServer::toolUpscaleTexture},
        {QStringLiteral("get_scene_info"), &MCPServer::toolGetSceneInfo},
        {QStringLiteral("take_screenshot"), &MCPServer::toolTakeScreenshot},
        {QStringLiteral("create_primitive"), &MCPServer::toolCreatePrimitive},
        {QStringLiteral("animate"), &MCPServer::toolAnimate},
        {QStringLiteral("list_skeletal_animations"), &MCPServer::toolListSkeletalAnimations},
        {QStringLiteral("get_animation_info"), &MCPServer::toolGetAnimationInfo},
        {QStringLiteral("set_animation_length"), &MCPServer::toolSetAnimationLength},
        {QStringLiteral("set_animation_time"), &MCPServer::toolSetAnimationTime},
        {QStringLiteral("add_keyframe"), &MCPServer::toolAddKeyframe},
        {QStringLiteral("remove_keyframe"), &MCPServer::toolRemoveKeyframe},
        {QStringLiteral("play_animation"), &MCPServer::toolPlayAnimation},
        {QStringLiteral("toggle_skeleton_debug"), &MCPServer::toolToggleSkeletonDebug},
        {QStringLiteral("toggle_bone_weights"), &MCPServer::toolToggleBoneWeights},
        {QStringLiteral("toggle_normals"), &MCPServer::toolToggleNormals},
        {QStringLiteral("toggle_mesh_info"), &MCPServer::toolToggleMeshInfo},
        {QStringLiteral("merge_animations"), &MCPServer::toolMergeAnimations},
        {QStringLiteral("resample_animation"), &MCPServer::toolResampleAnimation},
        {QStringLiteral("simplify_animation"), &MCPServer::toolSimplifyAnimation},
        {QStringLiteral("analyze_animation"), &MCPServer::toolAnalyzeAnimation},
        {QStringLiteral("bake_animation_fps"), &MCPServer::toolBakeAnimationFps},
        {QStringLiteral("save_scene"), &MCPServer::toolSaveScene},
        {QStringLiteral("open_scene"), &MCPServer::toolOpenScene},
        {QStringLiteral("validate_mesh"), &MCPServer::toolValidateMesh},
        {QStringLiteral("generate_lods"), &MCPServer::toolGenerateLods},
        {QStringLiteral("generate_auto_lods"), &MCPServer::toolGenerateAutoLods},
        {QStringLiteral("remove_lods"), &MCPServer::toolRemoveLods},
        {QStringLiteral("get_lod_info"), &MCPServer::toolGetLodInfo},
        {QStringLiteral("get_memory_usage"), &MCPServer::toolGetMemoryUsage},
        {QStringLiteral("analyze_draw_calls"), &MCPServer::toolAnalyzeDrawCalls},
        {QStringLiteral("optimize_vertex_cache"), &MCPServer::toolOptimizeVertexCache},
        {QStringLiteral("decimate_mesh"), &MCPServer::toolDecimateMesh},
        {QStringLiteral("list_files"), &MCPServer::toolListFiles},
        {QStringLiteral("search_files"), &MCPServer::toolSearchFiles},
        {QStringLiteral("read_file"), &MCPServer::toolReadFile},
        {QStringLiteral("delete_entity"), &MCPServer::toolDeleteEntity},
        {QStringLiteral("duplicate_entity"), &MCPServer::toolDuplicateEntity},
        {QStringLiteral("camera_control"), &MCPServer::toolCameraControl},
        {QStringLiteral("get_camera_info"), &MCPServer::toolGetCameraInfo},
        {QStringLiteral("set_snap_settings"), &MCPServer::toolSetSnapSettings},
        {QStringLiteral("get_snap_settings"), &MCPServer::toolGetSnapSettings},
        {QStringLiteral("export_pose"), &MCPServer::toolExportPose},
        {QStringLiteral("group_nodes"), &MCPServer::toolGroupNodes},
        {QStringLiteral("ungroup_node"), &MCPServer::toolUngroupNode},
        {QStringLiteral("reparent_node"), &MCPServer::toolReparentNode},
        {QStringLiteral("set_pivot_mode"), &MCPServer::toolSetPivotMode},
        {QStringLiteral("get_pivot_mode"), &MCPServer::toolGetPivotMode},
        {QStringLiteral("pack_textures"), &MCPServer::toolPackTextures},
        {QStringLiteral("generate_normal_map"), &MCPServer::toolGenerateNormalMap},
        {QStringLiteral("pack_atlas"), &MCPServer::toolPackAtlas},
        {QStringLiteral("apply_atlas"), &MCPServer::toolApplyAtlas},
        {QStringLiteral("optimize_mesh"), &MCPServer::toolOptimizeMesh},
        {QStringLiteral("generate_isometric_sprites"), &MCPServer::toolGenerateIsometricSprites},
        {QStringLiteral("bake_vat"), &MCPServer::toolBakeVat},
        {QStringLiteral("list_morph_targets"), &MCPServer::toolListMorphTargets},
        {QStringLiteral("set_morph_weight"), &MCPServer::toolSetMorphWeight},
        {QStringLiteral("list_node_animations"), &MCPServer::toolListNodeAnimations},
        {QStringLiteral("add_node_animation_clip"), &MCPServer::toolAddNodeAnimationClip},
        {QStringLiteral("set_node_keyframe"), &MCPServer::toolSetNodeKeyframe},
        {QStringLiteral("list_poses"), &MCPServer::toolListPoses},
        {QStringLiteral("save_pose"), &MCPServer::toolSavePose},
        {QStringLiteral("apply_pose"), &MCPServer::toolApplyPose},
        {QStringLiteral("delete_pose"), &MCPServer::toolDeletePose},
        {QStringLiteral("mirror_pose"), &MCPServer::toolMirrorPose},
        {QStringLiteral("save_pose_library"), &MCPServer::toolSavePoseLibrary},
        {QStringLiteral("load_pose_library"), &MCPServer::toolLoadPoseLibrary},
        {QStringLiteral("apply_pose_masked"), &MCPServer::toolApplyPoseMasked},
        {QStringLiteral("cloud_status"), &MCPServer::toolCloudStatus},
        {QStringLiteral("cloud_login"), &MCPServer::toolCloudLogin},
        {QStringLiteral("cloud_logout"), &MCPServer::toolCloudLogout},
        {QStringLiteral("cloud_list_projects"), &MCPServer::toolCloudListProjects},
        {QStringLiteral("cloud_delete_project"), &MCPServer::toolCloudDeleteProject},
        {QStringLiteral("cloud_upload"), &MCPServer::toolCloudUpload}
    };
    return handlers;
}

bool MCPServer::isHeavyTool(const QString &name)
{
    static const QSet<QString> heavyTools = {
        QStringLiteral("load_mesh"),
        QStringLiteral("export_mesh"),
        QStringLiteral("export_pose"),
        QStringLiteral("take_screenshot"),
        QStringLiteral("create_primitive"),
        QStringLiteral("create_material"),
        QStringLiteral("merge_animations"),
        QStringLiteral("resample_animation"),
        QStringLiteral("simplify_animation"),
        QStringLiteral("bake_animation_fps"),
        QStringLiteral("save_scene"),
        QStringLiteral("open_scene"),
        QStringLiteral("bake_vat"),
        QStringLiteral("list_morph_targets"),
        QStringLiteral("cloud_upload")
    };
    return heavyTools.contains(name);
}

QJsonObject MCPServer::callTool(const QString &name, const QJsonObject &args)
{
    qDebug() << "MCP Tool Call:" << name << args;

    SentryReporter::addBreadcrumb("ai.tool_call", QStringLiteral("Tool call: %1").arg(name));

    uintptr_t txn = 0;
    if (isHeavyTool(name)) {
        txn = SentryReporter::startTransaction(QStringLiteral("mcp.%1").arg(name), "mcp.tool");
    }

    // Lazily initialize Ogre/Manager on first scene-dependent tool call
    if (!name.startsWith(QStringLiteral("cloud_")) && !ensureOgreInitialized()) {
        if (txn) SentryReporter::finishTransaction(txn);
        return makeErrorResult("Error: Ogre 3D engine could not be initialized (no OpenGL available)");
    }

    const auto& handlers = toolHandlers();
    const auto handlerIt = handlers.constFind(name);
    if (handlerIt == handlers.constEnd()) {
        if (txn) SentryReporter::finishTransaction(txn);
        return makeErrorResult(QString("Unknown tool: %1").arg(name));
    }

    QJsonObject toolResult = (this->*(handlerIt.value()))(args);

    // Track errors as breadcrumbs
    if (toolResult.contains("isError") && toolResult["isError"].toBool()) {
        SentryReporter::addBreadcrumb("ai.tool_call",
            QStringLiteral("Tool error: %1").arg(name), "error");
    }

    if (txn) SentryReporter::finishTransaction(txn);

    return toolResult;
}

QJsonObject MCPServer::handleResourcesList()
{
    QJsonArray resources;

    // Add current material as a resource
    QJsonObject materialResource;
    materialResource["uri"] = "qtmesheditor://material/current";
    materialResource["name"] = "Current Material";
    materialResource["description"] = "The currently selected material in the editor";
    materialResource["mimeType"] = "text/plain";
    resources.append(materialResource);

    // Add scene info as a resource
    QJsonObject sceneResource;
    sceneResource["uri"] = "qtmesheditor://scene/info";
    sceneResource["name"] = "Scene Information";
    sceneResource["description"] = "Information about the current scene";
    sceneResource["mimeType"] = "application/json";
    resources.append(sceneResource);

    QJsonObject result;
    result["resources"] = resources;
    return result;
}

QJsonObject MCPServer::handleResourcesRead(const QJsonObject &params)
{
    QString uri = params["uri"].toString();

    QJsonArray contents;

    if (uri == "qtmesheditor://material/current") {
        QJsonObject content;
        content["uri"] = uri;
        content["mimeType"] = "text/plain";

        // Get material text from the MaterialEditorQML if available
        QString materialText = "// No material currently loaded";
        if (m_mainWindow) {
            MaterialEditorQML* matEditor = m_mainWindow->findChild<MaterialEditorQML*>();
            if (matEditor && !matEditor->materialName().isEmpty()) {
                materialText = matEditor->materialText();
            }
        }
        content["text"] = materialText;
        contents.append(content);
    } else if (uri == "qtmesheditor://scene/info") {
        QJsonObject content;
        content["uri"] = uri;
        content["mimeType"] = "application/json";

        // Reuse the scene info tool to get real data
        QJsonObject sceneResult = toolGetSceneInfo(QJsonObject());
        QJsonArray sceneContent = sceneResult["content"].toArray();
        QString sceneText = "{}";
        if (!sceneContent.isEmpty()) {
            sceneText = sceneContent[0].toObject()["text"].toString();
        }
        content["text"] = sceneText;
        contents.append(content);
    }

    QJsonObject result;
    result["contents"] = contents;
    return result;
}

// Tool implementations

namespace {

/// Pull a color array from either a top-level `key` argument or a
/// nested `colors.key` argument. Used by toolCreateMaterial — both
/// MCP message shapes are valid, so we accept either.
QJsonArray resolveColorArg(const QJsonObject& args, const QString& key)
{
    if (args.contains(key) && args[key].isArray())
        return args[key].toArray();
    const QJsonObject nested = args["colors"].toObject();
    if (nested.contains(key) && nested[key].isArray())
        return nested[key].toArray();
    return {};
}

double resolveNumberArg(const QJsonObject& args, const QString& key, double def)
{
    if (args.contains(key)) return args[key].toDouble(def);
    return args["colors"].toObject().value(key).toDouble(def);
}

/// Apply ambient / diffuse / specular / emissive colours from the
/// MCP args onto an Ogre pass. Defaults match the historical
/// toolCreateMaterial behaviour. Extracted as a free function so
/// the parent handler stays under Sonar's complexity threshold.
void applyColorsToPass(Ogre::Pass* pass, const QJsonObject& args)
{
    const QJsonArray amb = resolveColorArg(args, "ambient");
    if (!amb.isEmpty())
        pass->setAmbient(amb[0].toDouble(0.2), amb[1].toDouble(0.2), amb[2].toDouble(0.2));
    else
        pass->setAmbient(0.2, 0.2, 0.2);

    const QJsonArray diff = resolveColorArg(args, "diffuse");
    if (!diff.isEmpty())
        pass->setDiffuse(diff[0].toDouble(1.0), diff[1].toDouble(1.0),
                         diff[2].toDouble(1.0), 1.0);

    const QJsonArray spec = resolveColorArg(args, "specular");
    if (!spec.isEmpty()) {
        pass->setSpecular(spec[0].toDouble(0.5), spec[1].toDouble(0.5),
                          spec[2].toDouble(0.5), 1.0);
        pass->setShininess(resolveNumberArg(args, "shininess", 32.0));
    } else {
        pass->setSpecular(0.5, 0.5, 0.5, 1.0);
        pass->setShininess(32.0);
    }

    const QJsonArray emis = resolveColorArg(args, "emissive");
    if (!emis.isEmpty())
        pass->setSelfIllumination(emis[0].toDouble(), emis[1].toDouble(),
                                  emis[2].toDouble());
}

} // namespace

QJsonObject MCPServer::toolCreateMaterial(const QJsonObject &args)
{
    const QString name = args["name"].toString();
    if (name.isEmpty()) {
        return makeErrorResult("Error: Material name is required");
    }
    try {
        Ogre::MaterialPtr existing = Ogre::MaterialManager::getSingleton().getByName(name.toStdString());
        if (existing) {
            return makeErrorResult(QString("Error: Material '%1' already exists").arg(name));
        }
        Ogre::MaterialPtr mat = Ogre::MaterialManager::getSingleton().create(
            name.toStdString(), Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        applyColorsToPass(mat->getTechnique(0)->getPass(0), args);
        try { mat->load(); } catch (...) { /* headless — no GPU context */ }
        Ogre::MaterialSerializer serializer;
        serializer.queueForExport(mat);
        const QString materialScript = QString::fromStdString(serializer.getQueuedAsString());
        return makeSuccessResult(QString("Created material '%1':\n%2").arg(name, materialScript));
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QStringLiteral("Ogre error: %1")
            .arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolModifyMaterial(const QJsonObject &args)
{
    QString name = args["name"].toString();

    if (name.isEmpty()) {
        return makeErrorResult("Error: Material name is required");
    }

    try {
        Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().getByName(name.toStdString());
        if (!material) {
            return makeErrorResult(QString("Error: Material '%1' not found").arg(name));
        }
        if (material->getNumTechniques() == 0) {
            return makeErrorResult(QString("Error: Material '%1' has no techniques").arg(name));
        }
        Ogre::Technique* technique = material->getTechnique(0);
        if (technique->getNumPasses() == 0) {
            return makeErrorResult(QString("Error: Material '%1' technique has no passes").arg(name));
        }
        Ogre::Pass* pass = technique->getPass(0);

        QStringList modifications;
        if (args.contains("ambient")) {
            QJsonArray a = args["ambient"].toArray();
            Ogre::ColourValue ambient(a[0].toDouble(), a[1].toDouble(), a[2].toDouble());
            pass->setAmbient(ambient);
            modifications << QString("ambient: %1 %2 %3")
                .arg(a[0].toDouble()).arg(a[1].toDouble()).arg(a[2].toDouble());
        }
        if (args.contains("diffuse")) {
            QJsonArray d = args["diffuse"].toArray();
            Ogre::ColourValue diffuse(d[0].toDouble(), d[1].toDouble(), d[2].toDouble());
            pass->setDiffuse(diffuse);
            modifications << QString("diffuse: %1 %2 %3")
                .arg(d[0].toDouble()).arg(d[1].toDouble()).arg(d[2].toDouble());
        }
        if (args.contains("specular")) {
            QJsonArray s = args["specular"].toArray();
            double shininess = args.value("shininess").toDouble(pass->getShininess());
            Ogre::ColourValue specular(s[0].toDouble(), s[1].toDouble(), s[2].toDouble());
            pass->setSpecular(specular);
            pass->setShininess(shininess);
            modifications << QString("specular: %1 %2 %3 (shininess: %4)")
                .arg(s[0].toDouble()).arg(s[1].toDouble()).arg(s[2].toDouble()).arg(shininess);
        }
        if (args.contains("emissive")) {
            QJsonArray e = args["emissive"].toArray();
            Ogre::ColourValue emissive(e[0].toDouble(), e[1].toDouble(), e[2].toDouble());
            pass->setSelfIllumination(emissive);
            modifications << QString("emissive: %1 %2 %3")
                .arg(e[0].toDouble()).arg(e[1].toDouble()).arg(e[2].toDouble());
        }
        return makeSuccessResult(QString("Modified material '%1':\n%2")
            .arg(name, modifications.join("\n")));
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QStringLiteral("Ogre error: %1")
            .arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolGetMaterial(const QJsonObject &args)
{
    const QString name = args["name"].toString();
    if (name.isEmpty()) {
        return makeErrorResult("Error: Material name is required");
    }
    try {
        Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().getByName(name.toStdString());
        if (!material) {
            return makeErrorResult(QString("Error: Material '%1' not found").arg(name));
        }
        Ogre::MaterialSerializer serializer;
        serializer.queueForExport(material);
        const QString script = QString::fromStdString(serializer.getQueuedAsString());
        return makeSuccessResult(QString("Material '%1' script:\n%2").arg(name, script));
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QStringLiteral("Ogre error: %1")
            .arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolListMaterials(const QJsonObject &args)
{
    Q_UNUSED(args);
    try {
        QStringList materials;
        auto& matMgr = Ogre::MaterialManager::getSingleton();
        auto it = matMgr.getResourceIterator();
        while (it.hasMoreElements()) {
            Ogre::ResourcePtr res = it.getNext();
            materials << QString::fromStdString(res->getName());
        }
        materials.sort();
        return makeSuccessResult(QString("Available materials (%1):\n%2")
            .arg(materials.size()).arg(materials.join("\n")));
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QStringLiteral("Ogre error: %1")
            .arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolApplyMaterial(const QJsonObject &args)
{
    // Accept common model variations: "material" or "material_name"
    QString materialName = args["material"].toString();
    if (materialName.isEmpty()) materialName = args["material_name"].toString();
    // Accept "mesh", "mesh_name", or "entity" / "entity_name"
    QString meshName = args["mesh"].toString();
    if (meshName.isEmpty()) meshName = args["mesh_name"].toString();
    if (meshName.isEmpty()) meshName = args["entity"].toString();
    if (meshName.isEmpty()) meshName = args["entity_name"].toString();

    if (materialName.isEmpty()) {
        return makeErrorResult("Error: Material name is required");
    }
    try {
        Ogre::MaterialPtr mat = Ogre::MaterialManager::getSingleton().getByName(materialName.toStdString());
        if (!mat) {
            return makeErrorResult(QString("Error: Material '%1' not found").arg(materialName));
        }
        Manager* mgr = Manager::getSingletonPtr();
        if (!mgr) {
            return makeErrorResult("Error: Manager not available");
        }
        QStringList appliedTo;
        if (!meshName.isEmpty()) {
            // Use findEntityByName (which already checks getMovableType
            // == "Entity") to avoid the ManualObject cast crash that
            // would happen if we iterated Manager::getEntities() and
            // static_cast'd every attached movable. CodeRabbit feedback
            // on PR #532.
            bool found = false;
            if (Ogre::Entity* entity = findEntityByName(meshName)) {
                entity->setMaterialName(materialName.toStdString());
                appliedTo << QString::fromStdString(entity->getName());
                found = true;
            }
            // Fallback: look up by scene-node name if the entity name
            // wasn't found (entity name can differ from node name when
            // the node was created with a custom label).
            if (!found) {
                Ogre::SceneNode* sn = findSceneNodeByName(meshName);
                if (sn) {
                    for (int i = 0; i < static_cast<int>(sn->numAttachedObjects()); ++i) {
                        Ogre::MovableObject* obj = sn->getAttachedObject(i);
                        if (obj && obj->getMovableType() == "Entity") {
                            Ogre::Entity* ent = static_cast<Ogre::Entity*>(obj);
                            ent->setMaterialName(materialName.toStdString());
                            appliedTo << QString::fromStdString(ent->getName());
                            found = true;
                            break;
                        }
                    }
                }
            }
            if (!found) {
                return makeErrorResult(QString("Error: Mesh '%1' not found").arg(meshName));
            }
        } else {
            SelectionSet* sel = SelectionSet::getSingleton();
            if (!sel || sel->getEntitiesCount() == 0) {
                return makeErrorResult("Error: No entity specified and no entities selected");
            }
            for (int i = 0; i < sel->getEntitiesCount(); ++i) {
                Ogre::Entity* entity = sel->getEntity(i);
                if (entity) {
                    entity->setMaterialName(materialName.toStdString());
                    appliedTo << QString::fromStdString(entity->getName());
                }
            }
        }
        return makeSuccessResult(QString("Applied material '%1' to: %2")
            .arg(materialName, appliedTo.join(", ")));
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QStringLiteral("Ogre error: %1")
            .arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolListMaterialPresets(const QJsonObject &)
{
    auto names = MaterialPresetLibrary::instance()->presetNames();
    QString out = QString("Material presets (%1):").arg(names.size());
    for (const auto& n : names)
        out += "\n  - " + n;
    return makeSuccessResult(out);
}

QJsonObject MCPServer::toolApplyMaterialPreset(const QJsonObject &args)
{
    QString preset = args["preset"].toString();
    if (preset.isEmpty()) preset = args["name"].toString();
    if (preset.isEmpty())
        return makeErrorResult("Error: 'preset' is required (use list_material_presets to see available names)");

    auto* lib = MaterialPresetLibrary::instance();
    if (!lib->presetNames().contains(preset))
        return makeErrorResult(QString("Error: Unknown preset '%1'").arg(preset));

    // Accept "mesh", "mesh_name", or "entity" / "entity_name" — the
    // preset library applies to whatever's in the SelectionSet. If the
    // caller specifies a target, briefly swap selection to that entity,
    // apply, then restore.
    QString meshName = args["mesh"].toString();
    if (meshName.isEmpty()) meshName = args["mesh_name"].toString();
    if (meshName.isEmpty()) meshName = args["entity"].toString();
    if (meshName.isEmpty()) meshName = args["entity_name"].toString();

    auto* sel = SelectionSet::getSingleton();
    if (!sel)
        return makeErrorResult("Error: SelectionSet not available");

    try {
        if (!meshName.isEmpty()) {
            if (!Manager::getSingletonPtr())
                return makeErrorResult("Error: Manager not available");

            // Use the safe getMovableType-checking helper rather than
            // iterating Manager::getEntities() and casting; the latter
            // crashes on ManualObjects mixed into the entity list.
            Ogre::Entity* target = findEntityByName(meshName);
            if (!target)
                return makeErrorResult(QString("Error: Mesh '%1' not found").arg(meshName));

            Ogre::SceneNode* parent = target->getParentSceneNode();
            if (!parent)
                return makeErrorResult(
                    QString("Error: Mesh '%1' is not attached to a scene node").arg(meshName));

            // Snapshot the FULL selection (nodes + sub-entities) and
            // restore it on every exit path — including exceptions from
            // applyPreset — via QScopeGuard. The previous version
            // snapshot only nodes and used early returns, leaking the
            // swapped selection on error and silently dropping any
            // active sub-entity selection on success.
            const auto prevNodes   = sel->getNodesSelectionList();
            const auto prevSubEnts = sel->getSubEntitiesSelectionList();
            auto restoreSelection = qScopeGuard([&] {
                sel->clear();
                for (auto* n : prevNodes)    sel->append(n);
                for (auto* se : prevSubEnts) sel->append(se);
            });

            sel->clear();
            sel->append(parent);
            lib->applyPreset(preset);
        } else {
            if (sel->getEntitiesCount() == 0
                && sel->getSubEntitiesSelectionList().isEmpty())
                return makeErrorResult("Error: No mesh specified and no entities selected");
            lib->applyPreset(preset);
        }
        return makeSuccessResult(QString("Applied preset '%1'").arg(preset));
    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolDescribeMaterial(const QJsonObject &args)
{
    const QString prompt = args["prompt"].toString().trimmed();
    if (prompt.isEmpty())
        return makeErrorResult(
            "Error: 'prompt' is required (a natural-language material description, "
            "e.g. \"rusty bronze armor\").");

    const QString modelName = args["model"].toString();   // optional GGUF override

    // Resolve the target entity: explicit name (mesh/entity/...) else selection.
    QString meshName = args["mesh"].toString();
    if (meshName.isEmpty()) meshName = args["mesh_name"].toString();
    if (meshName.isEmpty()) meshName = args["entity"].toString();
    if (meshName.isEmpty()) meshName = args["entity_name"].toString();

    if (!Manager::getSingletonPtr())
        return makeErrorResult("Error: Manager not available");

    Ogre::Entity* entity = nullptr;
    if (!meshName.isEmpty()) {
        entity = findEntityByName(meshName);
        if (!entity)
            return makeErrorResult(QString("Error: Mesh '%1' not found").arg(meshName));
    } else {
        // Fall back to the first selected entity (resolved through nodes too).
        auto* sel = SelectionSet::getSingleton();
        if (sel) {
            const auto resolved = sel->getResolvedEntities();
            if (!resolved.isEmpty()) entity = resolved.first();
        }
        if (!entity)
            return makeErrorResult(
                "Error: No mesh specified and no entity selected. Pass 'mesh' or "
                "select an entity first.");
    }

    // Record only safe metadata — the prompt is user-controlled and may carry
    // proprietary descriptions or pasted secrets we must not ship to telemetry.
    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.describe_material"),
        QStringLiteral("MCP describe_material prompt accepted (%1 chars)")
            .arg(prompt.size()));

    // Shared core (same as the CLI --describe path): loads/uses a local LLM,
    // generates + parses the material script, binds it to every submesh.
    QString error;
    const QString matName =
        CLIPipeline::llmDescribeMaterialToEntity(entity, prompt, modelName, error);
    if (matName.isEmpty())
        return makeErrorResult(QString("Error: %1").arg(error));

    // Optionally re-export the mesh with the new material baked into the asset.
    const QString outputPath = args["output_path"].toString();
    if (!outputPath.isEmpty()) {
        Ogre::SceneNode* node = entity->getParentSceneNode();
        if (!node)
            return makeErrorResult(
                QString("Error: material '%1' applied, but the entity has no scene "
                        "node to export from").arg(matName));
        SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
            QStringLiteral("MCP describe_material export to %1").arg(outputPath));
        const int rc = MeshImporterExporter::exporter(
            node, outputPath, CLIPipeline::formatForExtension(outputPath));
        if (rc != 0)
            return makeErrorResult(
                QString("Error: material '%1' applied but export to '%2' failed "
                        "(code %3)").arg(matName, outputPath).arg(rc));
        return makeSuccessResult(
            QString("Generated material '%1' from \"%2\" and saved to %3")
                .arg(matName, prompt, outputPath));
    }

    return makeSuccessResult(
        QString("Generated material '%1' from \"%2\" and applied it to %3")
            .arg(matName, prompt, meshName.isEmpty()
                 ? QStringLiteral("the selected mesh") : meshName));
}

QJsonObject MCPServer::toolLoadMesh(const QJsonObject &args)
{
    const QString path = args["path"].toString();
    if (path.isEmpty()) {
        return makeErrorResult("Error: File path is required");
    }
    MainWindow* mainWindow = qobject_cast<MainWindow*>(m_mainWindow);
    if (!mainWindow) {
        return makeErrorResult("Error: MainWindow not available. Run with --with-mcp flag for full functionality.");
    }
    if (!QFile::exists(path)) {
        return makeErrorResult(QString("Error: File not found: %1").arg(path));
    }
    try {
        mainWindow->importMeshs(QStringList{path});
        return makeSuccessResult(QString("Loaded mesh from: %1").arg(path));
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QStringLiteral("Ogre error: %1")
            .arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolGetMeshInfo(const QJsonObject &args)
{
    Q_UNUSED(args);
    try {
        Manager* mgr = Manager::getSingletonPtr();
        if (!mgr) {
            return makeErrorResult("Error: Manager not available");
        }
        // Check if there's a selection first, otherwise report all entities
        SelectionSet* sel = SelectionSet::getSingleton();
        QList<Ogre::Entity*> entitiesToReport;
        if (sel && sel->getEntitiesCount() > 0) {
            for (int i = 0; i < sel->getEntitiesCount(); ++i) {
                entitiesToReport.append(sel->getEntity(i));
            }
        } else {
            entitiesToReport = mgr->getEntities();
        }
        if (entitiesToReport.isEmpty()) {
            return makeSuccessResult("No entities in scene");
        }
        QStringList infoLines;
        for (Ogre::Entity* entity : entitiesToReport) {
            if (!entity) continue;
            const Ogre::MeshPtr& mesh = entity->getMesh();
            if (!mesh) continue;
            unsigned int totalVertices = 0;
            unsigned int totalIndices = 0;
            const unsigned int numSubMeshes = mesh->getNumSubMeshes();
            for (unsigned int i = 0; i < numSubMeshes; ++i) {
                Ogre::SubMesh* subMesh = mesh->getSubMesh(i);
                if (subMesh->vertexData)
                    totalVertices += subMesh->vertexData->vertexCount;
                if (subMesh->indexData)
                    totalIndices += subMesh->indexData->indexCount;
            }
            if (mesh->sharedVertexData)
                totalVertices += mesh->sharedVertexData->vertexCount;
            QStringList materials;
            for (unsigned int i = 0; i < entity->getNumSubEntities(); ++i) {
                Ogre::SubEntity* subEnt = entity->getSubEntity(i);
                if (subEnt && subEnt->getMaterial()) {
                    materials << QString::fromStdString(subEnt->getMaterial()->getName());
                }
            }
            Ogre::SceneNode* parentNode = entity->getParentSceneNode();
            Ogre::Vector3 pos = parentNode ? parentNode->getPosition() : Ogre::Vector3::ZERO;
            Ogre::Vector3 scale = parentNode ? parentNode->getScale() : Ogre::Vector3::UNIT_SCALE;
            infoLines << QString(
                "Entity: %1\n"
                "  Mesh: %2\n"
                "  Vertices: %3\n"
                "  Triangles: %4\n"
                "  SubMeshes: %5\n"
                "  Materials: %6\n"
                "  Position: %7, %8, %9\n"
                "  Scale: %10, %11, %12"
            ).arg(QString::fromStdString(entity->getName()))
             .arg(QString::fromStdString(mesh->getName()))
             .arg(totalVertices)
             .arg(totalIndices / 3)
             .arg(numSubMeshes)
             .arg(materials.join(", "))
             .arg(pos.x).arg(pos.y).arg(pos.z)
             .arg(scale.x).arg(scale.y).arg(scale.z);
        }
        return makeSuccessResult(QString("Mesh Information (%1 entities):\n\n%2")
            .arg(entitiesToReport.size())
            .arg(infoLines.join("\n\n")));
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QStringLiteral("Ogre error: %1")
            .arg(QString::fromStdString(e.getFullDescription())));
    }
}

// Helper: parse a vector from JSON (supports both array [x,y,z] and object {x,y,z})
static Ogre::Vector3 parseVector3(const QJsonValue &val) {
    if (val.isArray()) {
        QJsonArray a = val.toArray();
        return Ogre::Vector3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble());
    }
    if (val.isObject()) {
        QJsonObject o = val.toObject();
        return Ogre::Vector3(o["x"].toDouble(), o["y"].toDouble(), o["z"].toDouble());
    }
    return Ogre::Vector3::ZERO;
}

QJsonObject MCPServer::toolTransformMesh(const QJsonObject &args)
{
    try {
        if (!Manager::getSingletonPtr()) {
            return makeErrorResult("Error: Manager not available");
        }
        const QString name = args["name"].toString();
        Ogre::SceneNode* targetNode = nullptr;
        if (!name.isEmpty()) {
            targetNode = findSceneNodeByName(name);
            if (!targetNode) {
                return makeErrorResult(QString("Error: Node '%1' not found").arg(name));
            }
        } else {
            SelectionSet* sel = SelectionSet::getSingleton();
            if (!sel || sel->getNodesCount() == 0) {
                return makeErrorResult("Error: No name provided and no scene nodes selected.");
            }
            targetNode = sel->getNodesSelectionList().first();
        }
        QStringList transforms;
        if (args.contains("position")) {
            Ogre::Vector3 pos = parseVector3(args["position"]);
            targetNode->setPosition(pos);
            transforms << QString("position: %1, %2, %3").arg(pos.x).arg(pos.y).arg(pos.z);
        }
        if (args.contains("rotation")) {
            Ogre::Vector3 rot = parseVector3(args["rotation"]);
            Ogre::Quaternion q;
            q.FromAngleAxis(Ogre::Degree(rot.x), Ogre::Vector3::UNIT_X);
            Ogre::Quaternion qy; qy.FromAngleAxis(Ogre::Degree(rot.y), Ogre::Vector3::UNIT_Y);
            Ogre::Quaternion qz; qz.FromAngleAxis(Ogre::Degree(rot.z), Ogre::Vector3::UNIT_Z);
            targetNode->setOrientation(qz * qy * q);
            transforms << QString("rotation: %1, %2, %3").arg(rot.x).arg(rot.y).arg(rot.z);
        }
        if (args.contains("scale")) {
            Ogre::Vector3 scale = parseVector3(args["scale"]);
            targetNode->setScale(scale);
            transforms << QString("scale: %1, %2, %3").arg(scale.x).arg(scale.y).arg(scale.z);
        }
        return makeSuccessResult(QString("Applied transforms to '%1':\n%2")
            .arg(QString::fromStdString(targetNode->getName()), transforms.join("\n")));
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QStringLiteral("Ogre error: %1")
            .arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolTransformSubMesh(const QJsonObject &args)
{
    try {
        if (!Manager::getSingletonPtr())
            return makeErrorResult("Error: Manager not available");
        const QString entityName = args["entity_name"].toString();
        if (entityName.isEmpty())
            return makeErrorResult("Error: entity_name is required");
        const int subIdx = args["submesh_index"].toInt(-1);
        if (subIdx < 0)
            return makeErrorResult("Error: submesh_index must be a non-negative integer");
        Ogre::Entity* entity = findEntityByName(entityName);
        if (!entity)
            return makeErrorResult(QString("Error: Entity '%1' not found").arg(entityName));
        const unsigned int uSubIdx = static_cast<unsigned int>(subIdx);
        if (uSubIdx >= entity->getNumSubEntities())
            return makeErrorResult(QString("Error: submesh_index %1 out of range (entity has %2 sub-meshes)")
                .arg(subIdx).arg(entity->getNumSubEntities()));
        QStringList transforms;
        if (args.contains("translate")) {
            Ogre::Vector3 delta = parseVector3(args["translate"]);
            SubMeshTransform::translateSubMesh(entity, uSubIdx, delta);
            transforms << QString("translate: %1, %2, %3").arg(delta.x).arg(delta.y).arg(delta.z);
        }
        if (args.contains("rotate")) {
            Ogre::Vector3 rot = parseVector3(args["rotate"]);
            Ogre::Quaternion q;
            q.FromAngleAxis(Ogre::Degree(rot.x), Ogre::Vector3::UNIT_X);
            Ogre::Quaternion qy; qy.FromAngleAxis(Ogre::Degree(rot.y), Ogre::Vector3::UNIT_Y);
            Ogre::Quaternion qz; qz.FromAngleAxis(Ogre::Degree(rot.z), Ogre::Vector3::UNIT_Z);
            SubMeshTransform::rotateSubMesh(entity, uSubIdx, qz * qy * q);
            transforms << QString("rotate: %1, %2, %3").arg(rot.x).arg(rot.y).arg(rot.z);
        }
        if (args.contains("scale")) {
            Ogre::Vector3 scale = parseVector3(args["scale"]);
            SubMeshTransform::scaleSubMesh(entity, uSubIdx, scale);
            transforms << QString("scale: %1, %2, %3").arg(scale.x).arg(scale.y).arg(scale.z);
        }
        if (transforms.isEmpty())
            return makeErrorResult("Error: No transform specified. Provide translate, rotate, or scale.");
        SentryReporter::addBreadcrumb("ai.tool_call",
            QString("transform_submesh: %1[%2]").arg(entityName).arg(subIdx));
        return makeSuccessResult(QString("Applied sub-mesh transforms to '%1' submesh %2:\n%3")
            .arg(entityName).arg(subIdx).arg(transforms.join("\n")));
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QStringLiteral("Ogre error: %1")
            .arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolListTextures(const QJsonObject &args)
{
    Q_UNUSED(args);
    try {
        QStringList textures;
        auto& texMgr = Ogre::TextureManager::getSingleton();
        auto it = texMgr.getResourceIterator();
        while (it.hasMoreElements()) {
            Ogre::ResourcePtr res = it.getNext();
            textures << QString::fromStdString(res->getName());
        }
        textures.sort();
        return makeSuccessResult(QString("Available textures (%1):\n%2")
            .arg(textures.size())
            .arg(textures.isEmpty() ? "(none)" : textures.join("\n")));
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QStringLiteral("Ogre error: %1")
            .arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolSetTexture(const QJsonObject &args)
{
    const QString materialName = args["material"].toString();
    const QString texturePath = args["texture"].toString();
    const int textureUnit = args["unit"].toInt(0);
    if (materialName.isEmpty() || texturePath.isEmpty()) {
        return makeErrorResult("Error: Both material and texture names are required");
    }
    try {
        Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().getByName(materialName.toStdString());
        if (!material) {
            return makeErrorResult(QString("Error: Material '%1' not found").arg(materialName));
        }
        if (material->getNumTechniques() == 0 ||
            material->getTechnique(0)->getNumPasses() == 0) {
            return makeErrorResult(QString("Error: Material '%1' has no technique/pass").arg(materialName));
        }
        Ogre::Pass* pass = material->getTechnique(0)->getPass(0);
        if (static_cast<int>(pass->getNumTextureUnitStates()) > textureUnit) {
            pass->getTextureUnitState(textureUnit)->setTextureName(texturePath.toStdString());
        } else {
            pass->createTextureUnitState(texturePath.toStdString());
        }
        return makeSuccessResult(QString("Set texture '%1' on material '%2' (unit %3)")
            .arg(texturePath, materialName).arg(textureUnit));
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QStringLiteral("Ogre error: %1")
            .arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolExportMesh(const QJsonObject &args)
{
    const QString path = args["path"].toString();
    const QString format = args["format"].toString("Ogre Mesh (*.mesh)");
    if (path.isEmpty()) {
        return makeErrorResult("Error: Export path is required");
    }
    try {
        SelectionSet* sel = SelectionSet::getSingleton();
        if (!sel || sel->getNodesCount() == 0) {
            return makeErrorResult("Error: No scene nodes selected. Select an object to export.");
        }
        Ogre::SceneNode* node = sel->getSceneNode(0);
        if (!node) {
            return makeErrorResult("Error: Selected scene node is null");
        }
        const int exportResult = MeshImporterExporter::exporter(node, path, format);
        if (exportResult == 0) {
            return makeSuccessResult(QString("Exported mesh to: %1 (format: %2)").arg(path, format));
        }
        return makeSuccessResult(QString("Export completed to: %1 (format: %2), result code: %3")
            .arg(path, format).arg(exportResult));
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QStringLiteral("Ogre error: %1")
            .arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolAutoUvUnwrap(const QJsonObject &args)
{
    // Issue #400: xatlas-backed auto UV unwrap on the currently
    // selected entity. Skin weights survive via xref remap.
    if (!hasSelectedEntities())
        return makeErrorResult("No mesh selected. Load a mesh first with load_mesh.");

    UvUnwrapOptions opts;
    if (args.contains("resolution")) opts.resolution = args["resolution"].toInt(1024);
    if (args.contains("padding"))    opts.padding    = args["padding"].toInt(4);
    if (args.contains("channel"))    opts.channel    = args["channel"].toInt(0);
    if (args.contains("preserve_original"))
        opts.preserveOriginalAsBackup = args["preserve_original"].toBool(true);

    SelectionSet* sel = SelectionSet::getSingleton();
    if (!sel || sel->getEntitiesCount() == 0)
        return makeErrorResult("No selected entity.");
    Ogre::Entity* entity = sel->getEntity(0);
    if (!entity) return makeErrorResult("Selected entity is null.");

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.uv_unwrap"),
        QStringLiteral("auto_uv_unwrap entity=%1 res=%2 pad=%3 ch=%4")
            .arg(QString::fromStdString(entity->getName()))
            .arg(opts.resolution).arg(opts.padding).arg(opts.channel));

    UvUnwrapReport report;
    try {
        report = UvUnwrap::unwrapEntity(entity, opts);
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QStringLiteral("Ogre error: %1")
            .arg(QString::fromStdString(e.getFullDescription())));
    }

    if (!report.applied) {
        return makeErrorResult(QStringLiteral("UV unwrap failed: %1").arg(report.error));
    }

    QJsonObject result = makeSuccessResult(UvUnwrap::reportToText(report));
    result["unwrap"] = UvUnwrap::reportToJson(report);
    return result;
}

QJsonObject MCPServer::toolRetopologize(const QJsonObject &args)
{
    // Issue #401: triangle-pairing quad retopology. Operates on the
    // currently selected entity. The mesh is rewritten in place;
    // exporters round-trip the new quads via the qtme.faces.<i>
    // n-gon binding.
    if (!hasSelectedEntities())
        return makeErrorResult("No mesh selected. Load a mesh first with load_mesh.");

    QuadRetopoOptions opts;
    if (args.contains("target_faces"))    opts.targetFaces        = args["target_faces"].toInt(-1);
    if (args.contains("max_angle_deg"))   opts.maxAngleDeg        = args["max_angle_deg"].toDouble(25.0);
    if (args.contains("shape_tol_deg"))   opts.shapeToleranceDeg  = args["shape_tol_deg"].toDouble(65.0);
    if (args.contains("max_aspect_ratio"))opts.maxAspectRatio     = args["max_aspect_ratio"].toDouble(6.0);

    // Validate option ranges up front so caller bugs surface as
    // clear usage errors rather than silent no-ops / confusing
    // output from the algorithm.
    if (opts.targetFaces != -1 && opts.targetFaces <= 0)
        return makeErrorResult("Error: 'target_faces' must be -1 (unlimited) or a positive integer.");
    if (opts.maxAngleDeg < 0.0 || opts.maxAngleDeg > 180.0)
        return makeErrorResult("Error: 'max_angle_deg' must be in [0, 180].");
    if (opts.shapeToleranceDeg < 0.0 || opts.shapeToleranceDeg > 90.0)
        return makeErrorResult("Error: 'shape_tol_deg' must be in [0, 90].");
    if (opts.maxAspectRatio < 1.0)
        return makeErrorResult("Error: 'max_aspect_ratio' must be >= 1.");

    // Use the resolved selection (matches `hasSelectedEntities()`
    // above) so valid node / sub-entity selections aren't rejected
    // by the raw-entity-count path.
    SelectionSet* sel = SelectionSet::getSingleton();
    const QList<Ogre::Entity*> resolved = sel ? sel->getResolvedEntities()
                                              : QList<Ogre::Entity*>{};
    if (resolved.isEmpty())
        return makeErrorResult("No selected entity.");
    Ogre::Entity* entity = resolved.first();
    if (!entity) return makeErrorResult("Selected entity is null.");

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.retopo"),
        QStringLiteral("retopologize entity=%1 target=%2 maxAngle=%3")
            .arg(QString::fromStdString(entity->getName()))
            .arg(opts.targetFaces).arg(opts.maxAngleDeg));

    QuadRetopoReport report;
    try {
        report = QuadRetopo::retopologize(entity, opts);
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QStringLiteral("Ogre error: %1")
            .arg(QString::fromStdString(e.getFullDescription())));
    }

    if (!report.applied) {
        return makeErrorResult(QStringLiteral("Quad retopology failed: %1").arg(report.error));
    }

    QJsonObject result = makeSuccessResult(QuadRetopo::reportToText(report));
    result["retopo"] = QuadRetopo::reportToJson(report);
    return result;
}

QJsonObject MCPServer::toolComputeSkinWeights(const QJsonObject &args)
{
    // Issue #402: inverse-distance skin weights for the currently
    // selected entity. The entity's mesh must have a skeleton.
    if (!hasSelectedEntities())
        return makeErrorResult("No mesh selected. Load a mesh first with load_mesh.");

    // Validate argument TYPES before reading. Qt's
    // QJsonValue::toInt/toDouble/toBool silently return the default
    // when the JSON type doesn't match (e.g. "falloff": "4" as a
    // string, or "replace_existing": "false"), which would apply
    // unintended settings instead of surfacing a usage error.
    if (args.contains("max_influences") && !args["max_influences"].isDouble())
        return makeErrorResult("Error: 'max_influences' must be a number.");
    if (args.contains("falloff") && !args["falloff"].isDouble())
        return makeErrorResult("Error: 'falloff' must be a number.");
    if (args.contains("max_distance") && !args["max_distance"].isDouble())
        return makeErrorResult("Error: 'max_distance' must be a number.");
    if (args.contains("skip_unweighted") && !args["skip_unweighted"].isBool())
        return makeErrorResult("Error: 'skip_unweighted' must be a boolean.");
    if (args.contains("replace_existing") && !args["replace_existing"].isBool())
        return makeErrorResult("Error: 'replace_existing' must be a boolean.");

    SkinWeightsOptions opts;
    if (args.contains("max_influences"))
        opts.maxInfluencesPerVertex = args["max_influences"].toInt(4);
    if (args.contains("falloff"))
        opts.falloff = args["falloff"].toDouble(4.0);
    if (args.contains("max_distance"))
        opts.maxInfluenceDistance = args["max_distance"].toDouble(0.5);
    if (args.contains("skip_unweighted"))
        opts.skipUnweightedBones = args["skip_unweighted"].toBool(false);
    if (args.contains("replace_existing"))
        opts.replaceExisting = args["replace_existing"].toBool(true);

    if (opts.maxInfluencesPerVertex < 1 || opts.maxInfluencesPerVertex > 8)
        return makeErrorResult("Error: 'max_influences' must be in [1, 8].");
    if (opts.falloff < 0.5 || opts.falloff > 16.0)
        return makeErrorResult("Error: 'falloff' must be in [0.5, 16].");
    if (opts.maxInfluenceDistance < 0.0 || opts.maxInfluenceDistance > 10.0)
        return makeErrorResult("Error: 'max_distance' must be in [0, 10].");

    SelectionSet* sel = SelectionSet::getSingleton();
    const QList<Ogre::Entity*> resolved = sel ? sel->getResolvedEntities()
                                              : QList<Ogre::Entity*>{};
    if (resolved.isEmpty())
        return makeErrorResult("No selected entity.");
    Ogre::Entity* entity = resolved.first();
    if (!entity) return makeErrorResult("Selected entity is null.");

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.skin_weights"),
        QStringLiteral("compute_skin_weights entity=%1 maxInf=%2 falloff=%3")
            .arg(QString::fromStdString(entity->getName()))
            .arg(opts.maxInfluencesPerVertex).arg(opts.falloff));

    SkinWeightsReport report;
    try {
        report = SkinWeights::computeAndApply(entity, opts);
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QStringLiteral("Ogre error: %1")
            .arg(QString::fromStdString(e.getFullDescription())));
    }

    if (!report.applied) {
        return makeErrorResult(QStringLiteral("Skin weights failed: %1").arg(report.error));
    }

    QJsonObject result = makeSuccessResult(SkinWeights::reportToText(report));
    result["skin"] = SkinWeights::reportToJson(report);
    return result;
}

QJsonObject MCPServer::toolGenerateMeshTexture(const QJsonObject &args)
{
#ifndef ENABLE_STABLE_DIFFUSION
    Q_UNUSED(args);
    return makeErrorResult(
        "This build was compiled without AI texture generation "
        "(rebuild with -DENABLE_STABLE_DIFFUSION=ON).");
#else
    const QString prompt = args.value("prompt").toString();
    if (prompt.trimmed().isEmpty())
        return makeErrorResult("'prompt' is required.");

    const int width  = args.contains("width")  ? args["width"].toInt(512)  : 512;
    const int height = args.contains("height") ? args["height"].toInt(512) : 512;
    if (width < 64 || width > 2048 || height < 64 || height > 2048)
        return makeErrorResult(
            "'width' and 'height' must be between 64 and 2048.");
    double strength  = args.contains("controlnet_strength")
        ? args["controlnet_strength"].toDouble(0.9) : 0.9;
    strength = std::clamp(strength, 0.0, 1.0);

    if (!hasSelectedEntities())
        return makeErrorResult("No mesh selected. Use select_entity / load_mesh first.");

    SelectionSet* sel = SelectionSet::getSingleton();
    const QList<Ogre::Entity*> resolved = sel ? sel->getResolvedEntities()
                                              : QList<Ogre::Entity*>{};
    if (resolved.isEmpty() || !resolved.first() || !resolved.first()->getMesh())
        return makeErrorResult("Selected entity is not a valid mesh.");
    Ogre::Entity* entity = resolved.first();

    SDManager* sd = SDManager::instance();
    if (!sd || !sd->isModelLoaded())
        return makeErrorResult("No SD base model loaded. Load one from AI Settings first.");

    // Render the depth map (we're already on the main thread — MCP
    // tools run on the main thread via QSocketNotifier).
    QString depthErr;
    const int depthSize = std::max(width, height);
    const QImage depth = MeshDepthRenderer::renderDepthMap(entity, depthSize, &depthErr);
    if (depth.isNull())
        return makeErrorResult(QStringLiteral("Depth render failed: %1").arg(depthErr));

    // Resolve / honor an explicit controlnet model path, else
    // auto-discover one in the models dir.
    QString controlNetPath = args.value("controlnet_path").toString();
    if (!controlNetPath.isEmpty() && !QFileInfo(controlNetPath).isFile())
        return makeErrorResult(
            QStringLiteral("ControlNet model not found: %1").arg(controlNetPath));
    if (controlNetPath.isEmpty()) {
        QDir d(sd->modelsDirectory());
        const QStringList files = d.entryList(
            QStringList() << "*.safetensors" << "*.ckpt", QDir::Files);
        // SD 1.5-only pipeline: skip SDXL depth ControlNets, prefer an
        // explicit SD1.5 tag (mirrors MaterialEditorQML::discovered...).
        auto isSdxl = [](const QString& lower) {
            return lower.contains("sdxl") || lower.contains("xl_")
                || lower.contains("-xl") || lower.contains("_xl");
        };
        QString fallback;
        for (const QString& f : files) {
            const QString lower = f.toLower();
            if (!(lower.contains("control") && lower.contains("depth")))
                continue;
            if (isSdxl(lower))
                continue;
            if (lower.contains("sd15") || lower.contains("sd_15")
                || lower.contains("v11")) {
                controlNetPath = d.filePath(f);
                break;
            }
            if (fallback.isEmpty())
                fallback = d.filePath(f);
        }
        if (controlNetPath.isEmpty())
            controlNetPath = fallback;
    }

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.mesh_texture"),
        QStringLiteral("MCP entity=%1 controlNet=%2 strength=%3 size=%4")
            .arg(QString::fromStdString(entity->getName()))
            .arg(controlNetPath.isEmpty() ? QStringLiteral("(none)")
                                          : QFileInfo(controlNetPath).fileName())
            .arg(strength).arg(depthSize));

    sd->generateMeshTexture(prompt, depth, controlNetPath,
                            static_cast<float>(strength), QString(),
                            width, height);

    QJsonObject result = makeSuccessResult(QStringLiteral(
        "Mesh-aware texture generation started for '%1'%2. "
        "Generation runs asynchronously; the result is applied to the "
        "active material's diffuse slot when complete.")
        .arg(QString::fromStdString(entity->getName()))
        .arg(controlNetPath.isEmpty()
             ? QStringLiteral(" (no ControlNet depth model found — unconditioned txt2img)")
             : QStringLiteral(" (depth-conditioned via %1)")
                 .arg(QFileInfo(controlNetPath).fileName())));
    result["controlNetUsed"]  = !controlNetPath.isEmpty();
    result["depthSize"]       = depthSize;
    result["controlStrength"] = strength;
    return result;
#endif
}

QJsonObject MCPServer::toolGeneratePbrMaps(const QJsonObject &args)
{
#ifndef ENABLE_ONNX
    Q_UNUSED(args);
    return makeErrorResult(
        "This build was compiled without AI PBR map synthesis "
        "(rebuild with -DENABLE_ONNX=ON).");
#else
    const QString albedoPath = args.value("albedo_path").toString();
    if (albedoPath.trimmed().isEmpty())
        return makeErrorResult("'albedo_path' is required.");
    if (!QFileInfo::exists(albedoPath))
        return makeErrorResult(
            QStringLiteral("albedo not found: %1").arg(albedoPath));

    PbrMapSynth::Options opts;
    if (args.contains("normal"))    opts.generateNormal    = args["normal"].toBool();
    if (args.contains("roughness")) opts.generateRoughness = args["roughness"].toBool();
    if (args.contains("height"))    opts.generateHeight    = args["height"].toBool();
    if (args.contains("tile_size")) opts.tileSize          = args["tile_size"].toInt(256);
    if (args.contains("overwrite")) opts.overwriteCache    = args["overwrite"].toBool();
    // Mirror the CLI bounds: 0 (whole image) or 32..4096. Reject out-of-range
    // so a bad MCP/HTTP request can't drive pathological tiling/allocation.
    if (opts.tileSize != 0 && (opts.tileSize < 32 || opts.tileSize > 4096))
        return makeErrorResult("'tile_size' must be 0 (whole image) or between 32 and 4096.");

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.pbr_synth"),
        QStringLiteral("MCP generate_pbr_maps from %1")
            .arg(QFileInfo(albedoPath).fileName()));

    const PbrMapSynthResult res =
        AIAssistManager::instance()->synthesizePbrMaps(albedoPath, opts);
    if (!res.ok)
        return makeErrorResult(res.error.isEmpty()
            ? QStringLiteral("PBR synthesis failed") : res.error);

    // If a mesh is selected, bind the maps into the canonical slice-E slots.
    int bound = 0;
    if (hasSelectedEntities()) {
        SelectionSet* sel = SelectionSet::getSingleton();
        const QList<Ogre::Entity*> resolved = sel ? sel->getResolvedEntities()
                                                  : QList<Ogre::Entity*>{};
        Ogre::Entity* entity = resolved.isEmpty() ? nullptr : resolved.first();
        if (entity) {
            Ogre::ResourceGroupManager::getSingleton().addResourceLocation(
                QFileInfo(albedoPath).absolutePath().toStdString(), "FileSystem",
                Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
            auto bindSlot = [](Ogre::Pass* pass, const char* slot, const QString& p) {
                if (p.isEmpty()) return;
                const std::string tex = QFileInfo(p).fileName().toStdString();
                Ogre::TextureUnitState* tus = nullptr;
                for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i)
                    if (pass->getTextureUnitState(i)->getName() == slot) { tus = pass->getTextureUnitState(i); break; }
                if (!tus) { tus = pass->createTextureUnitState(); tus->setName(slot); }
                tus->setTextureName(tex);
            };
            for (unsigned int s = 0; s < entity->getNumSubEntities(); ++s) {
                const auto mat = entity->getSubEntity(s)->getMaterial();
                if (!mat || mat->getNumTechniques() == 0) continue;
                auto* tech = mat->getTechnique(0);
                if (tech->getNumPasses() == 0) continue;
                auto* pass = tech->getPass(0);
                bindSlot(pass, "normal_map", res.normalPath);
                bindSlot(pass, "roughness",  res.roughnessPath);
                RTShaderHelper::wirePbrSlotsForFFP(mat.get());
                mat->compile();
                ++bound;
            }
        }
    }

    QJsonObject result = makeSuccessResult(QStringLiteral(
        "Generated PBR maps from '%1'%2.")
        .arg(QFileInfo(albedoPath).fileName())
        .arg(bound > 0 ? QStringLiteral(" and bound to %1 submesh(es)").arg(bound)
                       : QString()));
    result["normalPath"]    = res.normalPath;
    result["roughnessPath"] = res.roughnessPath;
    result["heightPath"]    = res.heightPath;
    result["fromCache"]     = res.fromCache;
    result["boundSubmeshes"] = bound;
    return result;
#endif
}

QJsonObject MCPServer::toolUpscaleTexture(const QJsonObject &args)
{
#ifndef ENABLE_ONNX
    Q_UNUSED(args);
    return makeErrorResult(
        "This build was compiled without AI texture upscaling "
        "(rebuild with -DENABLE_ONNX=ON).");
#else
    const QString srcPath = args.value("texture_path").toString();
    if (srcPath.trimmed().isEmpty())
        return makeErrorResult("'texture_path' is required.");
    if (!QFileInfo::exists(srcPath))
        return makeErrorResult(QStringLiteral("texture not found: %1").arg(srcPath));
    const int scale = args.contains("scale") ? args["scale"].toInt(4) : 4;
    if (scale != 2 && scale != 4)
        return makeErrorResult("'scale' must be 2 or 4.");
    const bool overwrite = args.value("overwrite").toBool();

    const QString out =
        AIAssistManager::instance()->upscaleTexture(srcPath, scale, overwrite);
    if (out.isEmpty())
        return makeErrorResult(
            "Upscale failed (model unavailable/offline or inference error).");

    QJsonObject result = makeSuccessResult(QStringLiteral(
        "Upscaled '%1' by %2x.").arg(QFileInfo(srcPath).fileName()).arg(scale));
    result["outputPath"] = out;
    result["scale"] = scale;
    return result;
#endif
}

QJsonObject MCPServer::toolGetSceneInfo(const QJsonObject &args)
{
    Q_UNUSED(args);
    try {
        Manager* mgr = Manager::getSingletonPtr();
        if (!mgr) {
            return makeErrorResult("Error: Manager not available");
        }
        // Copy lists to avoid reference invalidation
        const QList<Ogre::SceneNode*> nodes = mgr->getSceneNodes();
        int materialCount = 0;
        auto& matMgr = Ogre::MaterialManager::getSingleton();
        auto it = matMgr.getResourceIterator();
        while (it.hasMoreElements()) {
            it.getNext();
            materialCount++;
        }
        // Build scene node list and entity list by iterating nodes directly.
        // Manager::getEntities() uses static_cast<Entity*> on all attached objects,
        // which crashes on ManualObjects. Check movable type explicitly.
        QStringList nodeNames;
        QStringList entityInfo;
        int entityCount = 0;
        for (Ogre::SceneNode* node : nodes) {
            if (!node) continue;
            nodeNames << QString::fromStdString(node->getName());
            for (int i = 0; i < static_cast<int>(node->numAttachedObjects()); i++) {
                Ogre::MovableObject* obj = node->getAttachedObject(i);
                if (!obj || obj->getMovableType() != "Entity") continue;
                Ogre::Entity* entity = static_cast<Ogre::Entity*>(obj);
                entityCount++;
                QString info = QString("  - %1").arg(QString::fromStdString(entity->getName()));
                if (entity->getNumSubEntities() > 0) {
                    Ogre::SubEntity* subEnt = entity->getSubEntity(0);
                    if (subEnt && subEnt->getMaterial()) {
                        info += QString(" (material: %1)").arg(
                            QString::fromStdString(subEnt->getMaterial()->getName()));
                    }
                }
                entityInfo << info;
            }
        }
        const QString sceneInfo = QString(
            "Scene Information:\n"
            "- Scene Nodes: %1\n"
            "- Entities: %2\n"
            "- Materials loaded: %3\n\n"
            "Nodes:\n%4\n\n"
            "Entities:\n%5"
        ).arg(nodes.size())
         .arg(entityCount)
         .arg(materialCount)
         .arg(nodeNames.isEmpty() ? "  (none)" : "  " + nodeNames.join("\n  "))
         .arg(entityInfo.isEmpty() ? "  (none)" : entityInfo.join("\n"));
        return makeSuccessResult(sceneInfo);
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QStringLiteral("Ogre error: %1")
            .arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolTakeScreenshot(const QJsonObject &args)
{
    QString path = args["path"].toString();

    if (path.isEmpty()) {
        // Generate temp path
        path = QDir::temp().filePath("qtmesheditor_screenshot.png");
    }

    if (!m_mainWindow) {
        return makeErrorResult("Error: MainWindow not available. Run with --with-mcp flag for full functionality.");
    }

    OgreWidget* ogreWidget = m_mainWindow->findChild<OgreWidget*>();
    if (!ogreWidget) {
        return makeErrorResult("Error: OgreWidget not found");
    }

    QPixmap pixmap = ogreWidget->grab();
    if (pixmap.isNull()) {
        return makeErrorResult("Error: Failed to capture screenshot");
    }

    if (!pixmap.save(path)) {
        return makeErrorResult(QString("Error: Failed to save screenshot to: %1").arg(path));
    }

    return makeSuccessResult(QString("Screenshot saved to: %1 (%2x%3)")
        .arg(path).arg(pixmap.width()).arg(pixmap.height()));
}

QJsonObject MCPServer::toolCreatePrimitive(const QJsonObject &args)
{
    QString type = args["type"].toString().toLower();

    if (type.isEmpty()) {
        return makeErrorResult("Error: Primitive type is required (sphere, cube, plane, cylinder, cone, torus, tube, capsule, icosphere, roundedbox, spring)");
    }

    // Map type strings to PrimitiveType enum values
    static const QMap<QString, PrimitiveObject::PrimitiveType> typeMap = {
        {"cube",      PrimitiveObject::AP_CUBE},
        {"box",       PrimitiveObject::AP_CUBE},
        {"sphere",    PrimitiveObject::AP_SPHERE},
        {"plane",     PrimitiveObject::AP_PLANE},
        {"cylinder",  PrimitiveObject::AP_CYLINDER},
        {"cone",      PrimitiveObject::AP_CONE},
        {"torus",     PrimitiveObject::AP_TORUS},
        {"tube",      PrimitiveObject::AP_TUBE},
        {"capsule",   PrimitiveObject::AP_CAPSULE},
        {"icosphere", PrimitiveObject::AP_ICOSPHERE},
        {"roundedbox",PrimitiveObject::AP_ROUNDEDBOX},
        {"spring",    PrimitiveObject::AP_SPRING},
    };

    if (!typeMap.contains(type)) {
        return makeErrorResult(QString("Unknown primitive type: %1. Valid types: sphere, cube, plane, cylinder, cone, torus, tube, capsule, icosphere, roundedbox, spring").arg(type));
    }

    // Generate a name if not provided
    QString name = args["name"].toString();
    if (name.isEmpty()) {
        // Auto-generate a unique name based on type and timestamp
        name = type + "_" + QString::number(QDateTime::currentMSecsSinceEpoch());
    }

    Ogre::SceneNode* node = PrimitiveObject::createPrimitive(typeMap[type], name);

    if (!node) {
        return makeErrorResult(QString("Failed to create %1 primitive").arg(type));
    }

    // Return the ACTUAL node name — Manager::addSceneNode may append a number
    // if the requested name was already taken (e.g. "sphere" → "sphere1").
    // The AI must use this exact name for subsequent apply_material calls.
    QString actualName = QString::fromStdString(node->getName());
    return makeSuccessResult(QString("Created %1 primitive '%2'").arg(type).arg(actualName));
}
QJsonObject MCPServer::toolAnimate(const QJsonObject &args)
{
    QString name = args["name"].toString();
    bool stop = args["stop"].toBool(false);

    if (name.isEmpty()) {
        return makeErrorResult("Error: Node name is required");
    }

    if (stop) {
        m_animations.remove(name);
        if (m_animations.isEmpty() && m_animationTimer) {
            m_animationTimer->stop();
        }
        return makeSuccessResult(QString("Stopped animation on '%1'").arg(name));
    }
    try {
        if (!Manager::getSingletonPtr()) {
            return makeErrorResult("Error: Manager not available");
        }
        Ogre::SceneNode* targetNode = findSceneNodeByName(name);
        if (!targetNode) {
            return makeErrorResult(QString("Error: Node '%1' not found").arg(name));
        }
        float yaw   = static_cast<float>(args["yaw"].toDouble(0));
        float pitch = static_cast<float>(args["pitch"].toDouble(0));
        float roll  = static_cast<float>(args["roll"].toDouble(0));
        if (yaw == 0 && pitch == 0 && roll == 0) {
            yaw = 45; // default: 45 degrees/sec around Y
        }
        NodeAnimation anim;
        anim.node = targetNode;
        anim.yawSpeed = yaw;
        anim.pitchSpeed = pitch;
        anim.rollSpeed = roll;
        m_animations[name] = anim;
        if (!m_animationTimer) {
            m_animationTimer = new QTimer(this);
            connect(m_animationTimer, &QTimer::timeout, this, &MCPServer::onAnimationTick);
        }
        if (!m_animationTimer->isActive()) {
            m_animationTimer->start(16); // ~60fps
        }
        return makeSuccessResult(QString("Started animation on '%1' (yaw: %2, pitch: %3, roll: %4 deg/sec)")
            .arg(name).arg(yaw).arg(pitch).arg(roll));
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QStringLiteral("Ogre error: %1")
            .arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolListSkeletalAnimations(const QJsonObject &args)
{
    Q_UNUSED(args);
    try {
        Manager* mgr = Manager::getSingletonPtr();
        if (!mgr) return makeErrorResult("Error: Manager not available");
        QStringList infoLines;
        const QList<Ogre::SceneNode*> nodes = mgr->getSceneNodes();
        for (Ogre::SceneNode* node : nodes) {
            if (!node) continue;
            for (int i = 0; i < static_cast<int>(node->numAttachedObjects()); i++) {
                Ogre::MovableObject* obj = node->getAttachedObject(i);
                if (!obj || obj->getMovableType() != "Entity") continue;
                Ogre::Entity* entity = static_cast<Ogre::Entity*>(obj);
                if (!entity->hasSkeleton()) continue;
                Ogre::AnimationStateSet* stateSet = entity->getAllAnimationStates();
                if (!stateSet) continue;
                for (const auto &pair : stateSet->getAnimationStates()) {
                    Ogre::AnimationState* state = pair.second;
                    infoLines << QString("Entity: %1 | Animation: %2 | Length: %3s | Enabled: %4 | Loop: %5")
                        .arg(QString::fromStdString(entity->getName()))
                        .arg(QString::fromStdString(pair.first))
                        .arg(state->getLength())
                        .arg(state->getEnabled() ? "yes" : "no")
                        .arg(state->getLoop() ? "yes" : "no");
                }
            }
        }
        if (infoLines.isEmpty())
            return makeSuccessResult("No skeletal animations found in scene");
        return makeSuccessResult(QString("Skeletal animations (%1):\n%2")
            .arg(infoLines.size()).arg(infoLines.join("\n")));
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QStringLiteral("Ogre error: %1")
            .arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolGetAnimationInfo(const QJsonObject &args)
{
    QString entityName = args["entity"].toString();
    QString animName = args["animation"].toString();

    if (entityName.isEmpty() || animName.isEmpty())
        return makeErrorResult("Error: 'entity' and 'animation' are required");

    try {
        Ogre::Entity* entity = findEntityByName(entityName);
        if (!entity) return makeErrorResult(QString("Error: Entity '%1' not found").arg(entityName));
        if (!entity->hasSkeleton()) return makeErrorResult(QString("Error: Entity '%1' has no skeleton").arg(entityName));

        Ogre::SkeletonInstance* skeleton = entity->getSkeleton();
        if (!skeleton->hasAnimation(animName.toStdString()))
            return makeErrorResult(QString("Error: Animation '%1' not found on entity '%2'").arg(animName, entityName));

        Ogre::Animation* anim = skeleton->getAnimation(animName.toStdString());

        QStringList lines;
        lines << QString("Animation: %1").arg(animName);
        lines << QString("Length: %1s").arg(anim->getLength());

        auto trackList = anim->_getNodeTrackList();
        lines << QString("Tracks: %1").arg(trackList.size());

        for (const auto &trackPair : trackList) {
            Ogre::NodeAnimationTrack* track = trackPair.second;
            QString boneName = QString::fromStdString(track->getAssociatedNode()->getName());
            lines << QString("\n  Track: %1 (bone: %2)").arg(trackPair.first).arg(boneName);
            lines << QString("  Keyframes: %1").arg(track->getNumKeyFrames());

            for (unsigned short k = 0; k < track->getNumKeyFrames(); k++) {
                Ogre::TransformKeyFrame* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(k));
                Ogre::Vector3 t = kf->getTranslate();
                Ogre::Vector3 s = kf->getScale();
                Ogre::Quaternion r = kf->getRotation();
                lines << QString("    [%1] time=%2s  pos=(%3,%4,%5)  rot=(%6,%7,%8,%9)  scale=(%10,%11,%12)")
                    .arg(k).arg(kf->getTime())
                    .arg(t.x).arg(t.y).arg(t.z)
                    .arg(r.w).arg(r.x).arg(r.y).arg(r.z)
                    .arg(s.x).arg(s.y).arg(s.z);
            }
        }

        return makeSuccessResult(lines.join("\n"));

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolSetAnimationLength(const QJsonObject &args)
{
    QString entityName = args["entity"].toString();
    QString animName = args["animation"].toString();
    double length = args["length"].toDouble(-1);

    if (entityName.isEmpty() || animName.isEmpty())
        return makeErrorResult("Error: 'entity' and 'animation' are required");
    if (length <= 0)
        return makeErrorResult("Error: 'length' must be a positive number");

    try {
        Ogre::Entity* entity = findEntityByName(entityName);
        if (!entity) return makeErrorResult(QString("Error: Entity '%1' not found").arg(entityName));
        if (!entity->hasSkeleton()) return makeErrorResult(QString("Error: Entity '%1' has no skeleton").arg(entityName));

        Ogre::SkeletonInstance* skeleton = entity->getSkeleton();
        if (!skeleton->hasAnimation(animName.toStdString()))
            return makeErrorResult(QString("Error: Animation '%1' not found").arg(animName));

        Ogre::Animation* anim = skeleton->getAnimation(animName.toStdString());
        float oldLength = anim->getLength();
        anim->setLength(static_cast<float>(length));

        // Update animation state length and clamp time
        entity->getAllAnimationStates()->_notifyDirty();
        if (entity->hasAnimationState(animName.toStdString())) {
            Ogre::AnimationState* state = entity->getAnimationState(animName.toStdString());
            state->setLength(static_cast<float>(length));
            if (state->getTimePosition() > static_cast<float>(length))
                state->setTimePosition(static_cast<float>(length));
        }

        return makeSuccessResult(QString("Changed animation '%1' length from %2s to %3s")
            .arg(animName).arg(oldLength).arg(length));

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolSetAnimationTime(const QJsonObject &args)
{
    QString entityName = args["entity"].toString();
    QString animName = args["animation"].toString();

    if (entityName.isEmpty() || animName.isEmpty())
        return makeErrorResult("Error: 'entity' and 'animation' are required");

    try {
        Ogre::Entity* entity = findEntityByName(entityName);
        if (!entity) return makeErrorResult(QString("Error: Entity '%1' not found").arg(entityName));
        if (!entity->hasAnimationState(animName.toStdString()))
            return makeErrorResult(QString("Error: Animation '%1' not found on entity '%2'").arg(animName, entityName));

        Ogre::AnimationState* state = entity->getAnimationState(animName.toStdString());

        // Handle keyframe navigation
        QString navigate = args["navigate"].toString();
        if (!navigate.isEmpty()) {
            QString trackName = args["track"].toString();
            if (trackName.isEmpty())
                return makeErrorResult("Error: 'track' (bone name) is required for keyframe navigation");

            if (!entity->hasSkeleton())
                return makeErrorResult(QString("Error: Entity '%1' has no skeleton").arg(entityName));

            Ogre::SkeletonInstance* skeleton = entity->getSkeleton();
            Ogre::Animation* anim = skeleton->getAnimation(animName.toStdString());

            Ogre::NodeAnimationTrack* track = findTrackByBoneName(anim, trackName);
            if (!track)
                return makeErrorResult(QString("Error: Track for bone '%1' not found").arg(trackName));

            if (track->getNumKeyFrames() == 0)
                return makeErrorResult("Error: Track has no keyframes");

            float currentTime = state->getTimePosition();
            Ogre::TransformKeyFrame* target = nullptr;

            if (navigate == "next") {
                for (unsigned short i = 0; i < track->getNumKeyFrames(); i++) {
                    Ogre::TransformKeyFrame* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
                    if (kf->getTime() > currentTime + 0.001f) {
                        target = kf;
                        break;
                    }
                }
                if (!target)
                    target = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(track->getNumKeyFrames() - 1));
            } else if (navigate == "prev") {
                for (int i = static_cast<int>(track->getNumKeyFrames()) - 1; i >= 0; i--) {
                    Ogre::TransformKeyFrame* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(i));
                    if (kf->getTime() < currentTime - 0.001f) {
                        target = kf;
                        break;
                    }
                }
                if (!target)
                    target = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(0));
            } else if (navigate == "first") {
                target = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(0));
            } else if (navigate == "last") {
                target = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(track->getNumKeyFrames() - 1));
            } else {
                return makeErrorResult("Error: 'navigate' must be 'next', 'prev', 'first', or 'last'");
            }

            state->setEnabled(true);
            state->setTimePosition(target->getTime());

            Ogre::Vector3 t = target->getTranslate();
            Ogre::Quaternion r = target->getRotation();
            return makeSuccessResult(QString("Navigated to keyframe at %1s  pos=(%2,%3,%4)  rot=(%5,%6,%7,%8)")
                .arg(target->getTime())
                .arg(t.x).arg(t.y).arg(t.z)
                .arg(r.w).arg(r.x).arg(r.y).arg(r.z));
        }

        // Set absolute time
        if (!args.contains("time"))
            return makeErrorResult("Error: Either 'time' or 'navigate' is required");

        float time = static_cast<float>(args["time"].toDouble());
        bool enable = args["enabled"].toBool(true);
        bool loop = args.contains("loop") ? args["loop"].toBool() : state->getLoop();

        state->setEnabled(enable);
        state->setLoop(loop);
        state->setTimePosition(time);

        return makeSuccessResult(QString("Set animation '%1' time to %2s (enabled: %3, loop: %4)")
            .arg(animName).arg(time)
            .arg(enable ? "yes" : "no")
            .arg(loop ? "yes" : "no"));

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
    }
}

// Helper: find scene node by name across all scene nodes
static Ogre::SceneNode* findSceneNodeByName(const QString &nodeName)
{
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr) return nullptr;
    QList<Ogre::SceneNode*> nodes = mgr->getSceneNodes();
    for (Ogre::SceneNode* node : nodes) {
        if (node && QString::fromStdString(node->getName()) == nodeName)
            return node;
    }
    return nullptr;
}

// Helper: find entity by name across all scene nodes (checks movable type)
static Ogre::Entity* findEntityByName(const QString &entityName)
{
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr) return nullptr;
    QList<Ogre::SceneNode*> nodes = mgr->getSceneNodes();
    for (Ogre::SceneNode* node : nodes) {
        if (!node) continue;
        for (int i = 0; i < static_cast<int>(node->numAttachedObjects()); i++) {
            Ogre::MovableObject* obj = node->getAttachedObject(i);
            if (!obj || obj->getMovableType() != "Entity") continue;
            if (QString::fromStdString(obj->getName()) == entityName)
                return static_cast<Ogre::Entity*>(obj);
        }
    }
    return nullptr;
}

static bool hasSelectedEntities()
{
    SelectionSet* sel = SelectionSet::getSingleton();
    return sel && !sel->getResolvedEntities().isEmpty();
}

static QString captureLodControllerError(const std::function<void()> &operation)
{
    QString errorMsg;
    QObject errorCapture;
    QObject::connect(MeshLodController::instance(), &MeshLodController::error,
                     &errorCapture, [&errorMsg](const QString& msg) { errorMsg = msg; },
                     Qt::DirectConnection);
    operation();
    return errorMsg;
}

// Helper: find track by bone name in an animation
static Ogre::NodeAnimationTrack* findTrackByBoneName(Ogre::Animation* anim, const QString &boneName)
{
    auto trackList = anim->_getNodeTrackList();
    for (const auto &pair : trackList) {
        if (QString::fromStdString(pair.second->getAssociatedNode()->getName()) == boneName)
            return pair.second;
    }
    return nullptr;
}

QJsonObject MCPServer::toolAddKeyframe(const QJsonObject &args)
{
    QString entityName = args["entity"].toString();
    QString animName = args["animation"].toString();
    QString trackName = args["track"].toString();
    double time = args["time"].toDouble(-1);

    if (entityName.isEmpty() || animName.isEmpty() || trackName.isEmpty())
        return makeErrorResult("Error: 'entity', 'animation', and 'track' are required");
    if (time < 0)
        return makeErrorResult("Error: 'time' must be a non-negative number");

    try {
        Ogre::Entity* entity = findEntityByName(entityName);
        if (!entity) return makeErrorResult(QString("Error: Entity '%1' not found").arg(entityName));
        if (!entity->hasSkeleton()) return makeErrorResult(QString("Error: Entity '%1' has no skeleton").arg(entityName));

        Ogre::SkeletonInstance* skeleton = entity->getSkeleton();
        if (!skeleton->hasAnimation(animName.toStdString()))
            return makeErrorResult(QString("Error: Animation '%1' not found").arg(animName));

        Ogre::Animation* anim = skeleton->getAnimation(animName.toStdString());
        Ogre::NodeAnimationTrack* track = findTrackByBoneName(anim, trackName);
        if (!track)
            return makeErrorResult(QString("Error: Track for bone '%1' not found").arg(trackName));

        Ogre::TransformKeyFrame* kf = track->createNodeKeyFrame(static_cast<float>(time));

        // If no explicit transforms given, use interpolated values at this time
        if (!args.contains("translate") && !args.contains("rotate") && !args.contains("scale")) {
            if (entity->hasAnimationState(animName.toStdString())) {
                Ogre::TransformKeyFrame interp(nullptr, static_cast<float>(time));
                track->getInterpolatedKeyFrame(static_cast<float>(time), &interp);
                kf->setTranslate(interp.getTranslate());
                kf->setRotation(interp.getRotation());
                kf->setScale(interp.getScale());
            }
        } else {
            if (args.contains("translate")) {
                QJsonArray t = args["translate"].toArray();
                kf->setTranslate(Ogre::Vector3(t[0].toDouble(), t[1].toDouble(), t[2].toDouble()));
            }
            if (args.contains("rotate")) {
                QJsonArray r = args["rotate"].toArray();
                kf->setRotation(Ogre::Quaternion(r[0].toDouble(), r[1].toDouble(), r[2].toDouble(), r[3].toDouble()));
            }
            if (args.contains("scale")) {
                QJsonArray s = args["scale"].toArray();
                kf->setScale(Ogre::Vector3(s[0].toDouble(), s[1].toDouble(), s[2].toDouble()));
            }
        }

        entity->getAllAnimationStates()->_notifyDirty();

        Ogre::Vector3 t = kf->getTranslate();
        Ogre::Quaternion r = kf->getRotation();
        Ogre::Vector3 s = kf->getScale();
        return makeSuccessResult(QString("Added keyframe at %1s on track '%2': pos=(%3,%4,%5) rot=(%6,%7,%8,%9) scale=(%10,%11,%12)")
            .arg(time).arg(trackName)
            .arg(t.x).arg(t.y).arg(t.z)
            .arg(r.w).arg(r.x).arg(r.y).arg(r.z)
            .arg(s.x).arg(s.y).arg(s.z));

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolRemoveKeyframe(const QJsonObject &args)
{
    QString entityName = args["entity"].toString();
    QString animName = args["animation"].toString();
    QString trackName = args["track"].toString();
    double time = args["time"].toDouble(-1);

    if (entityName.isEmpty() || animName.isEmpty() || trackName.isEmpty())
        return makeErrorResult("Error: 'entity', 'animation', and 'track' are required");
    if (time < 0)
        return makeErrorResult("Error: 'time' must be a non-negative number");

    try {
        Ogre::Entity* entity = findEntityByName(entityName);
        if (!entity) return makeErrorResult(QString("Error: Entity '%1' not found").arg(entityName));
        if (!entity->hasSkeleton()) return makeErrorResult(QString("Error: Entity '%1' has no skeleton").arg(entityName));

        Ogre::SkeletonInstance* skeleton = entity->getSkeleton();
        if (!skeleton->hasAnimation(animName.toStdString()))
            return makeErrorResult(QString("Error: Animation '%1' not found").arg(animName));

        Ogre::Animation* anim = skeleton->getAnimation(animName.toStdString());
        Ogre::NodeAnimationTrack* track = findTrackByBoneName(anim, trackName);
        if (!track)
            return makeErrorResult(QString("Error: Track for bone '%1' not found").arg(trackName));

        // Find keyframe at the given time
        bool found = false;
        for (unsigned short i = 0; i < track->getNumKeyFrames(); i++) {
            if (std::fabs(track->getKeyFrame(i)->getTime() - static_cast<float>(time)) < 0.001f) {
                track->removeKeyFrame(i);
                found = true;
                break;
            }
        }

        if (!found)
            return makeErrorResult(QString("Error: No keyframe found at time %1s on track '%2'").arg(time).arg(trackName));

        entity->getAllAnimationStates()->_notifyDirty();

        return makeSuccessResult(QString("Removed keyframe at %1s from track '%2'").arg(time).arg(trackName));

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolPlayAnimation(const QJsonObject &args)
{
    QString entityName = args["entity"].toString();
    QString animName = args["animation"].toString();

    if (entityName.isEmpty() || animName.isEmpty())
        return makeErrorResult("Error: 'entity' and 'animation' are required");

    bool play = args.contains("play") ? args["play"].toBool() : true;
    bool loop = args.contains("loop") ? args["loop"].toBool() : true;

    try {
        Ogre::Entity* entity = findEntityByName(entityName);
        if (!entity) return makeErrorResult(QString("Error: Entity '%1' not found").arg(entityName));
        if (!entity->hasAnimationState(animName.toStdString()))
            return makeErrorResult(QString("Error: Animation '%1' not found on entity '%2'").arg(animName, entityName));

        Ogre::AnimationState* state = entity->getAnimationState(animName.toStdString());
        state->setEnabled(play);
        state->setLoop(loop);

        MainWindow* mainWindow = qobject_cast<MainWindow*>(m_mainWindow);
        if (mainWindow) {
            if (play) {
                mainWindow->setPlaying(true);
            } else {
                // Only stop global playback if no entity has enabled animations
                bool anyEnabled = false;
                for (Ogre::SceneNode* node : Manager::getSingletonPtr()->getSceneNodes()) {
                    if (!node) continue;
                    for (int i = 0; i < static_cast<int>(node->numAttachedObjects()); ++i) {
                        Ogre::MovableObject* obj = node->getAttachedObject(i);
                        if (!obj || obj->getMovableType() != "Entity") continue;
                        auto* ent = static_cast<Ogre::Entity*>(obj);
                        Ogre::AnimationStateSet* stateSet = ent->getAllAnimationStates();
                        if (!stateSet) continue;
                        for (const auto& [k, s] : stateSet->getAnimationStates()) {
                            if (s->getEnabled()) { anyEnabled = true; break; }
                        }
                        if (anyEnabled) break;
                    }
                    if (anyEnabled) break;
                }
                if (!anyEnabled)
                    mainWindow->setPlaying(false);
            }
        }

        if (play)
            return makeSuccessResult(QString("Playing animation '%1' on entity '%2' (loop=%3)")
                .arg(animName, entityName, loop ? "true" : "false"));
        else
            return makeSuccessResult(QString("Stopped animation '%1' on entity '%2'")
                .arg(animName, entityName));

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
    }
}

MCPServer::SkeletonEntityResult MCPServer::resolveSkeletonEntity(const QString &entityName)
{
    SkeletonEntityResult result;

    result.entity = findEntityByName(entityName);
    if (!result.entity) {
        result.error = makeErrorResult(QString("Error: Entity '%1' not found").arg(entityName));
        return result;
    }
    if (!result.entity->hasSkeleton()) {
        result.error = makeErrorResult(QString("Error: Entity '%1' has no skeleton").arg(entityName));
        return result;
    }
    if (!m_mainWindow) {
        result.error = makeErrorResult("Error: MainWindow not available. Run with --with-mcp flag.");
        return result;
    }
    result.animWidget = m_mainWindow->findChild<AnimationWidget*>();
    if (!result.animWidget) {
        result.error = makeErrorResult("Error: AnimationWidget not found");
        return result;
    }
    return result;
}

QJsonObject MCPServer::toolToggleSkeletonDebug(const QJsonObject &args)
{
    QString entityName = args["entity"].toString();
    if (entityName.isEmpty())
        return makeErrorResult("Error: 'entity' is required");

    bool bones = args.contains("bones") ? args["bones"].toBool() : true;
    bool axes = args.contains("axes") ? args["axes"].toBool() : false;
    bool names = args.contains("names") ? args["names"].toBool() : false;

    try {
        auto resolved = resolveSkeletonEntity(entityName);
        if (!resolved.error.isEmpty()) return resolved.error;

        // Use isSkeletonDebugActive (checks object existence) rather than
        // isSkeletonShown (checks bones visibility) so that toggling works
        // even when only axes or names are shown.
        bool currentlyActive = resolved.animWidget->isSkeletonDebugActive(resolved.entity);
        bool show = args.contains("show") ? args["show"].toBool() : !currentlyActive;

        if (!resolved.animWidget->toggleSkeletonDebug(resolved.entity, show))
            return makeErrorResult(QString("Error: Failed to toggle skeleton debug on entity '%1'").arg(entityName));

        if (show) {
            SkeletonDebug* sd = resolved.animWidget->getSkeletonDebug(resolved.entity);
            if (sd) {
                sd->showBones(bones);
                sd->showAxes(axes);
                sd->showNames(names);
            }
        }

        return makeSuccessResult(QString("Skeleton debug %1 on entity '%2' (bones=%3, axes=%4, names=%5)")
            .arg(show ? "shown" : "hidden", entityName,
                 bones ? "true" : "false", axes ? "true" : "false", names ? "true" : "false"));

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolToggleBoneWeights(const QJsonObject &args)
{
    QString entityName = args["entity"].toString();
    if (entityName.isEmpty())
        return makeErrorResult("Error: 'entity' is required");

    try {
        auto resolved = resolveSkeletonEntity(entityName);
        if (!resolved.error.isEmpty()) return resolved.error;

        // Determine show state: if 'show' provided, use it; otherwise toggle
        bool currentlyShown = resolved.animWidget->isBoneWeightsShown(resolved.entity);
        bool show = args.contains("show") ? args["show"].toBool() : !currentlyShown;

        if (!resolved.animWidget->toggleBoneWeights(resolved.entity, show))
            return makeErrorResult(QString("Error: Failed to toggle bone weights on entity '%1'").arg(entityName));

        // Optionally select a specific bone
        QString boneName = args["bone"].toString();
        if (show && !boneName.isEmpty()) {
            Ogre::SkeletonInstance* skeleton = resolved.entity->getSkeleton();
            if (skeleton->hasBone(boneName.toStdString())) {
                Ogre::Bone* bone = skeleton->getBone(boneName.toStdString());
                unsigned short boneIndex = bone->getHandle();
                BoneWeightOverlay* overlay = resolved.animWidget->getBoneWeightOverlay(resolved.entity);
                if (overlay)
                    overlay->setSelectedBone(boneIndex);
            } else {
                return makeSuccessResult(QString("Bone weight overlay shown on entity '%1', but bone '%2' not found in skeleton")
                    .arg(entityName, boneName));
            }
        }

        return makeSuccessResult(QString("Bone weight overlay %1 on entity '%2'")
            .arg(show ? "shown" : "hidden", entityName));

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
    }
}

void MCPServer::onAnimationTick()
{
    const float dt = 0.016f; // ~16ms per tick
    for (auto it = m_animations.begin(); it != m_animations.end(); ++it) {
        NodeAnimation &anim = it.value();
        if (!anim.node) continue;
        if (anim.yawSpeed != 0)
            anim.node->yaw(Ogre::Degree(anim.yawSpeed * dt));
        if (anim.pitchSpeed != 0)
            anim.node->pitch(Ogre::Degree(anim.pitchSpeed * dt));
        if (anim.rollSpeed != 0)
            anim.node->roll(Ogre::Degree(anim.rollSpeed * dt));
    }
}

QJsonObject MCPServer::toolToggleNormals(const QJsonObject &args)
{
    if (!m_mainWindow)
        return makeErrorResult("Error: MainWindow not available. Run with --with-mcp flag.");

    NormalVisualizer* visualizer = m_mainWindow->findChild<NormalVisualizer*>();
    if (!visualizer)
        return makeErrorResult("Error: NormalVisualizer not found");

    bool show = args.contains("show") ? args["show"].toBool() : !visualizer->isVisible();
    visualizer->setVisible(show);

    return makeSuccessResult(QString("Normals %1").arg(show ? "shown" : "hidden"));
}

QJsonObject MCPServer::toolToggleMeshInfo(const QJsonObject &args)
{
    if (!m_mainWindow)
        return makeErrorResult("Error: MainWindow not available. Run with --with-mcp flag.");

    MeshInfoOverlay* overlay = m_mainWindow->findChild<MeshInfoOverlay*>();
    if (!overlay)
        return makeErrorResult("Error: MeshInfoOverlay not found");

    bool show = args.contains("show") ? args["show"].toBool() : !overlay->isVisible();
    overlay->setVisible(show);

    return makeSuccessResult(QString("Mesh info overlay %1").arg(show ? "shown" : "hidden"));
}

QJsonObject MCPServer::toolMergeAnimations(const QJsonObject &args)
{
    try {
        Manager* mgr = Manager::getSingletonPtr();
        if (!mgr) {
            return makeErrorResult("Error: Manager not available");
        }

        // Get base entity name (optional — defaults to first entity with skeleton)
        QString baseName = args["base_entity"].toString();
        Ogre::Entity* baseEntity = nullptr;

        // Collect all entities with skeletons
        QList<Ogre::Entity*> allEntities = mgr->getEntities();
        QList<Ogre::Entity*> skeletonEntities;
        for (auto* ent : allEntities) {
            if (ent && ent->hasSkeleton())
                skeletonEntities.append(ent);
        }

        if (skeletonEntities.size() < 2) {
            return makeErrorResult("Error: Need at least 2 entities with skeletons in the scene to merge. "
                                   "Use load_mesh to load mesh files first.");
        }

        // Resolve base entity
        if (!baseName.isEmpty()) {
            for (auto* ent : skeletonEntities) {
                if (QString::fromStdString(ent->getName()) == baseName) {
                    baseEntity = ent;
                    break;
                }
            }
            if (!baseEntity) {
                return makeErrorResult(QString("Error: Entity '%1' not found or has no skeleton").arg(baseName));
            }
        } else {
            baseEntity = skeletonEntities.first();
        }

        // Check skeleton compatibility
        Ogre::SkeletonPtr baseSkel = baseEntity->getMesh()->getSkeleton();
        for (auto* ent : skeletonEntities) {
            if (ent == baseEntity) continue;
            if (!AnimationMerger::areSkeletonsCompatible(baseSkel, ent->getMesh()->getSkeleton())) {
                return makeErrorResult(QString("Error: Skeleton of '%1' is incompatible with base entity '%2'")
                    .arg(QString::fromStdString(ent->getName()),
                         QString::fromStdString(baseEntity->getName())));
            }
        }

        // Perform the merge
        QString errorMsg;
        Ogre::Entity* merged = AnimationMerger::mergeAnimations(baseEntity, skeletonEntities, errorMsg);
        if (!merged) {
            return makeErrorResult(QString("Error: Merge failed — %1").arg(errorMsg));
        }

        // Clean up: remove non-base entities from scene (like the GUI does)
        SelectionSet::getSingleton()->clear();

        QList<Ogre::SceneNode*> nodesToDestroy;
        for (auto* ent : skeletonEntities) {
            if (ent == baseEntity) continue;
            if (auto* node = ent->getParentSceneNode())
                nodesToDestroy.append(node);
        }
        for (auto* node : nodesToDestroy)
            Manager::getSingleton()->destroySceneNode(node);

        // Select the merged entity
        SelectionSet::getSingleton()->append(baseEntity);

        // Count animations on the merged entity
        unsigned short animCount = merged->getMesh()->getSkeleton()->getNumAnimations();

        // Build result with animation list
        QString result = QString("Successfully merged animations into '%1'. Total animations: %2\n\nAnimations:")
            .arg(QString::fromStdString(merged->getName()))
            .arg(animCount);

        auto* skel = merged->getMesh()->getSkeleton().get();
        for (unsigned short i = 0; i < animCount; ++i) {
            auto* anim = skel->getAnimation(i);
            result += QString("\n  - %1 (%2s)")
                .arg(QString::fromStdString(anim->getName()))
                .arg(anim->getLength(), 0, 'f', 2);
        }

        return makeSuccessResult(result);

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Error: Ogre exception — %1").arg(e.getFullDescription().c_str()));
    } catch (std::exception& e) {
        return makeErrorResult(QString("Error: %1").arg(e.what()));
    }
}

QJsonObject MCPServer::toolResampleAnimation(const QJsonObject &args)
{
    try {
        Manager* mgr = Manager::getSingletonPtr();
        if (!mgr)
            return makeErrorResult("Error: Manager not available");

        // Resolve entity
        QString entityName = args["entity_name"].toString();
        Ogre::Entity* entity = nullptr;

        QList<Ogre::Entity*> allEntities = mgr->getEntities();
        if (!entityName.isEmpty()) {
            for (auto* ent : allEntities) {
                if (ent && QString::fromStdString(ent->getName()) == entityName) {
                    entity = ent;
                    break;
                }
            }
            if (!entity)
                return makeErrorResult(QString("Error: Entity '%1' not found").arg(entityName));
        } else {
            // Use first entity with a skeleton
            for (auto* ent : allEntities) {
                if (ent && ent->hasSkeleton()) {
                    entity = ent;
                    break;
                }
            }
        }

        if (!entity || !entity->hasSkeleton())
            return makeErrorResult("Error: No entity with skeleton found");

        Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();
        if (!skel)
            return makeErrorResult("Error: No skeleton found");

        QString animName = args["animation_name"].toString();
        int targetKeyframes = args["target_keyframes"].toInt(0);
        int decimateStep = args["decimate_step"].toInt(0);

        if (targetKeyframes <= 0 && decimateStep <= 0)
            return makeErrorResult("Error: Specify 'target_keyframes' (>= 2) for resampling or 'decimate_step' (>= 2) for decimation");

        bool isResample = targetKeyframes >= 2;

        if (isResample && targetKeyframes < 2)
            return makeErrorResult("Error: target_keyframes must be >= 2");
        if (!isResample && decimateStep < 2)
            return makeErrorResult("Error: decimate_step must be >= 2");

        // Collect animation names to process
        std::vector<std::string> animNames;
        if (!animName.isEmpty()) {
            if (!skel->hasAnimation(animName.toStdString()))
                return makeErrorResult(QString("Error: Animation '%1' not found").arg(animName));
            animNames.push_back(animName.toStdString());
        } else {
            for (unsigned short i = 0; i < skel->getNumAnimations(); ++i)
                animNames.push_back(skel->getAnimation(i)->getName());
        }

        int totalRemoved = 0;
        int animsProcessed = 0;
        for (const auto& name : animNames) {
            int removed = isResample
                ? AnimationMerger::resampleAnimation(skel.get(), name, targetKeyframes)
                : AnimationMerger::decimateAnimation(skel.get(), name, decimateStep);
            totalRemoved += removed;
            ++animsProcessed;
        }

        entity->refreshAvailableAnimationState();

        QString op = isResample ? "Resampled" : "Decimated";
        QString detail = isResample
            ? QString("to %1 keyframes").arg(targetKeyframes)
            : QString("with step %1").arg(decimateStep);
        QString result = QString("%1 %2 animation(s) %3 (removed %4 keyframes)")
            .arg(op).arg(animsProcessed).arg(detail).arg(totalRemoved);

        // List resulting animations
        result += "\n\nAnimations:";
        for (unsigned short i = 0; i < skel->getNumAnimations(); ++i) {
            auto* anim = skel->getAnimation(i);
            int maxKf = 0;
            for (const auto& [handle, track] : anim->_getNodeTrackList()) {
                int kfCount = static_cast<int>(track->getNumKeyFrames());
                if (kfCount > maxKf) maxKf = kfCount;
            }
            result += QString("\n  - %1 (%2s, %3 keyframes)")
                .arg(QString::fromStdString(anim->getName()))
                .arg(anim->getLength(), 0, 'f', 2)
                .arg(maxKf);
        }

        return makeSuccessResult(result);

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Error: Ogre exception — %1").arg(e.getFullDescription().c_str()));
    } catch (std::exception& e) {
        return makeErrorResult(QString("Error: %1").arg(e.what()));
    }
}

// Build SimplifyTolerances from MCP args. Reads "preset" via the shared
// AnimationMerger::tolerancesForPreset helper (so CLI/MCP/Inspector stay in
// sync), then layers per-axis overrides from "tolerance",
// "rotation_tolerance_deg", and "scale_tolerance". Sets *outOk to false on
// an unknown preset string so the caller can surface a usage error.
static AnimationMerger::SimplifyTolerances tolerancesFromMcpArgs(const QJsonObject &args, bool *outOk = nullptr)
{
    bool presetOk = true;
    const std::string preset = args.value("preset").toString().toStdString();
    AnimationMerger::SimplifyTolerances tol =
        AnimationMerger::tolerancesForPreset(preset, &presetOk);

    if (args.contains("tolerance"))
        tol.translation = static_cast<float>(args.value("tolerance").toDouble(tol.translation));
    if (args.contains("rotation_tolerance_deg"))
        tol.rotationDeg = static_cast<float>(args.value("rotation_tolerance_deg").toDouble(tol.rotationDeg));
    if (args.contains("scale_tolerance"))
        tol.scale = static_cast<float>(args.value("scale_tolerance").toDouble(tol.scale));

    if (outOk) *outOk = presetOk;
    return tol;
}

QJsonObject MCPServer::toolSimplifyAnimation(const QJsonObject &args)
{
    try {
        Manager* mgr = Manager::getSingletonPtr();
        if (!mgr)
            return makeErrorResult("Error: Manager not available");

        bool presetOk = true;
        AnimationMerger::SimplifyTolerances tol = tolerancesFromMcpArgs(args, &presetOk);
        if (!presetOk)
            return makeErrorResult("Error: Unknown preset. Use conservative, balanced, or aggressive.");

        QString entityName = args["entity_name"].toString();
        Ogre::Entity* entity = nullptr;
        QList<Ogre::Entity*> allEntities = mgr->getEntities();
        if (!entityName.isEmpty()) {
            for (auto* ent : allEntities) {
                if (ent && QString::fromStdString(ent->getName()) == entityName) {
                    entity = ent; break;
                }
            }
            if (!entity)
                return makeErrorResult(QString("Error: Entity '%1' not found").arg(entityName));
        } else {
            for (auto* ent : allEntities) {
                if (ent && ent->hasSkeleton()) { entity = ent; break; }
            }
        }
        if (!entity || !entity->hasSkeleton())
            return makeErrorResult("Error: No entity with skeleton found");

        Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();
        if (!skel)
            return makeErrorResult("Error: No skeleton found");

        QString animName = args["animation_name"].toString();
        std::vector<std::string> animNames;
        if (!animName.isEmpty()) {
            if (!skel->hasAnimation(animName.toStdString()))
                return makeErrorResult(QString("Error: Animation '%1' not found").arg(animName));
            animNames.push_back(animName.toStdString());
        } else {
            for (unsigned short i = 0; i < skel->getNumAnimations(); ++i)
                animNames.push_back(skel->getAnimation(i)->getName());
        }

        int totalRemoved = 0;
        int totalOriginal = 0;
        for (const auto& name : animNames) {
            // Counting raw keyframes via the track list is O(N) — calling
            // analyzeRedundantKeyframes here would re-run the full simplifier
            // pass and discard the result, doubling per-animation work.
            const Ogre::Animation* anim = skel->getAnimation(name);
            for (const auto& [handle, track] : anim->_getNodeTrackList())
                totalOriginal += static_cast<int>(track->getNumKeyFrames());

            totalRemoved += AnimationMerger::simplifyAnimation(skel.get(), name, tol);
        }

        entity->refreshAvailableAnimationState();

        const double pct = totalOriginal > 0 ? (100.0 * totalRemoved / totalOriginal) : 0.0;
        QString result = QString("Simplified %1 animation(s): removed %2/%3 keyframes (%4%) using tolerances "
                                 "translation=%5, rotation=%6°, scale=%7")
            .arg(animNames.size()).arg(totalRemoved).arg(totalOriginal)
            .arg(pct, 0, 'f', 1).arg(tol.translation).arg(tol.rotationDeg).arg(tol.scale);

        result += "\n\nAnimations:";
        for (unsigned short i = 0; i < skel->getNumAnimations(); ++i) {
            auto* anim = skel->getAnimation(i);
            int maxKf = 0;
            for (const auto& [handle, track] : anim->_getNodeTrackList()) {
                int kfCount = static_cast<int>(track->getNumKeyFrames());
                if (kfCount > maxKf) maxKf = kfCount;
            }
            result += QString("\n  - %1 (%2s, %3 keyframes)")
                .arg(QString::fromStdString(anim->getName()))
                .arg(anim->getLength(), 0, 'f', 2)
                .arg(maxKf);
        }
        return makeSuccessResult(result);

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Error: Ogre exception — %1").arg(e.getFullDescription().c_str()));
    } catch (std::exception& e) {
        return makeErrorResult(QString("Error: %1").arg(e.what()));
    }
}

QJsonObject MCPServer::toolAnalyzeAnimation(const QJsonObject &args)
{
    try {
        Manager* mgr = Manager::getSingletonPtr();
        if (!mgr)
            return makeErrorResult("Error: Manager not available");

        bool presetOk = true;
        AnimationMerger::SimplifyTolerances tol = tolerancesFromMcpArgs(args, &presetOk);
        if (!presetOk)
            return makeErrorResult("Error: Unknown preset. Use conservative, balanced, or aggressive.");

        QString entityName = args["entity_name"].toString();
        Ogre::Entity* entity = nullptr;
        QList<Ogre::Entity*> allEntities = mgr->getEntities();
        if (!entityName.isEmpty()) {
            for (auto* ent : allEntities) {
                if (ent && QString::fromStdString(ent->getName()) == entityName) {
                    entity = ent; break;
                }
            }
            if (!entity)
                return makeErrorResult(QString("Error: Entity '%1' not found").arg(entityName));
        } else {
            for (auto* ent : allEntities) {
                if (ent && ent->hasSkeleton()) { entity = ent; break; }
            }
        }
        if (!entity || !entity->hasSkeleton())
            return makeErrorResult("Error: No entity with skeleton found");

        Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();
        if (!skel)
            return makeErrorResult("Error: No skeleton found");

        QString animName = args["animation_name"].toString();
        std::vector<std::string> animNames;
        if (!animName.isEmpty()) {
            if (!skel->hasAnimation(animName.toStdString()))
                return makeErrorResult(QString("Error: Animation '%1' not found").arg(animName));
            animNames.push_back(animName.toStdString());
        } else {
            for (unsigned short i = 0; i < skel->getNumAnimations(); ++i)
                animNames.push_back(skel->getAnimation(i)->getName());
        }

        QString result = QString("Redundant-keyframe analysis (translation=%1, rotation=%2°, scale=%3):")
            .arg(tol.translation).arg(tol.rotationDeg).arg(tol.scale);

        int grandTotal = 0;
        int grandRedundant = 0;
        for (const auto& name : animNames) {
            int total = 0, redundant = 0;
            AnimationMerger::analyzeRedundantKeyframes(skel->getAnimation(name), tol, &total, &redundant);
            grandTotal += total;
            grandRedundant += redundant;
            const double pct = total > 0 ? (100.0 * redundant / total) : 0.0;
            result += QString("\n  %1: %2/%3 keyframes redundant (%4%)")
                .arg(QString::fromStdString(name)).arg(redundant).arg(total).arg(pct, 0, 'f', 1);
        }

        const double totalPct = grandTotal > 0 ? (100.0 * grandRedundant / grandTotal) : 0.0;
        result += QString("\n  Total: %1/%2 keyframes redundant (%3%)")
            .arg(grandRedundant).arg(grandTotal).arg(totalPct, 0, 'f', 1);

        return makeSuccessResult(result);

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Error: Ogre exception — %1").arg(e.getFullDescription().c_str()));
    } catch (std::exception& e) {
        return makeErrorResult(QString("Error: %1").arg(e.what()));
    }
}

QJsonObject MCPServer::toolBakeAnimationFps(const QJsonObject &args)
{
    try {
        Manager* mgr = Manager::getSingletonPtr();
        if (!mgr)
            return makeErrorResult("Error: Manager not available");

        const int targetFps = args.value("fps").toInt(0);
        if (targetFps <= 0)
            return makeErrorResult("Error: 'fps' must be a positive integer (10/15/30/60 are typical).");

        QString entityName = args["entity_name"].toString();
        Ogre::Entity* entity = nullptr;
        QList<Ogre::Entity*> allEntities = mgr->getEntities();
        if (!entityName.isEmpty()) {
            for (auto* ent : allEntities) {
                if (ent && QString::fromStdString(ent->getName()) == entityName) {
                    entity = ent; break;
                }
            }
            if (!entity)
                return makeErrorResult(QString("Error: Entity '%1' not found").arg(entityName));
        } else {
            for (auto* ent : allEntities) {
                if (ent && ent->hasSkeleton()) { entity = ent; break; }
            }
        }
        if (!entity || !entity->hasSkeleton())
            return makeErrorResult("Error: No entity with skeleton found");

        Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();
        if (!skel)
            return makeErrorResult("Error: No skeleton found");

        QString animName = args["animation_name"].toString();
        std::vector<std::string> animNames;
        if (!animName.isEmpty()) {
            if (!skel->hasAnimation(animName.toStdString()))
                return makeErrorResult(QString("Error: Animation '%1' not found").arg(animName));
            animNames.push_back(animName.toStdString());
        } else {
            for (unsigned short i = 0; i < skel->getNumAnimations(); ++i)
                animNames.push_back(skel->getAnimation(i)->getName());
        }

        int totalKeys = 0;
        for (const auto& name : animNames) {
            totalKeys += AnimationMerger::bakeAnimationAtFps(skel.get(), name, targetFps);
        }
        entity->refreshAvailableAnimationState();

        QString result = QString("Baked %1 animation(s) to %2 FPS — %3 total keyframes")
            .arg(animNames.size()).arg(targetFps).arg(totalKeys);
        return makeSuccessResult(result);

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Error: Ogre exception — %1").arg(e.getFullDescription().c_str()));
    } catch (std::exception& e) {
        return makeErrorResult(QString("Error: %1").arg(e.what()));
    }
}

QJsonObject MCPServer::toolSaveScene(const QJsonObject &args)
{
    try {
        QString filePath = args["file_path"].toString();
        if (filePath.isEmpty())
            return makeErrorResult("Error: 'file_path' is required (e.g. /tmp/scene.scene.glb)");

        int result = MeshImporterExporter::sceneExporter(filePath);
        if (result != 0)
            return makeErrorResult("Error: Failed to save scene to " + filePath);

        return makeSuccessResult("Scene saved to " + filePath);
    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Error: Ogre exception — %1").arg(e.getFullDescription().c_str()));
    } catch (std::exception& e) {
        return makeErrorResult(QString("Error: %1").arg(e.what()));
    }
}

QJsonObject MCPServer::toolOpenScene(const QJsonObject &args)
{
    try {
        QString filePath = args["file_path"].toString();
        if (filePath.isEmpty())
            return makeErrorResult("Error: 'file_path' is required");

        if (!QFile::exists(filePath))
            return makeErrorResult("Error: File not found: " + filePath);

        bool ok = MeshImporterExporter::sceneImporter(filePath);
        if (!ok)
            return makeErrorResult("Error: Failed to import scene from " + filePath);

        // Report what was loaded
        Manager* mgr = Manager::getSingletonPtr();
        if (!mgr)
            return makeSuccessResult("Scene loaded from " + filePath);

        auto sceneNodes = mgr->getSceneNodes();
        QString result = QString("Scene loaded from %1. %2 scene node(s):\n")
            .arg(filePath).arg(sceneNodes.size());

        for (auto* node : sceneNodes) {
            QString nodeName = QString::fromStdString(node->getName());
            result += QString("  - %1").arg(nodeName);

            for (auto* obj : node->getAttachedObjects()) {
                if (obj->getMovableType() != "Entity") continue;
                auto* entity = static_cast<Ogre::Entity*>(obj);
                result += QString(" (entity: %1").arg(QString::fromStdString(entity->getName()));
                if (entity->hasSkeleton()) {
                    auto* skel = entity->getMesh()->getSkeleton().get();
                    result += QString(", %1 animation(s)").arg(skel->getNumAnimations());
                    for (unsigned short ai = 0; ai < skel->getNumAnimations(); ++ai) {
                        result += QString("\n      anim[%1]: '%2' (%3s)")
                            .arg(ai)
                            .arg(QString::fromStdString(skel->getAnimation(ai)->getName()))
                            .arg(skel->getAnimation(ai)->getLength(), 0, 'f', 2);
                    }
                }
                result += ")";
            }
            result += "\n";
        }

        return makeSuccessResult(result);
    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Error: Ogre exception — %1").arg(e.getFullDescription().c_str()));
    } catch (std::exception& e) {
        return makeErrorResult(QString("Error: %1").arg(e.what()));
    }
}

QJsonObject MCPServer::toolValidateMesh(const QJsonObject &args)
{
    Q_UNUSED(args);

    if (!hasSelectedEntities())
        return makeErrorResult("No mesh selected. Load a mesh first with load_mesh.");

    // doValidate() is synchronous — safe to call here since Ogre is initialized
    // in headless MCP mode (CPU buffers) or from the main thread with GUI.
    MeshValidator* validator = MeshValidator::instance();
    validator->doValidate();

    QVariantList issues = validator->issues();
    QStringList lines;
    for (const QVariant& v : issues) {
        QVariantMap m = v.toMap();
        QString type = m.value("type").toString();
        QString desc = m.value("description").toString();
        QString prefix = (type == "error") ? "[ERROR] " : (type == "warning") ? "[WARN]  " : "[OK]    ";
        lines << prefix + desc;
    }

    return makeSuccessResult(lines.isEmpty() ? "No issues found." : lines.join("\n"));
}

QJsonObject MCPServer::toolGenerateLods(const QJsonObject &args)
{
    if (!hasSelectedEntities())
        return makeErrorResult("No mesh selected. Load a mesh first with load_mesh.");

    int count = args.contains("count") ? args["count"].toInt() : 3;

    QVariantList reductions;
    if (args.contains("reductions")) {
        for (const QJsonValue& v : args["reductions"].toArray())
            reductions << v.toDouble();
    }

    // `algo` selects the LOD backend. Default `ogre` (Ogre's stock
    // `MeshLodGenerator`). Pass `"meshopt"` to use meshoptimizer's
    // attribute-aware `simplifyWithAttributes` (issue #398) — it
    // preserves UV seams + skin weights but in practice tends to
    // produce a softer silhouette than Ogre's path.
    QString algoStr = args.contains("algo")
        ? args["algo"].toString().toLower() : QStringLiteral("ogre");
    if (algoStr != "meshopt" && algoStr != "ogre") {
        return makeErrorResult(
            QString("Invalid algo '%1' (expected 'meshopt' or 'ogre').").arg(algoStr));
    }
    const auto algoEnum = (algoStr == "meshopt")
        ? MeshLodController::Algorithm::Meshopt
        : MeshLodController::Algorithm::Ogre;

    SentryReporter::addBreadcrumb("ai.tool_call",
        QString("generate_lods count=%1 algo=%2").arg(count).arg(algoStr));

    QString errorMsg = captureLodControllerError([&]() {
        MeshLodController::instance()->generateLods(count, reductions, algoEnum);
    });
    if (!errorMsg.isEmpty())
        return makeErrorResult(errorMsg);

    QVariantList info = MeshLodController::instance()->lodLevelInfo();
    // info[0] is the base mesh; remaining entries are the generated LODs
    int actualLods = info.size() > 1 ? info.size() - 1 : 0;
    QStringList lines;
    lines << QString("Generated %1 LOD level(s):").arg(actualLods);
    for (const QVariant& v : info) {
        QVariantMap m = v.toMap();
        lines << QString("  %1: %2 triangles")
                 .arg(m["label"].toString()).arg(m["triangles"].toInt());
    }
    return makeSuccessResult(lines.join("\n"));
}

QJsonObject MCPServer::toolGenerateAutoLods(const QJsonObject &args)
{
    Q_UNUSED(args);

    if (!hasSelectedEntities())
        return makeErrorResult("No mesh selected. Load a mesh first with load_mesh.");

    QString errorMsg = captureLodControllerError([&]() {
        MeshLodController::instance()->generateAutoLods();
    });
    if (!errorMsg.isEmpty())
        return makeErrorResult(errorMsg);

    QVariantList info = MeshLodController::instance()->lodLevelInfo();
    QStringList lines;
    lines << "Auto LOD levels generated:";
    for (const QVariant& v : info) {
        QVariantMap m = v.toMap();
        lines << QString("  %1: %2 triangles")
                 .arg(m["label"].toString()).arg(m["triangles"].toInt());
    }
    return makeSuccessResult(lines.join("\n"));
}

QJsonObject MCPServer::toolRemoveLods(const QJsonObject &args)
{
    Q_UNUSED(args);

    if (!hasSelectedEntities())
        return makeErrorResult("No mesh selected. Load a mesh first with load_mesh.");

    MeshLodController::instance()->removeLods();
    return makeSuccessResult("LOD levels removed.");
}

QJsonObject MCPServer::toolGetLodInfo(const QJsonObject &args)
{
    Q_UNUSED(args);

    if (!hasSelectedEntities())
        return makeErrorResult("No mesh selected. Load a mesh first with load_mesh.");

    QVariantList info = MeshLodController::instance()->lodLevelInfo();
    if (info.isEmpty())
        return makeSuccessResult("No LOD info available (load a mesh first).");

    QStringList lines;
    for (const QVariant& v : info) {
        QVariantMap m = v.toMap();
        lines << QString("%1: %2 triangles")
                 .arg(m["label"].toString()).arg(m["triangles"].toInt());
    }
    return makeSuccessResult(lines.join("\n"));
}

// NOSONAR(cpp:S5817) — ToolHandler is a non-const member-fn pointer (matching
// every other tool method in this class); marking just this one const would
// break the registry signature in MCPServer.h.
QJsonObject MCPServer::toolGetMemoryUsage(const QJsonObject &args)
{
    try {
        if (const Manager* mgr = Manager::getSingletonPtr(); !mgr)
            return makeErrorResult("Error: Manager not available");

        quint64 budget = 0;
        if (args.contains("budget")) {
            const QString spec = args.value("budget").toString();
            if (!spec.isEmpty()) {
                budget = MemoryEstimator::parseBudget(spec);
                if (budget == 0)
                    return makeErrorResult(
                        QString("Invalid budget '%1' — use e.g. '50MB', '1GB'").arg(spec));
            }
        }

        SceneMemoryReport report = MemoryEstimator::estimateScene(budget);

        // Human-readable text goes in the standard `content` field; machine
        // consumers (LLM tool wrappers, CI scripts) read the structured
        // `memory` payload alongside it.
        QJsonObject result = makeSuccessResult(MemoryEstimator::toText(report));
        result["memory"] = MemoryEstimator::toJson(report);
        return result;
    } catch (Ogre::Exception& e) {
        return makeErrorResult(
            QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
    }
}

// NOSONAR(cpp:S5817) — ToolHandler is a non-const member-fn pointer (matching
// every other tool method in this class); marking just this one const would
// break the registry signature in MCPServer.h.
QJsonObject MCPServer::toolAnalyzeDrawCalls(const QJsonObject &args)
{
    Q_UNUSED(args);
    try {
        if (const Manager* mgr = Manager::getSingletonPtr(); !mgr)
            return makeErrorResult("Error: Manager not available");

        const DrawCallReport report = DrawCallAnalyzer::analyzeScene();
        QJsonObject result = makeSuccessResult(DrawCallAnalyzer::toText(report));
        result["drawCalls"] = DrawCallAnalyzer::toJson(report);
        return result;
    } catch (Ogre::Exception& e) {
        return makeErrorResult(
            QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
    }
}

// NOSONAR(cpp:S5817) — ToolHandler is a non-const member-fn pointer (matching
// every other tool method in this class); marking just this one const would
// break the registry signature in MCPServer.h.
QJsonObject MCPServer::toolOptimizeVertexCache(const QJsonObject &args)
{
    // Args: rewrite (bool, default false). When true, the in-memory index
    // buffers are rewritten; otherwise the tool only reports ACMR.
    try {
        if (const Manager* mgr = Manager::getSingletonPtr(); !mgr)
            return makeErrorResult("Error: Manager not available");

        const bool rewrite = args.value("rewrite").toBool(false);

        VertexCacheReport aggregate;
        for (const Ogre::SceneNode* node : Manager::getSingleton()->getSceneNodes()) {
            if (!node) continue;
            for (unsigned i = 0; i < node->numAttachedObjects(); ++i) {
                Ogre::MovableObject* obj = node->getAttachedObject(i);
                if (!obj || obj->getMovableType() != "Entity") continue;
                auto* entity = static_cast<Ogre::Entity*>(obj);
                VertexCacheOptimizer::mergeReport(
                    aggregate, VertexCacheOptimizer::analyzeEntity(entity, rewrite));
            }
        }
        VertexCacheOptimizer::finalize(aggregate);

        QJsonObject result = makeSuccessResult(VertexCacheOptimizer::toText(aggregate));
        result["vertexCache"] = VertexCacheOptimizer::toJson(aggregate);
        return result;
    } catch (Ogre::Exception& e) {
        return makeErrorResult(
            QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
    }
}

namespace {

// Translate the JSON args into a reduction fraction. Returns -1.0 when the
// caller forgot to provide one of the three target keys; caller turns that
// into a user-facing error.
double resolveMcpReduction(const QJsonObject& args,
                           int currentTris, int currentVerts)
{
    if (args.contains("reduction"))
        return MeshDecimator::clampReduction(args.value("reduction").toDouble());
    if (args.contains("target_tris"))
        return MeshDecimator::reductionFromTargetTris(
            currentTris, args.value("target_tris").toInt());
    if (args.contains("target_verts"))
        return MeshDecimator::reductionFromTargetVerts(
            currentVerts, args.value("target_verts").toInt());
    return -1.0;
}

} // namespace

// NOSONAR(cpp:S5817) — ToolHandler is a non-const member-fn pointer (matching
// every other tool method in this class); marking just this one const would
// break the registry signature in MCPServer.h.
QJsonObject MCPServer::toolDecimateMesh(const QJsonObject &args)
{
    // Args (one wins, in priority order):
    //   reduction (double 0..1)       — drop this fraction of triangles
    //   target_tris (int)             — reduce to approximately this many tris
    //   target_verts (int)            — reduce to approximately this many verts
    //   dry_run (bool, default false) — projected report only, no mutation
    try {
        if (const Manager* mgr = Manager::getSingletonPtr(); !mgr)
            return makeErrorResult("Error: Manager not available");

        // Decimation is destructive — operate on the user's selection, not
        // an arbitrary scene entity. The previous "first entity in scene"
        // path could mutate the wrong asset in multi-mesh scenes.
        const SelectionSet* sel = SelectionSet::getSingleton();
        const QList<Ogre::Entity*> selected = sel ? sel->getResolvedEntities()
                                                  : QList<Ogre::Entity*>{};
        if (selected.isEmpty())
            return makeErrorResult(
                "No mesh selected. Select the entity to decimate (load_mesh + click) first.");
        Ogre::Entity* target = selected.first();
        if (!target)
            return makeErrorResult("Selected entity is null.");

        const bool dryRun = args.value("dry_run").toBool(false);

        int currentTris = 0;
        int currentVerts = 0;
        MeshDecimator::countBaseline(target, currentTris, currentVerts);

        const double reduction = resolveMcpReduction(args, currentTris, currentVerts);
        if (reduction < 0.0)
            return makeErrorResult(
                "Pass one of: reduction (0..1), target_tris, or target_verts.");

        QString algoStr = args.contains("algo")
            ? args["algo"].toString().toLower() : QStringLiteral("ogre");
        if (algoStr != "ogre" && algoStr != "meshopt") {
            return makeErrorResult(
                QString("Invalid algo '%1' (expected 'ogre' or 'meshopt').").arg(algoStr));
        }
        const auto algoEnum = (algoStr == "meshopt")
            ? MeshDecimator::Algorithm::Meshopt
            : MeshDecimator::Algorithm::Ogre;

        const DecimationReport report = dryRun
            ? MeshDecimator::projectEntity(target, reduction)
            : MeshDecimator::decimateEntity(target, reduction, algoEnum);

        // A real (non-dry-run) decimation that didn't apply means the
        // generator failed — automation should treat that as an error, not
        // assume the mesh was modified.
        if (!dryRun && !report.applied && reduction > 0.0) {
            return makeErrorResult(
                QString("Decimation failed: %1 backend could not produce a reduced mesh. "
                        "The mesh may not be suitable (e.g. zero index data).").arg(algoStr));
        }

        QJsonObject result = makeSuccessResult(MeshDecimator::toText(report));
        result["decimation"] = MeshDecimator::toJson(report);
        return result;
    } catch (Ogre::Exception& e) {
        return makeErrorResult(
            QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
    }
}

// Helper methods

QJsonObject MCPServer::toolListFiles(const QJsonObject &args)
{
    QString path = args["path"].toString();
    if (path.isEmpty())
        path = QDir::homePath();

    QDir dir(path);
    if (!dir.exists())
        return makeErrorResult(QString("Error: Directory '%1' does not exist").arg(path));

    QString pattern = args["pattern"].toString();
    QStringList nameFilters;
    if (!pattern.isEmpty())
        nameFilters << pattern;

    QFileInfoList entries = dir.entryInfoList(
        nameFilters,
        QDir::AllEntries | QDir::NoDotAndDotDot,
        QDir::DirsFirst | QDir::Name);

    // Cap at 200 entries to keep response compact
    const int maxEntries = 200;
    QStringList lines;
    lines << QString("Directory: %1").arg(dir.absolutePath());
    lines << QString("Entries: %1%2").arg(
        QString::number(qMin(entries.size(), maxEntries)),
        entries.size() > maxEntries ? QString(" (showing first %1 of %2)").arg(maxEntries).arg(entries.size()) : "");
    lines << "";

    for (int i = 0; i < qMin(entries.size(), maxEntries); ++i) {
        const QFileInfo& fi = entries[i];
        if (fi.isDir()) {
            lines << QString("[dir]  %1/").arg(fi.fileName());
        } else {
            // Human-readable size
            qint64 sz = fi.size();
            QString sizeStr;
            if (sz < 1024)            sizeStr = QString("%1 B").arg(sz);
            else if (sz < 1024*1024)  sizeStr = QString("%1 KB").arg(sz / 1024);
            else                      sizeStr = QString("%1 MB").arg(sz / (1024*1024));
            lines << QString("[file] %1  (%2)").arg(fi.fileName(), sizeStr);
        }
    }

    return makeSuccessResult(lines.join("\n"));
}

QJsonObject MCPServer::toolSearchFiles(const QJsonObject &args)
{
    QString startPath = args["path"].toString();
    if (startPath.isEmpty())
        startPath = QDir::homePath();

    QString query = args["query"].toString();
    if (query.isEmpty())
        return makeErrorResult("Error: 'query' is required (e.g. '*.fbx', 'wood*', 'model.obj')");

    QDir startDir(startPath);
    if (!startDir.exists())
        return makeErrorResult(QString("Error: Directory '%1' does not exist").arg(startPath));

    // Recursive search with depth limit
    int maxDepth = qBound(1, args["max_depth"].toInt(5), 10);
    int maxResults = 100;
    QStringList results;

    std::function<void(const QDir&, int)> searchDir = [&](const QDir& dir, int depth) {
        if (depth > maxDepth || results.size() >= maxResults)
            return;

        // Match files against the query pattern
        QFileInfoList files = dir.entryInfoList(
            QStringList{query}, QDir::Files, QDir::Name);
        for (const QFileInfo& fi : files) {
            if (results.size() >= maxResults) break;
            qint64 sz = fi.size();
            QString sizeStr;
            if (sz < 1024)            sizeStr = QString("%1 B").arg(sz);
            else if (sz < 1024*1024)  sizeStr = QString("%1 KB").arg(sz / 1024);
            else                      sizeStr = QString("%1 MB").arg(sz / (1024*1024));
            results << QString("%1  (%2)").arg(fi.absoluteFilePath(), sizeStr);
        }

        // Recurse into subdirectories
        QFileInfoList dirs = dir.entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo& di : dirs) {
            if (results.size() >= maxResults) break;
            searchDir(QDir(di.absoluteFilePath()), depth + 1);
        }
    };

    searchDir(startDir, 1);

    if (results.isEmpty())
        return makeSuccessResult(QString("No files matching '%1' found in %2 (depth %3)")
            .arg(query, startDir.absolutePath()).arg(maxDepth));

    QStringList lines;
    lines << QString("Found %1 file(s) matching '%2' in %3:")
        .arg(results.size()).arg(query, startDir.absolutePath());
    lines << "";
    lines += results;
    if (results.size() >= maxResults)
        lines << QString("\n(results capped at %1)").arg(maxResults);

    return makeSuccessResult(lines.join("\n"));
}

QJsonObject MCPServer::toolReadFile(const QJsonObject &args)
{
    QString path = args["path"].toString();
    if (path.isEmpty())
        return makeErrorResult("Error: 'path' is required");

    QFileInfo fi(path);
    if (!fi.exists())
        return makeErrorResult(QString("Error: File '%1' does not exist").arg(path));
    if (!fi.isFile())
        return makeErrorResult(QString("Error: '%1' is not a file").arg(path));

    // Reject binary files by extension
    static const QStringList binaryExts = {
        "png", "jpg", "jpeg", "bmp", "tga", "gif", "ico", "tif", "tiff",
        "mesh", "skeleton", "exe", "dll", "dylib", "so", "o", "a",
        "zip", "gz", "tar", "rar", "7z",
        "mp3", "wav", "ogg", "mp4", "avi", "mov",
        "pdf", "doc", "docx", "xls", "ppt"
    };
    if (binaryExts.contains(fi.suffix().toLower()))
        return makeErrorResult(QString("Error: Cannot read binary file '%1'").arg(fi.fileName()));

    // Size limit: 1 MB
    if (fi.size() > 1024 * 1024)
        return makeErrorResult(QString("Error: File too large (%1 MB). Max 1 MB.").arg(fi.size() / (1024*1024)));

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return makeErrorResult(QString("Error: Cannot open '%1': %2").arg(path, file.errorString()));

    int maxLines = qBound(1, args["max_lines"].toInt(100), 500);
    QStringList lines;
    QTextStream stream(&file);
    while (!stream.atEnd() && lines.size() < maxLines)
        lines << stream.readLine();

    bool truncated = !stream.atEnd();
    QString header = QString("File: %1 (%2 lines%3)\n---\n").arg(
        fi.fileName(),
        QString::number(lines.size()),
        truncated ? ", truncated" : "");

    return makeSuccessResult(header + lines.join("\n"));
}

QJsonObject MCPServer::toolDeleteEntity(const QJsonObject &args)
{
    QString name = args["name"].toString();
    if (name.isEmpty())
        return makeErrorResult("Error: 'name' is required — specify the entity/node name to delete.");

    Ogre::SceneNode* node = findSceneNodeByName(name);
    if (!node)
        return makeErrorResult(QString("Error: Node '%1' not found").arg(name));

    // Deselect first (same as UI delete flow)
    SelectionSet* sel = SelectionSet::getSingleton();
    if (sel) {
        sel->removeOne(node);
    }

    // Destroy the node properly (same as TransformOperator::removeSelected)
    Manager::getSingleton()->destroySceneNode(node);

    return makeSuccessResult(QString("Deleted '%1' from the scene.").arg(name));
}

QJsonObject MCPServer::toolDuplicateEntity(const QJsonObject &args)
{
    QString name = args["name"].toString();

    Ogre::SceneNode* sourceNode = nullptr;
    if (!name.isEmpty()) {
        sourceNode = findSceneNodeByName(name);
        if (!sourceNode)
            return makeErrorResult(QString("Error: Node '%1' not found").arg(name));
    } else {
        // Duplicate current selection
        SelectionSet* sel = SelectionSet::getSingleton();
        if (!sel || sel->getNodesCount() == 0)
            return makeErrorResult("Error: No name provided and no scene nodes selected.");
        sourceNode = sel->getNodesSelectionList().first();
    }

    Ogre::SceneNode* clone = Manager::getSingleton()->duplicateSceneNode(sourceNode);
    if (!clone)
        return makeErrorResult("Error: Failed to duplicate node.");

    // Push undo command so MCP duplication is reversible (same as UI path)
    QList<Ogre::SceneNode*> sources = {sourceNode};
    QList<Ogre::SceneNode*> clones = {clone};
    UndoManager::getSingleton()->push(new DuplicateCommand(sources, clones));

    return makeSuccessResult(QString("Duplicated '%1' as '%2'")
        .arg(QString::fromStdString(sourceNode->getName()),
             QString::fromStdString(clone->getName())));
}

QJsonObject MCPServer::toolGetCameraInfo(const QJsonObject &args)
{
    Q_UNUSED(args);
    auto* top = TransformOperator::getSingleton();
    OgreWidget* ogreWidget = top ? top->getActiveWidget() : nullptr;
    // Fallback to any viewport if no active widget yet
    if (!ogreWidget && m_mainWindow)
        ogreWidget = m_mainWindow->findChild<OgreWidget*>();
    if (!ogreWidget || !ogreWidget->getSpaceCamera())
        return makeErrorResult("Error: No active viewport");

    SpaceCamera* cam = ogreWidget->getSpaceCamera();
    Ogre::Camera* ogreCam = cam->getCamera();
    if (!ogreCam)
        return makeErrorResult("Error: No Ogre camera");

    Ogre::Vector3 pos = ogreCam->getDerivedPosition();
    Ogre::Vector3 dir = ogreCam->getDerivedDirection();
    Ogre::Quaternion orient = ogreCam->getDerivedOrientation();

    QStringList lines;
    lines << QString("Camera position: [%1, %2, %3]").arg(pos.x, 0, 'f', 2).arg(pos.y, 0, 'f', 2).arg(pos.z, 0, 'f', 2);
    lines << QString("Camera direction: [%1, %2, %3]").arg(dir.x, 0, 'f', 3).arg(dir.y, 0, 'f', 3).arg(dir.z, 0, 'f', 3);
    lines << QString("Camera orientation: [w=%1, x=%2, y=%3, z=%4]")
        .arg(orient.w, 0, 'f', 3).arg(orient.x, 0, 'f', 3).arg(orient.y, 0, 'f', 3).arg(orient.z, 0, 'f', 3);
    lines << QString("Near clip: %1  Far clip: %2").arg(ogreCam->getNearClipDistance()).arg(ogreCam->getFarClipDistance());

    return makeSuccessResult(lines.join("\n"));
}

QJsonObject MCPServer::toolCameraControl(const QJsonObject &args)
{
    auto* top = TransformOperator::getSingleton();
    OgreWidget* ogreWidget = top ? top->getActiveWidget() : nullptr;
    // Fallback to any viewport if no active widget yet
    if (!ogreWidget && m_mainWindow)
        ogreWidget = m_mainWindow->findChild<OgreWidget*>();
    if (!ogreWidget || !ogreWidget->getSpaceCamera())
        return makeErrorResult("Error: No active viewport");

    SpaceCamera* cam = ogreWidget->getSpaceCamera();
    QStringList actions;

    // Frame selection — zoom to fit selected objects
    if (args.contains("frame_selection") && args["frame_selection"].toBool()) {
        cam->frameSelection();
        actions << "Framed selection";
    }

    // Set camera position
    if (args.contains("position")) {
        Ogre::Vector3 pos = parseVector3(args["position"]);
        cam->setCameraPosition(pos);
        actions << QString("Position: [%1, %2, %3]").arg(pos.x).arg(pos.y).arg(pos.z);
    }

    // Set look-at target
    if (args.contains("target")) {
        Ogre::Vector3 target = parseVector3(args["target"]);
        cam->setTargetPosition(target);
        actions << QString("Target: [%1, %2, %3]").arg(target.x).arg(target.y).arg(target.z);
    }

    // Zoom by delta
    if (args.contains("zoom")) {
        Ogre::Real delta = args["zoom"].toDouble();
        cam->zoomByDelta(delta);
        actions << QString("Zoom: %1").arg(delta);
    }

    if (actions.isEmpty())
        return makeErrorResult("Error: No camera action specified. Use position, target, zoom, or frame_selection.");

    return makeSuccessResult("Camera updated:\n" + actions.join("\n"));
}

QJsonObject MCPServer::toolSetSnapSettings(const QJsonObject &args)
{
    auto* top = TransformOperator::getSingleton();
    if (!top)
        return makeErrorResult("Error: TransformOperator not initialized");

    // Validate all fields first to avoid partial mutation
    if (args.contains("grid_size") && args["grid_size"].toDouble() <= 0.0)
        return makeErrorResult("Error: grid_size must be positive");
    if (args.contains("angle_step") && args["angle_step"].toDouble() <= 0.0)
        return makeErrorResult("Error: angle_step must be positive");
    if (args.contains("scale_step") && args["scale_step"].toDouble() <= 0.0)
        return makeErrorResult("Error: scale_step must be positive");

    // Apply all validated fields
    QStringList changes;

    if (args.contains("enabled")) {
        bool enabled = args["enabled"].toBool();
        top->setSnapEnabled(enabled);
        changes << QString("Snap %1").arg(enabled ? "enabled" : "disabled");
    }

    if (args.contains("grid_size")) {
        double gridSize = args["grid_size"].toDouble();
        top->setSnapGridSize(gridSize);
        changes << QString("Grid size: %1").arg(gridSize);
    }

    if (args.contains("angle_step")) {
        double angleStep = args["angle_step"].toDouble();
        top->setSnapAngleStep(angleStep);
        changes << QString("Angle step: %1 degrees").arg(angleStep);
    }

    if (args.contains("scale_step")) {
        double scaleStep = args["scale_step"].toDouble();
        top->setSnapScaleStep(scaleStep);
        changes << QString("Scale step: %1").arg(scaleStep);
    }

    if (changes.isEmpty())
        return makeErrorResult("Error: No snap settings specified. Use enabled, grid_size, angle_step, or scale_step.");

    return makeSuccessResult("Snap settings updated:\n" + changes.join("\n"));
}

QJsonObject MCPServer::toolGetSnapSettings(const QJsonObject &args)
{
    Q_UNUSED(args);
    auto* top = TransformOperator::getSingleton();
    if (!top)
        return makeErrorResult("Error: TransformOperator not initialized");

    QStringList lines;
    lines << QString("Snap enabled: %1").arg(top->isSnapEnabled() ? "true" : "false");
    lines << QString("Grid size: %1 (translation)").arg(top->snapGridSize());
    lines << QString("Angle step: %1 degrees (rotation)").arg(top->snapAngleStep());
    lines << QString("Scale step: %1 (scale)").arg(top->snapScaleStep());
    lines << "";
    lines << "Tip: Hold Ctrl during drag for temporary snap, even when snap is disabled.";

    return makeSuccessResult(lines.join("\n"));
}

QJsonObject MCPServer::toolExportPose(const QJsonObject &args)
{
    QString entityName = args["entity"].toString();
    QString animName = args["animation"].toString();
    double time = args["time"].toDouble(0.0);
    QString outputPath = args["output_path"].toString();

    if (outputPath.isEmpty())
        return makeErrorResult("Error: output_path is required");
    if (animName.isEmpty())
        return makeErrorResult("Error: animation name is required");

    try {
        auto* mgr = Manager::getSingletonPtr();
        if (!mgr) return makeErrorResult("Error: Manager not available");

        // Find the entity
        Ogre::Entity* entity = nullptr;
        if (!entityName.isEmpty()) {
            auto* sm = mgr->getSceneMgr();
            if (sm->hasEntity(entityName.toStdString()))
                entity = sm->getEntity(entityName.toStdString());
        }
        if (!entity) {
            // Try the first selected entity, or first entity in scene
            SelectionSet* sel = SelectionSet::getSingleton();
            if (sel && sel->getEntitiesCount() > 0) {
                entity = sel->getEntity(0);
            } else {
                auto& entities = mgr->getEntities();
                if (!entities.isEmpty())
                    entity = entities.first();
            }
        }

        if (!entity)
            return makeErrorResult("Error: No entity found. Specify entity name or select one.");
        if (!entity->hasSkeleton())
            return makeErrorResult("Error: Entity has no skeleton — cannot export pose.");

        // Set animation time
        auto* animStates = entity->getAllAnimationStates();
        if (!animStates || !animStates->hasAnimationState(animName.toStdString()))
            return makeErrorResult(QString("Error: Animation '%1' not found").arg(animName));

        auto* animState = animStates->getAnimationState(animName.toStdString());
        animState->setEnabled(true);
        animState->setTimePosition(static_cast<float>(time));

        int result = MeshImporterExporter::exportCurrentPose(entity, outputPath);

        animState->setEnabled(false);

        if (result != 0)
            return makeErrorResult(QString("Error: Export failed (code %1)").arg(result));

        return makeSuccessResult(QString("Exported pose to: %1 (animation: %2, time: %3s)")
            .arg(outputPath).arg(animName).arg(time, 0, 'f', 3));

    } catch (std::exception& e) {
        return makeErrorResult(QString("Error exporting pose: %1").arg(e.what()));
    }
}

QJsonObject MCPServer::toolGroupNodes(const QJsonObject &args)
{
    try {
        auto* mgr = Manager::getSingletonPtr();
        if (!mgr) return makeErrorResult("Error: Manager not available");

        QList<Ogre::SceneNode*> nodes;

        if (args.contains("names")) {
            QJsonArray names = args["names"].toArray();
            for (const auto& nameVal : names) {
                QString name = nameVal.toString();
                Ogre::SceneNode* node = mgr->getSceneNode(name);
                if (!node)
                    return makeErrorResult(QString("Error: Scene node '%1' not found").arg(name));
                nodes.append(node);
            }
        } else {
            // Use current selection
            SelectionSet* sel = SelectionSet::getSingleton();
            if (!sel || sel->getNodesCount() < 2)
                return makeErrorResult("Error: At least 2 nodes must be selected or specified");
            nodes = sel->getNodesSelectionList();
        }

        if (nodes.size() < 2)
            return makeErrorResult("Error: At least 2 nodes are required to create a group");

        Ogre::SceneNode* groupNode = mgr->groupNodes(nodes);
        if (!groupNode)
            return makeErrorResult("Error: Failed to create group node");

        UndoManager::getSingleton()->push(new GroupCommand(nodes));

        return makeSuccessResult(QString("Created group '%1' with %2 child nodes")
            .arg(QString::fromStdString(groupNode->getName()))
            .arg(nodes.size()));

    } catch (std::exception& e) {
        return makeErrorResult(QString("Error grouping nodes: %1").arg(e.what()));
    }
}

QJsonObject MCPServer::toolUngroupNode(const QJsonObject &args)
{
    try {
        auto* mgr = Manager::getSingletonPtr();
        if (!mgr) return makeErrorResult("Error: Manager not available");

        Ogre::SceneNode* groupNode = nullptr;

        if (args.contains("name")) {
            QString name = args["name"].toString();
            groupNode = mgr->getSceneNode(name);
            if (!groupNode)
                return makeErrorResult(QString("Error: Scene node '%1' not found").arg(name));
        } else {
            // Use current selection
            SelectionSet* sel = SelectionSet::getSingleton();
            if (!sel || sel->getNodesCount() != 1)
                return makeErrorResult("Error: Select exactly one group node, or specify a name");
            groupNode = sel->getSceneNode(0);
        }

        if (!mgr->isGroupNode(groupNode))
            return makeErrorResult(QString("Error: '%1' is not a group node (must have children and no attached meshes)")
                .arg(QString::fromStdString(groupNode->getName())));

        int childCount = static_cast<int>(groupNode->numChildren());
        QString groupName = QString::fromStdString(groupNode->getName());

        UndoManager::getSingleton()->push(new UngroupCommand(groupNode));
        mgr->ungroupNode(groupNode);

        return makeSuccessResult(QString("Ungrouped '%1': %2 children moved to parent")
            .arg(groupName).arg(childCount));

    } catch (std::exception& e) {
        return makeErrorResult(QString("Error ungrouping node: %1").arg(e.what()));
    }
}

QJsonObject MCPServer::toolReparentNode(const QJsonObject &args)
{
    try {
        auto* mgr = Manager::getSingletonPtr();
        if (!mgr) return makeErrorResult("Error: Manager not available");

        SentryReporter::addBreadcrumb("ai.tool_call", "reparent_node");

        if (!args.contains("node_name"))
            return makeErrorResult("Error: 'node_name' is required");

        QString nodeName = args["node_name"].toString();
        QString newParentName = args.value("new_parent_name").toString();

        // Resolve "root" to empty string (root scene node)
        if (newParentName.toLower() == "root")
            newParentName = QString();

        auto* sceneMgr = mgr->getSceneMgr();
        if (!sceneMgr) return makeErrorResult("Error: SceneManager not available");

        Ogre::SceneNode* node = mgr->getSceneNode(nodeName);
        if (!node)
            return makeErrorResult(QString("Error: Scene node '%1' not found").arg(nodeName));

        Ogre::SceneNode* newParent = nullptr;
        if (newParentName.isEmpty()) {
            newParent = sceneMgr->getRootSceneNode();
        } else {
            newParent = mgr->getSceneNode(newParentName);
            if (!newParent)
                return makeErrorResult(QString("Error: Target parent node '%1' not found").arg(newParentName));
        }

        // Validate
        if (node == newParent)
            return makeErrorResult("Error: Cannot reparent a node to itself");

        if (Manager::isDescendantOf(newParent, node))
            return makeErrorResult("Error: Cannot reparent a node into its own subtree (would create a cycle)");

        if (node->getParent() == newParent)
            return makeErrorResult(QString("Error: Node '%1' is already a child of '%2'")
                .arg(nodeName, newParentName.isEmpty() ? "root" : newParentName));

        // Capture old state for undo
        Ogre::SceneNode* oldParent = static_cast<Ogre::SceneNode*>(node->getParent());
        QString oldParentName = (oldParent && oldParent != sceneMgr->getRootSceneNode())
            ? QString::fromStdString(oldParent->getName()) : QString();
        Ogre::Vector3 oldLocalPos = node->getPosition();
        Ogre::Quaternion oldLocalOrient = node->getOrientation();
        Ogre::Vector3 oldLocalScale = node->getScale();

        if (!mgr->reparentNode(node, newParent))
            return makeErrorResult("Error: Reparent operation failed");

        // Capture new local transform
        Ogre::Vector3 newLocalPos = node->getPosition();
        Ogre::Quaternion newLocalOrient = node->getOrientation();
        Ogre::Vector3 newLocalScale = node->getScale();

        QString resolvedNewParentName = (newParent != sceneMgr->getRootSceneNode())
            ? QString::fromStdString(newParent->getName()) : QString();

        UndoManager::getSingleton()->push(new ReparentCommand(
            nodeName, oldParentName, resolvedNewParentName,
            oldLocalPos, oldLocalOrient, oldLocalScale,
            newLocalPos, newLocalOrient, newLocalScale));

        return makeSuccessResult(QString("Reparented '%1' under '%2' (world transform preserved)")
            .arg(nodeName, newParentName.isEmpty() ? "root" : newParentName));

    } catch (std::exception& e) {
        return makeErrorResult(QString("Error reparenting node: %1").arg(e.what()));
    }
}

QJsonObject MCPServer::toolSetPivotMode(const QJsonObject &args)
{
    auto* top = TransformOperator::getSingleton();
    if (!top)
        return makeErrorResult("Error: TransformOperator not initialized");

    QString modeStr = args["mode"].toString().toLower();
    TransformOperator::PivotMode mode;

    if (modeStr == "center")
        mode = TransformOperator::PIVOT_CENTER;
    else if (modeStr == "bottom")
        mode = TransformOperator::PIVOT_BOTTOM;
    else if (modeStr == "origin")
        mode = TransformOperator::PIVOT_ORIGIN;
    else
        return makeErrorResult(QString("Error: Invalid pivot mode '%1'. Must be 'center', 'bottom', or 'origin'.").arg(modeStr));

    SentryReporter::addBreadcrumb("ai.tool_call",
        QString("set_pivot_mode: %1").arg(modeStr));

    top->setPivotMode(mode);
    return makeSuccessResult(QString("Pivot mode set to '%1'").arg(modeStr));
}

QJsonObject MCPServer::toolGetPivotMode(const QJsonObject &args)
{
    Q_UNUSED(args);
    auto* top = TransformOperator::getSingleton();
    if (!top)
        return makeErrorResult("Error: TransformOperator not initialized");

    QString modeStr;
    switch (top->pivotMode()) {
    case TransformOperator::PIVOT_CENTER: modeStr = "center"; break;
    case TransformOperator::PIVOT_BOTTOM: modeStr = "bottom"; break;
    case TransformOperator::PIVOT_ORIGIN: modeStr = "origin"; break;
    }

    QJsonObject result;
    result["content"] = QJsonArray{QJsonObject{{"type", "text"}, {"text", QString("Pivot mode: %1").arg(modeStr)}}};
    result["mode"] = modeStr;
    return result;
}

QJsonObject MCPServer::toolPackTextures(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "pack_textures");

    TextureChannelPacker::PackingSpec spec;
    auto fillChan = [&args](TextureChannelPacker::ChannelSource& dst,
                            const QString& pathKey,
                            const QString& constKey,
                            const QString& invertKey) {
        if (args.contains(pathKey))
            dst.path = args.value(pathKey).toString();
        if (args.contains(constKey))
            dst.constantValue = static_cast<float>(args.value(constKey).toDouble());
        if (args.contains(invertKey))
            dst.invert = args.value(invertKey).toBool();
    };
    fillChan(spec.red,   "red",   "red_constant",   "invert_red");
    fillChan(spec.green, "green", "green_constant", "invert_green");
    fillChan(spec.blue,  "blue",  "blue_constant",  "invert_blue");
    fillChan(spec.alpha, "alpha", "alpha_constant", "invert_alpha");
    if (args.contains("width"))
        spec.outputWidth = args.value("width").toInt();
    if (args.contains("height"))
        spec.outputHeight = args.value("height").toInt();
    if (args.contains("include_alpha"))
        spec.includeAlpha = args.value("include_alpha").toBool();

    const QString outPath = args.value("output").toString();
    if (outPath.isEmpty())
        return makeErrorResult("Error: missing required 'output' argument");

    auto r = TextureChannelPacker::packToFile(spec, outPath);
    if (!r.ok)
        return makeErrorResult(QString("Error: %1").arg(r.error));

    QJsonObject result;
    result["content"] = QJsonArray{QJsonObject{
        {"type", "text"},
        {"text", QString("Packed %1x%2 -> %3").arg(r.usedWidth).arg(r.usedHeight).arg(outPath)}}};
    result["width"]  = r.usedWidth;
    result["height"] = r.usedHeight;
    result["output"] = outPath;
    return result;
}

QJsonObject MCPServer::toolGenerateNormalMap(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "generate_normal_map");

    NormalMapGenerator::GenSpec spec;
    spec.sourcePath = args.value("source").toString();
    if (args.contains("strength"))
        spec.strength = static_cast<float>(args.value("strength").toDouble());
    if (args.contains("width"))
        spec.outputWidth = args.value("width").toInt();
    if (args.contains("height"))
        spec.outputHeight = args.value("height").toInt();
    if (args.contains("invert_r"))
        spec.invertR = args.value("invert_r").toBool();
    if (args.contains("invert_g"))
        spec.invertG = args.value("invert_g").toBool();
    if (args.contains("directx") && args.value("directx").toBool())
        spec.invertG = true;  // alias for invert_g

    const QString outPath = args.value("output").toString();
    if (spec.sourcePath.isEmpty() || outPath.isEmpty())
        return makeErrorResult("Error: missing required 'source' and 'output' arguments");

    auto r = NormalMapGenerator::generateToFile(spec, outPath);
    if (!r.ok)
        return makeErrorResult(QString("Error: %1").arg(r.error));

    QJsonObject result;
    result["content"] = QJsonArray{QJsonObject{
        {"type", "text"},
        {"text", QString("Normal map %1x%2 -> %3").arg(r.usedWidth).arg(r.usedHeight).arg(outPath)}}};
    result["width"]  = r.usedWidth;
    result["height"] = r.usedHeight;
    result["output"] = outPath;
    return result;
}

QJsonObject MCPServer::toolPackAtlas(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "pack_atlas");

    TextureAtlasPacker::AtlasSpec spec;
    // Accept either a JSON array of strings ("inputs": ["a.png", "b.png"]) or
    // a comma-separated single string for CLI-style ergonomics.
    if (args.contains("inputs")) {
        const QJsonValue v = args.value("inputs");
        if (v.isArray()) {
            for (const auto& it : v.toArray())
                spec.sourcePaths.append(it.toString());
        } else {
            const QStringList parts = v.toString().split(',', Qt::SkipEmptyParts);
            for (const QString& p : parts) {
                const QString trimmed = p.trimmed();
                if (!trimmed.isEmpty())
                    spec.sourcePaths.append(trimmed);
            }
        }
    }
    if (args.contains("size")) {
        const int n = args.value("size").toInt();
        spec.atlasWidth = n;
        spec.atlasHeight = n;
    }
    if (args.contains("width"))   spec.atlasWidth  = args.value("width").toInt();
    if (args.contains("height"))  spec.atlasHeight = args.value("height").toInt();
    if (args.contains("padding")) spec.padding     = args.value("padding").toInt();

    const QString outPath = args.value("output").toString();
    const QString manifestPath = args.value("manifest").toString();
    if (spec.sourcePaths.isEmpty() || outPath.isEmpty())
        return makeErrorResult("Error: missing required 'inputs' and 'output' arguments");

    auto r = TextureAtlasPacker::packToFile(spec, outPath);
    if (!r.ok)
        return makeErrorResult(QString("Error: %1").arg(r.error));
    SentryReporter::addBreadcrumb("file.export",
        QString("Atlas %1 tiles -> %2").arg(r.tiles.size()).arg(QFileInfo(outPath).fileName()));

    if (!manifestPath.isEmpty()) {
        const QString json = TextureAtlasPacker::manifestToJson(r, spec.padding);
        const QByteArray bytes = json.toUtf8();
        QFile mf(manifestPath);
        if (!mf.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            return makeErrorResult(QString("Error: could not open manifest path: %1").arg(manifestPath));
        const qint64 written = mf.write(bytes);
        mf.close();
        if (written != bytes.size())
            return makeErrorResult(QString("Error: short write to manifest path: %1 (%2/%3 bytes)")
                                       .arg(manifestPath).arg(written).arg(bytes.size()));
        SentryReporter::addBreadcrumb("file.export",
            QString("Atlas manifest -> %1").arg(QFileInfo(manifestPath).fileName()));
    }

    QJsonObject result;
    result["content"] = QJsonArray{QJsonObject{
        {"type", "text"},
        {"text", QString("Packed %1 tiles into %2x%3 atlas (used %4x%5) -> %6")
            .arg(r.tiles.size())
            .arg(spec.atlasWidth)
            .arg(spec.atlasHeight)
            .arg(r.usedWidth)
            .arg(r.usedHeight)
            .arg(outPath)}}};
    result["tiles_packed"] = r.tiles.size();
    result["atlas_width"]  = spec.atlasWidth;
    result["atlas_height"] = spec.atlasHeight;
    result["used_width"]   = r.usedWidth;
    result["used_height"]  = r.usedHeight;
    result["output"]       = outPath;
    if (!manifestPath.isEmpty())
        result["manifest"] = manifestPath;
    return result;
}

QJsonObject MCPServer::toolApplyAtlas(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "apply_atlas");

    const QString filePath     = args.value("file").toString();
    const QString outputPath   = args.value("output").toString();
    const QString manifestPath = args.value("manifest").toString();
    const QString atlasPath    = args.value("atlas").toString();
    const QString matchMode    = args.value("match").toString().toLower();
    const bool clamp           = args.contains("no_clamp") ? !args.value("no_clamp").toBool() : true;
    const bool keepExtras      = args.contains("keep_extras") && args.value("keep_extras").toBool();

    if (filePath.isEmpty() || outputPath.isEmpty()
        || manifestPath.isEmpty() || atlasPath.isEmpty())
        return makeErrorResult("Error: missing required 'file', 'output', 'manifest', 'atlas' arguments");
    if (!QFileInfo::exists(filePath))
        return makeErrorResult(QString("Error: file not found: %1").arg(filePath));
    if (!QFileInfo::exists(manifestPath))
        return makeErrorResult(QString("Error: manifest not found: %1").arg(manifestPath));
    if (!QFileInfo::exists(atlasPath))
        return makeErrorResult(QString("Error: atlas not found: %1").arg(atlasPath));
    // Same-file guard. canonicalFilePath() returns "" for the not-yet-
    // existing output, so equality on canonical paths would never fire.
    // Compare normalized absolute paths instead, case-folded on the
    // platforms whose filesystems are case-insensitive by default.
    {
        const QString a = QDir::cleanPath(QFileInfo(filePath).absoluteFilePath());
        const QString b = QDir::cleanPath(QFileInfo(outputPath).absoluteFilePath());
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        const auto cs = Qt::CaseInsensitive;
#else
        const auto cs = Qt::CaseSensitive;
#endif
        if (!a.isEmpty() && a.compare(b, cs) == 0)
            return makeErrorResult("Error: output points to the input file; choose a different path.");
    }
    if (!matchMode.isEmpty() && matchMode != "basename" && matchMode != "fullpath")
        return makeErrorResult("Error: 'match' must be 'basename' or 'fullpath'");

    QFile mf(manifestPath);
    if (!mf.open(QIODevice::ReadOnly))
        return makeErrorResult(QString("Error: could not read manifest: %1").arg(manifestPath));
    const QByteArray manifestJson = mf.readAll();
    mf.close();

    auto parsed = ApplyAtlas::parseManifestJson(manifestJson);
    if (!parsed.ok)
        return makeErrorResult(QString("Error: %1").arg(parsed.error));

    if (!Ogre::Root::getSingletonPtr() || !Ogre::Root::getSingletonPtr()->getRenderSystem())
        return makeErrorResult("Ogre render system not initialized");

    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return makeErrorResult("Manager unavailable");

    // Transient import: load the file, apply the atlas, tear it down
    // on scope exit (with the user's prior selection restored).
    SentryReporter::addBreadcrumb("file.import",
        QString("Apply-atlas importing %1").arg(QFileInfo(filePath).fileName()));
    TransientImportSession session(mgr);
    if (QString err = session.runImporter(QFileInfo(filePath).absoluteFilePath());
        !err.isEmpty())
        return makeErrorResult(err);
    const QList<Ogre::Entity*>& entities = session.importedEntities();
    if (entities.isEmpty())
        return makeErrorResult(QString("Failed to load entities from %1").arg(filePath));

    // Register the atlas image's directory as a resource location into
    // the default group (the only group the imported materials see).
    // RAII-cleanup so repeated apply_atlas calls don't accumulate
    // locations in the live process.
    const QFileInfo atlasFi(atlasPath);
    const Ogre::String atlasDir = atlasFi.absolutePath().toStdString();
    bool addedAtlasLoc = false;
    try {
        Ogre::ResourceGroupManager::getSingleton().addResourceLocation(
            atlasDir, "FileSystem",
            Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, false, true);
        addedAtlasLoc = true;
    } catch (const Ogre::Exception&) { /* already-registered is fine */ }
    struct LocationCleanup {
        Ogre::String dir;
        bool added;
        ~LocationCleanup() {
            if (!added) return;
            try {
                Ogre::ResourceGroupManager::getSingleton().removeResourceLocation(
                    dir, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
            } catch (...) {}
        }
    } locationCleanup{atlasDir, addedAtlasLoc};

    ApplyAtlas::ApplyOptions opts;
    opts.matchMode = (matchMode == "fullpath")
        ? ApplyAtlas::MatchMode::FullPath
        : ApplyAtlas::MatchMode::Basename;
    opts.atlasTextureName = atlasFi.fileName();
    opts.clampOutOfRangeUVs = clamp;
    opts.stripNonDiffuseTextures = !keepExtras;

    int totalSubmeshes = 0, totalRewritten = 0, totalOutOfRange = 0;
    QJsonArray perEntity;
    try {
        for (Ogre::Entity* ent : entities) {
            ApplyAtlas::ApplyReport r = ApplyAtlas::applyToEntity(ent, parsed.manifest, opts);
            if (!r.ok) return makeErrorResult(QString("Error: %1").arg(r.error));
            totalSubmeshes  += r.submeshCount();
            totalRewritten  += r.rewrittenCount();
            for (const auto& s : r.submeshes) totalOutOfRange += s.outOfRangeUVs;
            QJsonObject e = r.toJson();
            e["entity"] = QString::fromStdString(ent->getName());
            perEntity.append(e);
        }

        Ogre::Entity* first = entities.first();
        const auto* node = first ? first->getParentSceneNode() : nullptr;
        if (!node) return makeErrorResult("Could not resolve scene node for export");

        static const QMap<QString, QString> formatByExt = {
            {QStringLiteral("fbx"),  QStringLiteral("FBX Binary (*.fbx)")},
            {QStringLiteral("gltf"), QStringLiteral("glTF 2.0 (*.gltf)")},
            {QStringLiteral("glb"),  QStringLiteral("glTF 2.0 Binary (*.glb)")},
            {QStringLiteral("dae"),  QStringLiteral("Collada (*.dae)")},
            {QStringLiteral("obj"),  QStringLiteral("OBJ (*.obj)")},
            {QStringLiteral("ply"),  QStringLiteral("PLY (*.ply)")},
            {QStringLiteral("stl"),  QStringLiteral("STL (*.stl)")},
            {QStringLiteral("mesh"), QStringLiteral("Ogre Mesh (*.mesh)")},
        };
        const QString ext = QFileInfo(outputPath).suffix().toLower();
        if (!formatByExt.contains(ext))
            return makeErrorResult(QString("Error: unsupported export format for .%1").arg(ext));
        if (MeshImporterExporter::exporter(node, outputPath, formatByExt.value(ext)) != 0)
            return makeErrorResult("Export failed");
        SentryReporter::addBreadcrumb("file.export",
            QString("apply_atlas -> %1").arg(QFileInfo(outputPath).fileName()));
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QString("Ogre error: %1")
                                   .arg(QString::fromStdString(e.getFullDescription())));
    } catch (const std::exception& e) {
        return makeErrorResult(QString("Error: %1").arg(QString::fromUtf8(e.what())));
    } catch (...) {
        return makeErrorResult("Unknown error during apply_atlas");
    }

    QJsonObject result;
    result["content"] = QJsonArray{QJsonObject{
        {"type", "text"},
        {"text", QString("Applied atlas '%1' to %2: %3/%4 submeshes rewritten -> %5")
            .arg(atlasFi.fileName())
            .arg(QFileInfo(filePath).fileName())
            .arg(totalRewritten).arg(totalSubmeshes)
            .arg(QFileInfo(outputPath).fileName())}}};
    result["file"]          = QFileInfo(filePath).fileName();
    result["output"]        = QFileInfo(outputPath).fileName();
    result["atlas"]         = atlasFi.fileName();
    result["manifest"]      = QFileInfo(manifestPath).fileName();
    result["submeshes"]     = totalSubmeshes;
    result["rewritten"]     = totalRewritten;
    result["outOfRangeUVs"] = totalOutOfRange;
    result["entities"]      = perEntity;
    return result;
}

QJsonObject MCPServer::toolOptimizeMesh(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "optimize_mesh");

    const QString filePath = args.value("file").toString();
    const QString outputPath = args.value("output").toString();
    if (filePath.isEmpty() || outputPath.isEmpty())
        return makeErrorResult("Error: missing required 'file' and 'output' arguments");
    if (!QFileInfo::exists(filePath))
        return makeErrorResult(QString("Error: file not found: %1").arg(filePath));
    if (QFileInfo(filePath).canonicalFilePath()
        == QFileInfo(outputPath).canonicalFilePath()
        && !QFileInfo(filePath).canonicalFilePath().isEmpty())
        return makeErrorResult("Error: output points to the input file; choose a different path.");

    // The flag set mirrors the CLI: by default the non-destructive
    // optimizations run; a decimation knob triggers the slice D pass.
    bool vertexCache  = args.value("vertex_cache").toBool(true);
    bool simplifyAnim = args.value("simplify_anim").toBool(true);
    // "all" — convenience: enable everything except decimation (which still
    // needs an explicit target).
    if (args.contains("all") && args.value("all").toBool()) {
        vertexCache  = true;
        simplifyAnim = true;
    }
    const bool hasReduction   = args.contains("reduction");
    const bool hasTargetTris  = args.contains("target_tris");
    const bool hasTargetVerts = args.contains("target_verts");
    if ((hasReduction ? 1 : 0) + (hasTargetTris ? 1 : 0) + (hasTargetVerts ? 1 : 0) > 1)
        return makeErrorResult("Error: pass at most one of reduction / target_tris / target_verts.");
    const bool decimate = hasReduction || hasTargetTris || hasTargetVerts;

    // Anim simplify tolerances (default to AnimationMerger Balanced).
    AnimationMerger::SimplifyTolerances tol;
    if (args.contains("simplify_translation_tol"))
        tol.translation = static_cast<float>(args.value("simplify_translation_tol").toDouble());
    if (args.contains("simplify_rotation_deg_tol"))
        tol.rotationDeg = static_cast<float>(args.value("simplify_rotation_deg_tol").toDouble());
    if (args.contains("simplify_scale_tol"))
        tol.scale       = static_cast<float>(args.value("simplify_scale_tol").toDouble());

    // Ensure Ogre headless. Reuse the CLI initOgreHeadless via a direct
    // singleton check — MCP runs inside the editor and Ogre is already up.
    if (!Ogre::Root::getSingletonPtr() || !Ogre::Root::getSingletonPtr()->getRenderSystem())
        return makeErrorResult("Ogre render system not initialized");

    auto* mgr = Manager::getSingletonPtr();
    if (!mgr) return makeErrorResult("Manager unavailable");

    // The MCP server runs inside the editor process, so the importer
    // appends into the user's currently-loaded scene rather than into a
    // fresh one. Snapshot which entities existed before the import so the
    // optimize stages only touch the newly-loaded asset and never mutate
    // the user's other meshes.
    SentryReporter::addBreadcrumb("file.import",
        QString("Optimize importing %1").arg(QFileInfo(filePath).fileName()));
    const QList<Ogre::Entity*> beforeEntities = mgr->getEntities();
    QSet<Ogre::Entity*> beforeSet;
    for (Ogre::Entity* e : beforeEntities) beforeSet.insert(e);

    QList<Ogre::SkeletonPtr> animOnlySkeletons;
    try {
        MeshImporterExporter::importer({QFileInfo(filePath).absoluteFilePath()}, 0,
                                       &animOnlySkeletons);
    } catch (const std::exception& e) {
        return makeErrorResult(QString("Importer threw: %1").arg(QString::fromUtf8(e.what())));
    } catch (...) {
        return makeErrorResult("Importer threw (unknown exception type)");
    }

    QList<Ogre::Entity*> entities;
    for (Ogre::Entity* e : mgr->getEntities()) {
        if (!beforeSet.contains(e))
            entities.append(e);
    }
    if (entities.isEmpty())
        return makeErrorResult(QString("Failed to load any entities from %1").arg(filePath));
    if (decimate && entities.size() > 1)
        return makeErrorResult(
            QString("%1 contains %2 entities; optimize_mesh + decimation supports one entity per file.")
                .arg(filePath).arg(entities.size()));

    // Cleanup: when we leave this function (success or error), destroy
    // the entities we just imported so the user's live scene returns to
    // exactly the state it was in before. The optimize result is already
    // serialized to the output path; no in-scene state needs to survive.
    struct ImportCleanup {
        Manager* mgr;
        QList<Ogre::Entity*> imported;
        ~ImportCleanup() {
            if (!mgr) return;
            // Walk scene-node parents and destroy via Manager so the
            // SelectionSet / SceneTreeModel signals fire correctly. Drop
            // exceptions — best-effort cleanup, the original failure
            // (if any) is the one the caller cares about.
            try {
                std::set<Ogre::SceneNode*> nodes;
                for (Ogre::Entity* e : imported) {
                    if (e && e->getParentSceneNode())
                        nodes.insert(e->getParentSceneNode());
                }
                for (Ogre::SceneNode* sn : nodes)
                    mgr->destroySceneNode(sn);
            } catch (...) {}
        }
    } cleanup{mgr, entities};

    QJsonArray stagesArr;
    try {

    if (vertexCache) {
        VertexCacheReport aggregate;
        for (Ogre::Entity* e : entities)
            VertexCacheOptimizer::mergeReport(aggregate,
                VertexCacheOptimizer::analyzeEntity(e, /*rewrite=*/true));
        VertexCacheOptimizer::finalize(aggregate);
        QJsonObject s;
        s["name"] = "vertex-cache";
        s["applied"] = aggregate.totalReordered > 0 || aggregate.totalTriangles > 0;
        s["summary"] = QString("ACMR %1 -> %2 across %3 triangles, %4 submeshes rewritten")
                           .arg(QString::number(aggregate.weightedAcmrBefore, 'f', 3))
                           .arg(QString::number(aggregate.weightedAcmrAfter, 'f', 3))
                           .arg(aggregate.totalTriangles)
                           .arg(aggregate.totalReordered);
        s["details"] = VertexCacheOptimizer::toJson(aggregate);
        stagesArr.append(s);
    }

    if (decimate) {
        Ogre::Entity* entity = entities.first();
        int currentTris = 0, currentVerts = 0;
        MeshDecimator::countBaseline(entity, currentTris, currentVerts);
        double reduction = 0.0;
        if (hasReduction)
            reduction = MeshDecimator::clampReduction(args.value("reduction").toDouble());
        else if (hasTargetTris)
            reduction = MeshDecimator::reductionFromTargetTris(currentTris, args.value("target_tris").toInt());
        else if (hasTargetVerts)
            reduction = MeshDecimator::reductionFromTargetVerts(currentVerts, args.value("target_verts").toInt());
        QJsonObject s;
        s["name"] = "decimate";
        if (reduction <= 0.0) {
            s["applied"] = false;
            s["summary"] = "target equals or exceeds current count; nothing to do";
            stagesArr.append(s);
        } else {
            const DecimationReport report = MeshDecimator::decimateEntity(entity, reduction);
            s["applied"] = report.applied;
            s["summary"] = QString("%1% triangle reduction (%2 -> %3)")
                               .arg(QString::number(100.0 * report.effectiveReduction(), 'f', 1))
                               .arg(report.totalTrianglesBefore)
                               .arg(report.totalTrianglesAfter);
            s["details"] = MeshDecimator::toJson(report);
            stagesArr.append(s);
            // A positive reduction that didn't apply means the generator
            // failed — surface as an error instead of silent "success".
            if (!report.applied)
                return makeErrorResult(
                    "Decimation failed: MeshLodGenerator could not produce a reduced mesh. "
                    "The mesh may not be suitable (e.g. zero index data).");
        }
    }

    if (simplifyAnim) {
        QJsonObject s;
        s["name"] = "simplify-anim";
        // Walk every skeleton in the imported asset (mesh-level + animation-
        // only). Multi-entity scenes with multiple skeletons get the same
        // simplification treatment everywhere — matches the CLI fix.
        QList<Ogre::SkeletonPtr> skels;
        std::set<std::string> seenSkelNames;
        for (Ogre::Entity* e : entities) {
            if (!e || !e->hasSkeleton() || !e->getMesh()) continue;
            const Ogre::SkeletonPtr s2 = e->getMesh()->getSkeleton();
            if (s2 && seenSkelNames.insert(s2->getName()).second)
                skels.append(s2);
        }
        for (const auto& s2 : animOnlySkeletons) {
            if (s2 && seenSkelNames.insert(s2->getName()).second)
                skels.append(s2);
        }
        int totalRemoved = 0;
        long long totalKeysBefore = 0;
        for (const Ogre::SkeletonPtr& skel : skels) {
            for (unsigned short ai = 0; ai < skel->getNumAnimations(); ++ai) {
                const Ogre::Animation* a = skel->getAnimation(ai);
                if (!a) continue;
                for (const auto& [handle, track] : a->_getNodeTrackList()) {
                    Q_UNUSED(handle);
                    if (track) totalKeysBefore += track->getNumKeyFrames();
                }
            }
            std::vector<std::string> names;
            names.reserve(skel->getNumAnimations());
            for (unsigned short ai = 0; ai < skel->getNumAnimations(); ++ai)
                names.push_back(skel->getAnimation(ai)->getName());
            for (const auto& n : names)
                totalRemoved += AnimationMerger::simplifyAnimation(skel.get(), n, tol);
        }
        s["applied"] = totalRemoved > 0;
        if (skels.isEmpty())
            s["summary"] = "no skeleton / animations to simplify";
        else if (totalRemoved <= 0)
            s["summary"] = "no additional simplification within configured tolerances";
        else
            s["summary"] = QString("removed %1 / %2 keyframes (%3%) across %4 skeleton(s)")
                               .arg(totalRemoved).arg(totalKeysBefore)
                               .arg(totalKeysBefore > 0
                                    ? QString::number(100.0 * totalRemoved / static_cast<double>(totalKeysBefore), 'f', 1)
                                    : QString("0.0"))
                               .arg(skels.size());
        QJsonObject d;
        d["removed"] = totalRemoved;
        d["totalKeyframesBefore"] = static_cast<qint64>(totalKeysBefore);
        d["skeletons"] = skels.size();
        s["details"] = d;
        stagesArr.append(s);
    }

    // Export the (possibly mutated) scene rooted at the first entity.
    Ogre::Entity* entity = entities.first();
    if (entity) entity->refreshAvailableAnimationState();
    const auto* node = entity ? entity->getParentSceneNode() : nullptr;
    if (!node) return makeErrorResult("Could not resolve scene node for export");

    static const QMap<QString, QString> formatByExt = {
        {QStringLiteral("fbx"),  QStringLiteral("FBX Binary (*.fbx)")},
        {QStringLiteral("fbxa"), QStringLiteral("FBX Binary (*.fbx)")},
        {QStringLiteral("gltf"), QStringLiteral("glTF 2.0 (*.gltf)")},
        {QStringLiteral("glb"),  QStringLiteral("glTF 2.0 Binary (*.glb)")},
        {QStringLiteral("dae"),  QStringLiteral("Collada (*.dae)")},
        {QStringLiteral("obj"),  QStringLiteral("OBJ (*.obj)")},
        {QStringLiteral("ply"),  QStringLiteral("PLY (*.ply)")},
        {QStringLiteral("stl"),  QStringLiteral("STL (*.stl)")},
        {QStringLiteral("mesh"), QStringLiteral("Ogre Mesh (*.mesh)")},
    };
    const QString ext = QFileInfo(outputPath).suffix().toLower();
    if (!formatByExt.contains(ext))
        return makeErrorResult(QString("Error: unsupported export format for .%1").arg(ext));
    if (MeshImporterExporter::exporter(node, outputPath, formatByExt.value(ext)) != 0)
        return makeErrorResult("Export failed");
    SentryReporter::addBreadcrumb("file.export",
        QString("Optimize -> %1").arg(QFileInfo(outputPath).fileName()));

    const qint64 srcBytes = QFileInfo(filePath).size();
    const qint64 outBytes = QFileInfo(outputPath).size();
    const qint64 delta = srcBytes - outBytes;
    QJsonObject result;
    result["content"] = QJsonArray{QJsonObject{
        {"type", "text"},
        {"text", QString("Optimized %1 -> %2 (%3 KB -> %4 KB, %5%)")
            .arg(QFileInfo(filePath).fileName())
            .arg(QFileInfo(outputPath).fileName())
            .arg(srcBytes / 1024)
            .arg(outBytes / 1024)
            .arg(srcBytes > 0
                ? QString::number(100.0 * static_cast<double>(delta) / static_cast<double>(srcBytes), 'f', 1)
                : QString("0.0"))}}};
    result["file"] = QFileInfo(filePath).fileName();
    result["output"] = QFileInfo(outputPath).fileName();
    result["inputBytes"]  = srcBytes;
    result["outputBytes"] = outBytes;
    result["bytesDelta"]  = delta;
    if (srcBytes > 0)
        result["bytesDeltaPct"] = 100.0 * static_cast<double>(delta) / static_cast<double>(srcBytes);
    result["stages"] = stagesArr;
    return result;
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(
            QString("Ogre error during optimize: %1")
                .arg(QString::fromStdString(e.getFullDescription())));
    } catch (const std::exception& e) {
        return makeErrorResult(
            QString("Error during optimize: %1").arg(QString::fromUtf8(e.what())));
    } catch (...) {
        return makeErrorResult("Unknown error during optimize");
    }
}

QJsonObject MCPServer::toolGenerateIsometricSprites(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "generate_isometric_sprites");

    const QString filePath = args.value("file").toString();
    const QString outputPath = args.value("output").toString();
    if (filePath.isEmpty() || outputPath.isEmpty())
        return makeErrorResult("Error: missing required 'file' and 'output' arguments");
    if (!QFileInfo::exists(filePath))
        return makeErrorResult(QString("Error: file not found: %1").arg(filePath));

    if (!Ogre::Root::getSingletonPtr() || !Ogre::Root::getSingletonPtr()->getRenderSystem())
        return makeErrorResult("Ogre render system not initialized");

    auto *mgr = Manager::getSingletonPtr();
    if (!mgr)
        return makeErrorResult("Manager unavailable");

    const QString animationName = args.value("animation").toString();
    int frameCount = 1;
    if (args.contains("frames"))
        frameCount = args.value("frames").toInt(1);
    else if (!animationName.isEmpty())
        frameCount = 8;

    IsometricOptions options;
    if (args.contains("resolution")) {
        const int res = args.value("resolution").toInt(512);
        if (res < 16 || res > 8192)
            return makeErrorResult("Error: resolution must be an integer in [16..8192]");
        options.width = res;
        options.height = res;
    }
    if (args.contains("width")) options.width = args.value("width").toInt(options.width);
    if (args.contains("height")) options.height = args.value("height").toInt(options.height);
    if (args.contains("elevation")) options.elevationDegrees = static_cast<float>(args.value("elevation").toDouble(30.0));
    if (args.contains("directions")) options.directionCount = args.value("directions").toInt(8);
    if (args.contains("start_azimuth")) options.startAzimuthDegrees = static_cast<float>(args.value("start_azimuth").toDouble(0.0));
    if (args.contains("camera_distance")) {
        const double dist = args.value("camera_distance").toDouble(0.0);
        if (dist <= 0.0)
            return makeErrorResult("Error: camera_distance must be a positive number");
        options.cameraDistance = static_cast<float>(dist);
    }
    if (args.contains("camera_padding") || args.contains("padding")) {
        const double pad = args.contains("camera_padding")
                               ? args.value("camera_padding").toDouble(1.25)
                               : args.value("padding").toDouble(1.25);
        if (pad <= 0.0)
            return makeErrorResult("Error: camera_padding must be a positive number");
        options.cameraPadding = static_cast<float>(pad);
    }

    SentryReporter::addBreadcrumb("file.import",
                                  QString("Isometric import %1").arg(QFileInfo(filePath).fileName()));

    TransientImportSession session(mgr);
    if (QString err = session.runImporter(QFileInfo(filePath).absoluteFilePath()); !err.isEmpty())
        return makeErrorResult(err);

    const QList<Ogre::Entity *> &imported = session.importedEntities();
    if (imported.isEmpty())
        return makeErrorResult(QString("Failed to load any entities from %1").arg(filePath));

    QList<Ogre::Entity *> entityList;
    for (Ogre::Entity *e : imported)
        if (e)
            entityList.append(e);

    Ogre::Entity *animatedEntity = nullptr;
    if (!animationName.isEmpty()) {
        animatedEntity = ModelIsometricRenderer::findEntityWithAnimation(entityList, animationName);
        if (!animatedEntity)
            return makeErrorResult(QString("Error: no skinned entity has animation '%1'").arg(animationName));
    }

    QList<QList<QImage>> grid;
    if (QString renderError;
        !ModelIsometricRenderer::renderToGrid(entityList, animatedEntity, animationName, frameCount, options,
                                              &grid, &renderError)) {
        ModelIsometricRenderer::shutdown();
        return makeErrorResult(QString("Isometric render failed: %1").arg(renderError));
    }

    const QImage sheet = ModelIsometricRenderer::composeDirectionGrid(grid);
    ModelIsometricRenderer::shutdown();
    if (sheet.isNull() || !sheet.save(outputPath))
        return makeErrorResult(QString("Failed to write isometric sprite sheet: %1").arg(outputPath));

    SentryReporter::addBreadcrumb("file.export", QFileInfo(outputPath).absoluteFilePath());

    const int dirs = static_cast<int>(grid.size());
    const int frames = dirs > 0 ? static_cast<int>(grid.first().size()) : 0;

    QJsonObject result = makeSuccessResult(
        QString("Wrote isometric sprite sheet (%1 directions × %2 frames): %3")
            .arg(dirs)
            .arg(frames)
            .arg(QFileInfo(outputPath).fileName()));
    result["output"] = QFileInfo(outputPath).absoluteFilePath();
    result["directions"] = dirs;
    result["frames"] = frames;
    result["cellWidth"] = options.width;
    result["cellHeight"] = options.height;
    if (options.width == options.height)
        result["resolution"] = options.width;
    result["sheetWidth"] = sheet.width();
    result["sheetHeight"] = sheet.height();
    result["elevation"] = options.elevationDegrees;
    result["startAzimuth"] = options.startAzimuthDegrees;
    if (options.cameraDistance > 0.0f)
        result["cameraDistance"] = options.cameraDistance;
    else
        result["cameraPadding"] = options.cameraPadding;
    result["directionOrder"] = ModelIsometricRenderer::directionOrderConvention();
    if (!animationName.isEmpty())
        result["animation"] = animationName;
    return result;
}

QJsonObject MCPServer::toolBakeVat(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "bake_vat");

    const QString filePath  = args.value("file").toString();
    const QString animName  = args.value("anim").toString();
    const QString outputDir = args.value("output_dir").toString();
    if (filePath.isEmpty() || animName.isEmpty() || outputDir.isEmpty())
        return makeErrorResult(
            "Error: missing required 'file', 'anim', or 'output_dir' arguments");
    if (!QFileInfo::exists(filePath))
        return makeErrorResult(QString("Error: file not found: %1").arg(filePath));

    const double fps = args.contains("fps") ? args.value("fps").toDouble() : 30.0;
    if (fps <= 0.0)
        return makeErrorResult("Error: fps must be > 0");

    const QString basename = args.value("basename").toString();

    auto* mgr = Manager::getSingleton();
    SentryReporter::addBreadcrumb("file.import",
        QString("Importing %1 for VAT bake").arg(filePath));
    TransientImportSession session(mgr);
    if (QString err = session.runImporter(filePath); !err.isEmpty())
        return makeErrorResult(err);
    const QList<Ogre::Entity*>& imported = session.importedEntities();
    if (imported.isEmpty())
        return makeErrorResult(QString("Error: failed to load mesh: %1").arg(filePath));

    // Pick the bake target by skeleton presence rather than list
    // order. Some mesh formats produce auxiliary unskinned entities
    // alongside the skinned mesh (e.g. helper geometry).
    Ogre::Entity* entity = nullptr;
    for (Ogre::Entity* e : imported) {
        if (e && e->hasSkeleton()) { entity = e; break; }
    }
    if (!entity)
        return makeErrorResult("Error: mesh has no skeleton — cannot bake VAT");

    VATBaker::Options opts;
    opts.animationName = animName;
    opts.fps           = fps;
    opts.outputDir     = outputDir;
    opts.basename      = basename.isEmpty() ? animName : basename;

    SentryReporter::addBreadcrumb("file.export",
        QString("Writing OpenVAT bake to %1 (anim=%2)").arg(outputDir, animName));

    VATBaker::BakeResult result = VATBaker::bake(entity, opts);
    if (!result.ok)
        return makeErrorResult(QString("VAT bake failed: %1").arg(result.error));

    QJsonObject content;
    content["ok"]          = true;
    content["texture"]     = result.posTexPath;
    content["sidecar"]     = result.jsonPath;
    content["frameCount"]  = result.frameCount;
    content["vertexCount"] = result.vertexCount;
    content["animation"]   = animName;
    content["fps"]         = fps;
    QJsonObject bounds, lo, hi;
    lo["x"] = result.minBound.x; lo["y"] = result.minBound.y; lo["z"] = result.minBound.z;
    hi["x"] = result.maxBound.x; hi["y"] = result.maxBound.y; hi["z"] = result.maxBound.z;
    bounds["min"] = lo; bounds["max"] = hi;
    content["bounds"] = bounds;

    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

// ---------------------------------------------------------------------------
// Morph A6 — morph-target inspection + weight poke.
// ---------------------------------------------------------------------------

QJsonObject MCPServer::toolListMorphTargets(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "list_morph_targets");

    const QString filePath = args.value("file").toString();
    if (filePath.isEmpty())
        return makeErrorResult("Error: missing required 'file' argument");
    if (!QFileInfo::exists(filePath))
        return makeErrorResult(QString("Error: file not found: %1").arg(filePath));

    // Transient import: load the file, read what we need, tear it
    // down on scope exit (with the user's prior selection restored).
    auto* mgr = Manager::getSingleton();
    SentryReporter::addBreadcrumb("file.import",
        QString("Importing %1 for morph list").arg(filePath));
    TransientImportSession session(mgr);
    if (QString err = session.runImporter(filePath); !err.isEmpty())
        return makeErrorResult(err);
    const QList<Ogre::Entity*>& imported = session.importedEntities();
    if (imported.isEmpty())
        return makeErrorResult(QString("Error: failed to load mesh: %1").arg(filePath));

    // Union targets across every imported entity (multi-entity files
    // can split shapes across body + head meshes). Same de-dup the
    // CLI subcommand does.
    QStringList targets;
    QSet<QString> seen;
    auto* mgrMorph = MorphAnimationManager::instance();
    for (Ogre::Entity* e : imported) {
        for (const QString& n : mgrMorph->morphTargetsFor(e)) {
            if (!seen.contains(n)) { seen.insert(n); targets.append(n); }
        }
    }

    QJsonObject content;
    content["file"] = filePath;
    content["count"] = static_cast<int>(targets.size());
    QJsonArray arr;
    for (const QString& n : targets) arr.append(n);
    content["morphTargets"] = arr;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolSetMorphWeight(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "set_morph_weight");

    const QString name = args.value("name").toString();
    if (name.isEmpty())
        return makeErrorResult("Error: missing required 'name' argument");
    if (!args.contains("weight"))
        return makeErrorResult("Error: missing required 'weight' argument");
    const double w = args.value("weight").toDouble();
    if (!std::isfinite(w))
        return makeErrorResult("Error: weight must be a finite number");

    auto* m = MorphAnimationManager::instance();
    if (!m->setWeightForSelection(name, w))
        return makeErrorResult(
            QString("Error: failed to set weight (no selection, "
                    "or '%1' not found on the selected entity)").arg(name));

    QJsonObject content;
    content["ok"] = true;
    content["name"] = name;
    content["weight"] = m->weightForSelection(name);
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

// ---------------------------------------------------------------------------
// Node-anim C6 — clip + keyframe authoring on the live scene.
// ---------------------------------------------------------------------------
// These tools operate on the LIVE scene (not a transient import) — same
// surface as `set_morph_weight`. The agent is expected to drive them
// in concert with `load_mesh` / `save_scene` if it wants persistence.

QJsonObject MCPServer::toolListNodeAnimations(const QJsonObject & /*args*/)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "list_node_animations");
    auto* m = NodeAnimationManager::instance();
    const QStringList clips = m ? m->listClips() : QStringList();

    QJsonArray arr;
    for (const QString& n : clips) arr.append(n);
    QJsonObject content;
    content["count"] = static_cast<int>(clips.size());
    content["clips"] = arr;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolAddNodeAnimationClip(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "add_node_animation_clip");

    const QString name = args.value("name").toString();
    if (name.isEmpty())
        return makeErrorResult("Error: missing required 'name' argument");
    if (!args.contains("length"))
        return makeErrorResult("Error: missing required 'length' argument");
    // Strict number check first — see set_node_keyframe rationale.
    const QJsonValue lengthV = args.value("length");
    if (!lengthV.isDouble())
        return makeErrorResult("Error: 'length' must be a number");
    const double length = lengthV.toDouble();
    if (!std::isfinite(length) || length <= 0.0)
        return makeErrorResult("Error: length must be a positive finite number");

    auto* m = NodeAnimationManager::instance();
    if (!m->createClip(name, length))
        return makeErrorResult(
            QString("Error: failed to create clip (name '%1' may already exist)").arg(name));

    QJsonObject content;
    content["ok"] = true;
    content["name"] = name;
    content["length"] = length;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

// Parse an optional N-element numeric JSON array argument. Returns
// `def` when the key is missing. Sets `ok = false` and populates
// `err` on wrong arity or any non-numeric element. Pulled out of
// `toolSetNodeKeyframe` so the dispatcher stays under SonarCloud's
// cognitive-complexity threshold and the inner lambdas don't exceed
// the 20-line cap.
template <int N>
static bool readNumericArray(const QJsonObject& args, const char* key,
                             double out[N], bool& present, QString& err)
{
    present = false;
    err.clear();
    if (!args.contains(key)) return true;
    present = true;
    const QJsonArray a = args.value(key).toArray();
    if (a.size() != N) {
        err = QStringLiteral("Error: '%1' must be an array of %2 numbers")
                  .arg(key).arg(N);
        return false;
    }
    for (int i = 0; i < N; ++i) {
        if (!a[i].isDouble()) {
            err = QStringLiteral("Error: '%1'[%2] must be a number")
                      .arg(key).arg(i);
            return false;
        }
        const double v = a[i].toDouble();
        // Extremely large literals (e.g. `1e400`) JSON-parse as Doubles
        // but produce Inf, which would silently propagate into the
        // keyframe. Reject NaN / Inf the same way time / length do.
        if (!std::isfinite(v)) {
            err = QStringLiteral("Error: '%1'[%2] must be a finite number")
                      .arg(key).arg(i);
            return false;
        }
        out[i] = v;
    }
    return true;
}

QJsonObject MCPServer::toolSetNodeKeyframe(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "set_node_keyframe");

    const QString clip = args.value("clip").toString();
    const QString node = args.value("node").toString();
    if (clip.isEmpty())
        return makeErrorResult("Error: missing required 'clip' argument");
    if (node.isEmpty())
        return makeErrorResult("Error: missing required 'node' argument");
    if (!args.contains("time"))
        return makeErrorResult("Error: missing required 'time' argument");
    // Strict number check first: QJsonValue::toDouble() silently
    // returns 0.0 on non-numeric inputs (string, bool, null), which
    // would create a phantom keyframe at t=0 from a caller bug like
    // `"time": "0.5"`. Reject any non-Double type up front.
    const QJsonValue timeV = args.value("time");
    if (!timeV.isDouble())
        return makeErrorResult("Error: 'time' must be a number");
    const double time = timeV.toDouble();
    if (!std::isfinite(time) || time < 0.0)
        return makeErrorResult("Error: time must be a non-negative finite number");

    // Translate / rotation / scale all default to identity / one when
    // omitted, so the agent can author "snapshot pose at time T" with
    // just the clip/node/time triple. Each is parsed strictly:
    // wrong-arity arrays AND non-numeric elements return an error.
    QString err;
    bool present = false;
    double t[3] = {0, 0, 0};
    double r[4] = {1, 0, 0, 0};
    double s[3] = {1, 1, 1};
    if (!readNumericArray<3>(args, "translate", t, present, err))
        return makeErrorResult(err);
    if (!readNumericArray<4>(args, "rotation", r, present, err))
        return makeErrorResult(err);
    if (!readNumericArray<3>(args, "scale", s, present, err))
        return makeErrorResult(err);
    const Ogre::Vector3 translate(static_cast<Ogre::Real>(t[0]),
                                  static_cast<Ogre::Real>(t[1]),
                                  static_cast<Ogre::Real>(t[2]));
    const Ogre::Quaternion rotation(static_cast<Ogre::Real>(r[0]),
                                    static_cast<Ogre::Real>(r[1]),
                                    static_cast<Ogre::Real>(r[2]),
                                    static_cast<Ogre::Real>(r[3]));
    const Ogre::Vector3 scale(static_cast<Ogre::Real>(s[0]),
                              static_cast<Ogre::Real>(s[1]),
                              static_cast<Ogre::Real>(s[2]));

    auto* m = NodeAnimationManager::instance();
    if (!m->addKeyframe(clip, node, time, translate, rotation, scale))
        return makeErrorResult(
            QString("Error: failed to set keyframe (clip '%1' missing, node '%2' missing, "
                    "or time %3 out of range)").arg(clip, node).arg(time, 0, 'f', 3));

    QJsonObject content;
    content["ok"] = true;
    content["clip"] = clip;
    content["node"] = node;
    content["time"] = time;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

// ---------------------------------------------------------------------------
// Pose-lib D-MCP — named bone-TRS snapshots on the live scene.
// ---------------------------------------------------------------------------
// All four tools operate on the first selected entity, same surface as
// `set_morph_weight` / `set_node_keyframe`. The agent is expected to
// drive `load_mesh` / `save_scene` around them for persistence — the
// `.poselib` sidecar arrives with D-Project.

QJsonObject MCPServer::toolListPoses(const QJsonObject & /*args*/)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "list_poses");
    auto* lib = PoseLibrary::instance();
    const QStringList poses = lib ? lib->listPosesForSelection() : QStringList();

    QJsonArray arr;
    for (const QString& n : poses) arr.append(n);
    QJsonObject content;
    content["count"] = static_cast<int>(poses.size());
    content["poses"] = arr;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolSavePose(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "save_pose");
    const QString name = args.value("name").toString();
    if (name.isEmpty())
        return makeErrorResult("Error: missing required 'name' argument");

    auto* lib = PoseLibrary::instance();
    if (!lib->savePoseForSelection(name))
        return makeErrorResult(
            QString("Error: failed to save pose '%1' (no selection or unskinned entity)")
                .arg(name));

    QJsonObject content;
    content["ok"] = true;
    content["name"] = name;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolApplyPose(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "apply_pose");
    const QString name = args.value("name").toString();
    if (name.isEmpty())
        return makeErrorResult("Error: missing required 'name' argument");

    auto* lib = PoseLibrary::instance();
    if (!lib->applyPoseForSelection(name))
        return makeErrorResult(
            QString("Error: failed to apply pose '%1' (no selection, unskinned entity, "
                    "or pose name not found in this entity's library)")
                .arg(name));

    QJsonObject content;
    content["ok"] = true;
    content["name"] = name;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolDeletePose(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "delete_pose");
    const QString name = args.value("name").toString();
    if (name.isEmpty())
        return makeErrorResult("Error: missing required 'name' argument");

    auto* lib = PoseLibrary::instance();
    if (!lib->deletePoseForSelection(name))
        return makeErrorResult(
            QString("Error: pose '%1' not found on the first selected entity")
                .arg(name));

    QJsonObject content;
    content["ok"] = true;
    content["name"] = name;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolMirrorPose(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "mirror_pose");
    const QString src = args.value("src").toString();
    const QString dst = args.value("dst").toString();
    if (src.isEmpty())
        return makeErrorResult("Error: missing required 'src' argument");
    if (dst.isEmpty())
        return makeErrorResult("Error: missing required 'dst' argument");

    auto* lib = PoseLibrary::instance();
    if (!lib->mirrorPoseForSelection(src, dst))
        return makeErrorResult(
            QString("Error: failed to mirror pose '%1' → '%2' "
                    "(no selection, no skeleton, or src not found)")
                .arg(src, dst));

    QJsonObject content;
    content["ok"] = true;
    content["src"] = src;
    content["dst"] = dst;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolSavePoseLibrary(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "save_pose_library");
    const QString path = args.value("path").toString();
    if (path.isEmpty())
        return makeErrorResult("Error: missing required 'path' argument");

    auto* lib = PoseLibrary::instance();
    if (!lib->savePoseLibraryForSelection(path))
        return makeErrorResult(
            QString("Error: failed to save library to '%1' "
                    "(no selection, empty library, or unwritable path)")
                .arg(path));

    QJsonObject content;
    content["ok"] = true;
    content["path"] = path;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolLoadPoseLibrary(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "load_pose_library");
    const QString path = args.value("path").toString();
    if (path.isEmpty())
        return makeErrorResult("Error: missing required 'path' argument");

    auto* lib = PoseLibrary::instance();
    if (!lib->loadPoseLibraryForSelection(path))
        return makeErrorResult(
            QString("Error: failed to load library from '%1' "
                    "(no selection, file missing, or malformed JSON/schema)")
                .arg(path));

    QJsonObject content;
    content["ok"] = true;
    content["path"] = path;
    content["count"] = static_cast<int>(lib->listPosesForSelection().size());
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolApplyPoseMasked(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "apply_pose_masked");
    const QString name = args.value("name").toString();
    if (name.isEmpty())
        return makeErrorResult("Error: missing required 'name' argument");
    if (!args.contains("bones"))
        return makeErrorResult("Error: missing required 'bones' argument");
    const QJsonValue bonesV = args.value("bones");
    if (!bonesV.isArray())
        return makeErrorResult("Error: 'bones' must be an array of bone-name strings");

    // Strict-parse: every entry must be a string. A non-string slot
    // would silently degrade to empty (toString returns "") and the
    // mask would match unintended bones.
    QStringList boneNames;
    const QJsonArray arr = bonesV.toArray();
    for (int i = 0; i < arr.size(); ++i) {
        if (!arr[i].isString())
            return makeErrorResult(
                QString("Error: 'bones'[%1] must be a string").arg(i));
        boneNames << arr[i].toString();
    }

    auto* lib = PoseLibrary::instance();
    if (!lib->applyPoseMaskedForSelection(name, boneNames))
        return makeErrorResult(
            QString("Error: failed to apply pose '%1' masked "
                    "(no selection, no skeleton, or pose not found)")
                .arg(name));

    QJsonObject content;
    content["ok"] = true;
    content["name"] = name;
    content["bone_count"] = boneNames.size();
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolCloudStatus(const QJsonObject & /*args*/)
{
    SentryReporter::addBreadcrumb(QStringLiteral("ai.tool_call"), QStringLiteral("cloud_status"));
    CloudCredentialStore::migrateLegacySettingsIfNeeded();
    const bool connected = CloudCredentialStore::hasSession();
    const CloudSession session = CloudCredentialStore::loadSession();

    QJsonObject content;
    content[QStringLiteral("connected")] = connected;
    if (connected && !session.email.isEmpty())
        content[QStringLiteral("email")] = session.email;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolCloudLogin(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb(QStringLiteral("ai.tool_call"), QStringLiteral("cloud_login"));
    const QString apiKey = args.value(QStringLiteral("api_key")).toString().trimmed();
    if (apiKey.isEmpty())
        return makeErrorResult(
            "Error: cloud_login requires an 'api_key' argument. "
            "Interactive device flow is only available via `qtmesh cloud login`.");

    CloudSession session;
    session.token = apiKey;
    if (!CloudCredentialStore::saveSession(session))
        return makeErrorResult("Error: could not persist API key securely.");

    QJsonObject content;
    content[QStringLiteral("ok")] = true;
    content[QStringLiteral("message")] = QStringLiteral("Saved API key to secure storage.");
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolCloudLogout(const QJsonObject & /*args*/)
{
    SentryReporter::addBreadcrumb(QStringLiteral("ai.tool_call"), QStringLiteral("cloud_logout"));
    CloudCredentialStore::migrateLegacySettingsIfNeeded();
    const QString token = CloudCredentialStore::loadSession().token;
    if (!token.isEmpty())
        QtMeshCloudClient::logout(token);
    CloudCredentialStore::clearSession();

    QJsonObject content;
    content[QStringLiteral("ok")] = true;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolCloudListProjects(const QJsonObject & /*args*/)
{
    SentryReporter::addBreadcrumb(QStringLiteral("ai.tool_call"), QStringLiteral("cloud_list_projects"));
    CloudCredentialStore::migrateLegacySettingsIfNeeded();
    const QString token = CloudCredentialStore::loadSession().token;
    if (token.isEmpty())
        return makeErrorResult("Error: not signed in. Use cloud_login first.");

    const auto result = QtMeshCloudClient::fetchAllProjects(token);
    if (!result.ok)
        return makeErrorResult(result.errorString);

    QJsonArray projects;
    for (const auto& project : result.projects) {
        QJsonObject obj;
        obj.insert(QStringLiteral("id"), project.id);
        obj.insert(QStringLiteral("name"), project.name);
        obj.insert(QStringLiteral("ownerSlug"), project.ownerSlug);
        obj.insert(QStringLiteral("projectSlug"), project.projectSlug);
        obj.insert(QStringLiteral("projectUrl"), project.projectUrl);
        projects.append(obj);
    }
    QJsonObject content;
    content.insert(QStringLiteral("projects"), projects);
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolCloudDeleteProject(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb(QStringLiteral("ai.tool_call"), QStringLiteral("cloud_delete_project"));
    const QString projectId = args.value(QStringLiteral("project_id")).toString().trimmed();
    if (projectId.isEmpty())
        return makeErrorResult("Error: missing required 'project_id' argument");

    CloudCredentialStore::migrateLegacySettingsIfNeeded();
    const QString token = CloudCredentialStore::loadSession().token;
    if (token.isEmpty())
        return makeErrorResult("Error: not signed in.");

    const auto result = QtMeshCloudClient::deleteProject(token, projectId);
    if (!result.ok)
        return makeErrorResult(result.errorString);

    QJsonObject content;
    content[QStringLiteral("ok")] = true;
    content[QStringLiteral("project_id")] = projectId;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolCloudUpload(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb(QStringLiteral("ai.tool_call"), QStringLiteral("cloud_upload"));
    const QString filePath = args.value(QStringLiteral("file")).toString();
    if (filePath.isEmpty())
        return makeErrorResult("Error: missing required 'file' argument");
    if (!QFileInfo::exists(filePath))
        return makeErrorResult(QString("Error: file not found: %1").arg(filePath));

    CloudCredentialStore::migrateLegacySettingsIfNeeded();
    const QString token = CloudCredentialStore::loadSession().token;
    if (token.isEmpty())
        return makeErrorResult("Error: not signed in.");

    QString projectName = args.value(QStringLiteral("name")).toString().trimmed();
    if (projectName.isEmpty())
        projectName = QFileInfo(filePath).completeBaseName();

    const bool runScan = !args.contains(QStringLiteral("scan")) || args.value(QStringLiteral("scan")).toBool(true);

    const QString mainCanonical = QFileInfo(filePath).canonicalFilePath();
    QStringList selectedPaths;
    selectedPaths.append(mainCanonical);
    for (const DependencyEntry& entry : DependencyResolver::detect(filePath)) {
        if (!entry.exists || !entry.checkedByDefault)
            continue;
        const QString absolute = QFileInfo(entry.absolutePath).absoluteFilePath();
        if (absolute == mainCanonical)
            continue;
        selectedPaths.append(absolute);
    }

    CloudPackageUploadRequest request;
    request.mainAssetPath = filePath;
    request.selectedAbsolutePaths = selectedPaths;
    request.projectName = projectName;
    request.createNewProject = true;
    request.runLocalScan = runScan;

    QtMeshCloudSession session(token);
    QEventLoop loop;
    QString projectUrl;
    QString error;
    QString reportWarning;
    bool uploadOk = false;
    const int uploadedFileCount = selectedPaths.size();
    connect(&session, &QtMeshCloudSession::uploadFinished, &loop,
            [&](bool ok, const QString& err, const QString& url, const QString&) {
                uploadOk = ok;
                projectUrl = url;
                if (ok && !err.isEmpty())
                    reportWarning = err;
                else if (!ok)
                    error = err.isEmpty() ? QStringLiteral("Upload failed") : err;
                loop.quit();
            });
    connect(&session, &QtMeshCloudSession::uploadCanceled, &loop, [&]() {
        uploadOk = false;
        error = QStringLiteral("Upload canceled");
        loop.quit();
    });
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(10 * 60 * 1000);
    connect(&timeout, &QTimer::timeout, &loop, [&]() {
        session.cancel();
        uploadOk = false;
        error = QStringLiteral("Upload timed out");
        loop.quit();
    });
    timeout.start();
    session.uploadPackageFromAssets(request);
    loop.exec();

    if (!uploadOk)
        return makeErrorResult(error);

    QJsonObject content;
    content[QStringLiteral("ok")] = true;
    content[QStringLiteral("projectUrl")] = projectUrl;
    content[QStringLiteral("fileCount")] = uploadedFileCount;
    if (!reportWarning.isEmpty())
        content[QStringLiteral("reportWarning")] = reportWarning;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonArray MCPServer::buildToolsList()
{
    QJsonArray tools;
    const auto appendTool = [this, &tools](const QString &name,
                                           const QString &description,
                                           const QJsonObject &properties,
                                           const QJsonArray &required = QJsonArray()) {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        inputSchema["properties"] = properties;
        if (!required.isEmpty()) {
            inputSchema["required"] = required;
        }
        tools.append(buildToolDefinition(name, description, inputSchema));
    };

    // create_material
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        QJsonObject properties;
        properties["name"]      = QJsonObject{{"type", "string"}, {"description", "Name of the new material"}};
        properties["script"]    = QJsonObject{{"type", "string"}, {"description", "Optional: full Ogre3D material script (overrides color params)"}};
        properties["ambient"]   = QJsonObject{{"type", "array"},  {"description", "Ambient color [R, G, B] (0.0-1.0)"}};
        properties["diffuse"]   = QJsonObject{{"type", "array"},  {"description", "Diffuse color [R, G, B] (0.0-1.0)"}};
        properties["specular"]  = QJsonObject{{"type", "array"},  {"description", "Specular color [R, G, B] (0.0-1.0)"}};
        properties["shininess"] = QJsonObject{{"type", "number"}, {"description", "Specular shininess (1-128)"}};
        properties["emissive"]  = QJsonObject{{"type", "array"},  {"description", "Emissive/glow color [R, G, B] (0.0-1.0)"}};
        inputSchema["properties"] = properties;
        inputSchema["required"] = QJsonArray{"name"};

        tools.append(buildToolDefinition(
            "create_material",
            "Create a new Ogre3D material with optional colors. Colors are [R,G,B] arrays (0.0-1.0). Apply the result to a mesh with apply_material.",
            inputSchema
        ));
    }

    // modify_material
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        QJsonObject properties;
        properties["name"] = QJsonObject{{"type", "string"}, {"description", "Name of the material to modify"}};
        properties["ambient"] = QJsonObject{{"type", "array"}, {"description", "Ambient color [R, G, B] (0.0-1.0)"}};
        properties["diffuse"] = QJsonObject{{"type", "array"}, {"description", "Diffuse color [R, G, B] (0.0-1.0)"}};
        properties["specular"] = QJsonObject{{"type", "array"}, {"description", "Specular color [R, G, B] (0.0-1.0)"}};
        properties["shininess"] = QJsonObject{{"type", "number"}, {"description", "Specular shininess (1-128)"}};
        properties["emissive"] = QJsonObject{{"type", "array"}, {"description", "Emissive color [R, G, B] (0.0-1.0)"}};
        properties["texture"] = QJsonObject{{"type", "string"}, {"description", "Texture filename to apply"}};
        inputSchema["properties"] = properties;
        inputSchema["required"] = QJsonArray{"name"};

        tools.append(buildToolDefinition(
            "modify_material",
            "Modify an existing material's properties. Can change ambient, diffuse, specular, emissive colors (as [R,G,B] arrays with 0.0-1.0 values), shininess (1-128), and texture. Use list_materials to find material names.",
            inputSchema
        ));
    }

    // get_material
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        QJsonObject properties;
        properties["name"] = QJsonObject{{"type", "string"}, {"description", "Name of the material to retrieve"}};
        inputSchema["properties"] = properties;
        inputSchema["required"] = QJsonArray{"name"};

        tools.append(buildToolDefinition(
            "get_material",
            "Get the full Ogre3D material script for a specific material. Returns the serialized material definition including all techniques, passes, and texture units.",
            inputSchema
        ));
    }

    // list_materials
    appendTool(
        "list_materials",
        "List all materials currently loaded in the Ogre3D resource system, including their names and resource groups.",
        QJsonObject());

    // apply_material
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        QJsonObject properties;
        properties["material"] = QJsonObject{{"type", "string"}, {"description", "Name of the material to apply"}};
        properties["mesh"] = QJsonObject{{"type", "string"}, {"description", "Name of the mesh to apply material to"}};
        inputSchema["properties"] = properties;
        inputSchema["required"] = QJsonArray{"material"};

        tools.append(buildToolDefinition(
            "apply_material",
            "Apply a material to a mesh entity in the scene. Use list_materials to find available material names and get_scene_info to find mesh/entity names.",
            inputSchema
        ));
    }

    // list_material_presets
    appendTool(
        "list_material_presets",
        "List the built-in material presets (Plastic / Metal / Wood / Glass / Unlit / Wireframe + PBR templates: Metallic-Roughness, Specular-Glossiness, Unlit PBR). Pass any returned name to apply_material_preset.",
        QJsonObject());

    // apply_material_preset
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        QJsonObject properties;
        properties["preset"] = QJsonObject{{"type", "string"}, {"description", "Preset name from list_material_presets (e.g. 'Metal (Gold)', 'Metallic-Roughness')."}};
        properties["mesh"] = QJsonObject{{"type", "string"}, {"description", "Optional mesh/entity name. When omitted, the preset applies to the current selection."}};
        inputSchema["properties"] = properties;
        inputSchema["required"] = QJsonArray{"preset"};
        tools.append(buildToolDefinition(
            "apply_material_preset",
            "Apply a built-in material preset to a mesh. PBR templates (Metallic-Roughness / Specular-Glossiness / Unlit PBR) create the canonical 6-slot texture-unit layout (albedo / normal_map / metallic / roughness / ao / emissive) and tag the pass with a 'pbr_workflow' user binding so PBR-aware shaders can detect intent.",
            inputSchema
        ));
    }

    // describe_material (#406)
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        QJsonObject properties;
        properties["prompt"] = QJsonObject{{"type", "string"},
            {"description", "Natural-language material description, e.g. "
                            "\"rusty bronze armor\" or \"glossy red plastic\"."}};
        properties["mesh"] = QJsonObject{{"type", "string"},
            {"description", "Optional mesh/entity name. When omitted, applies to "
                            "the current selection."}};
        properties["model"] = QJsonObject{{"type", "string"},
            {"description", "Optional GGUF model filename to use. Defaults to the "
                            "last-used / first available local model."}};
        properties["output_path"] = QJsonObject{{"type", "string"},
            {"description", "Optional path to re-export the mesh with the generated "
                            "material baked in. When omitted, the material is applied "
                            "to the in-session scene only."}};
        inputSchema["properties"] = properties;
        inputSchema["required"] = QJsonArray{"prompt"};
        tools.append(buildToolDefinition(
            "describe_material",
            "Generate a material from a natural-language description using the local "
            "LLM (llama.cpp), then bind it to the target/selected mesh (issue #406). "
            "Mirrors the Material Editor's 'Generate' prompt. Fails gracefully with a "
            "clear error when no local model is loaded or the build has no LLM support.",
            inputSchema
        ));
    }

    // load_mesh
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        QJsonObject properties;
        properties["path"] = QJsonObject{{"type", "string"}, {"description", "Absolute or relative path to the 3D file to import"}};
        inputSchema["properties"] = properties;
        inputSchema["required"] = QJsonArray{"path"};

        tools.append(buildToolDefinition(
            "load_mesh",
            "Import a 3D mesh file into the scene. Supports Ogre (.mesh, .mesh.xml), FBX, Collada (.dae), OBJ, glTF, STL, PLY, 3DS, DirectX (.x), and 40+ other formats via Assimp. Skeleton and animation data is preserved when available.",
            inputSchema
        ));
    }

    // get_mesh_info
    appendTool(
        "get_mesh_info",
        "Get detailed information about loaded meshes: vertex/index counts, submeshes, materials, bounding box, and skeleton data. Reports selected entities if any, otherwise all entities in the scene.",
        QJsonObject());

    // transform_mesh
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        QJsonObject properties;
        properties["name"]     = QJsonObject{{"type", "string"}, {"description", "Name of the scene node to transform (required). Use get_scene_info to find exact names."}};
        properties["position"] = QJsonObject{{"type", "array"}, {"description", "Position [X, Y, Z]"}};
        properties["rotation"] = QJsonObject{{"type", "array"}, {"description", "Rotation in degrees [X, Y, Z]"}};
        properties["scale"]    = QJsonObject{{"type", "array"}, {"description", "Scale [X, Y, Z]"}};
        inputSchema["properties"] = properties;
        inputSchema["required"] = QJsonArray{"name"};

        tools.append(buildToolDefinition(
            "transform_mesh",
            "Set the position, rotation, and/or scale of a named scene node. 'name' is required — use get_scene_info to find the exact node name. Position and scale are [X, Y, Z] arrays. Rotation is in degrees [X, Y, Z].",
            inputSchema
        ));
    }

    // transform_submesh
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        QJsonObject properties;
        properties["entity_name"]   = QJsonObject{{"type", "string"}, {"description", "Name of the entity containing the sub-mesh"}};
        properties["submesh_index"] = QJsonObject{{"type", "integer"}, {"description", "Zero-based index of the sub-mesh within the entity"}};
        properties["translate"]     = QJsonObject{{"type", "array"}, {"description", "Translation delta [X, Y, Z] applied to vertex positions"}};
        properties["rotate"]        = QJsonObject{{"type", "array"}, {"description", "Rotation in degrees [X, Y, Z] applied around sub-mesh centroid"}};
        properties["scale"]         = QJsonObject{{"type", "array"}, {"description", "Scale factor [X, Y, Z] applied around sub-mesh centroid"}};
        inputSchema["properties"] = properties;
        inputSchema["required"] = QJsonArray{"entity_name", "submesh_index"};

        tools.append(buildToolDefinition(
            "transform_submesh",
            "Transform vertices of a specific sub-mesh within an entity. Modifies the actual vertex buffer data (positions and normals), making changes exportable. Provide entity_name and submesh_index, plus any combination of translate, rotate, and scale.",
            inputSchema
        ));
    }

    // list_textures
    appendTool(
        "list_textures",
        "List all textures currently loaded in the Ogre3D texture manager, including their names. Use these names with set_texture to apply textures to materials.",
        QJsonObject());

    // set_texture
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        QJsonObject properties;
        properties["material"] = QJsonObject{{"type", "string"}, {"description", "Name of the material"}};
        properties["texture"] = QJsonObject{{"type", "string"}, {"description", "Texture filename"}};
        properties["unit"] = QJsonObject{{"type", "integer"}, {"description", "Texture unit index (default: 0)"}};
        inputSchema["properties"] = properties;
        inputSchema["required"] = QJsonArray{"material", "texture"};

        tools.append(buildToolDefinition(
            "set_texture",
            "Set a texture on a material's texture unit. The texture must be loaded (use list_textures to check). Optionally specify the texture unit index (default: 0).",
            inputSchema
        ));
    }

    // export_mesh
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        QJsonObject properties;
        properties["path"] = QJsonObject{{"type", "string"}, {"description", "Output file path for the exported mesh"}};
        properties["format"] = QJsonObject{{"type", "string"}, {"description",
            "Export format filter string. Valid values: "
            "'Ogre Mesh (*.mesh)', 'Ogre Mesh v1.10+(*.mesh)', 'Ogre Mesh v1.8+(*.mesh)', "
            "'Ogre Mesh v1.7+(*.mesh)', 'Ogre Mesh v1.4+(*.mesh)', 'Ogre Mesh v1.0+(*.mesh)', "
            "'Ogre XML (*.mesh.xml)', 'Collada (*.dae)', 'X (*.x)', 'OBJ (*.obj)', "
            "'OBJ without MTL (*.objnomtl)', 'STL (*.stl)', 'PLY (*.ply)', '3DS (*.3ds)', "
            "'glTF 2.0 (*.gltf2)', 'glTF 2.0 Binary (*.glb2)', 'Assimp Binary (*.assbin)', "
            "'FBX Binary (*.fbx)', 'PlayStation TMD (*.tmd)'. "
            "Default: 'Ogre Mesh (*.mesh)'"}};
        inputSchema["properties"] = properties;
        inputSchema["required"] = QJsonArray{"path"};

        tools.append(buildToolDefinition(
            "export_mesh",
            "Export the selected scene node's mesh to a file. A node must be selected first (use get_scene_info to list nodes). Skeleton and animation data is included automatically when present.",
            inputSchema
        ));
    }

    // export_pose
    {
        QJsonObject props;
        props["entity"] = QJsonObject{{"type", "string"}, {"description", "Name of the entity (optional, defaults to selected or first entity)"}};
        props["animation"] = QJsonObject{{"type", "string"}, {"description", "Name of the skeletal animation to pose"}};
        props["time"] = QJsonObject{{"type", "number"}, {"description", "Time position in seconds within the animation (default: 0.0)"}};
        props["output_path"] = QJsonObject{{"type", "string"}, {"description", "Output file path (e.g., 'posed.stl', 'posed.obj', 'posed.fbx')"}};
        appendTool(
            "export_pose",
            "Export the current animated pose of a skeletal entity as a static mesh (no skeleton, no animations). "
            "Scrub the animation to the desired time, then bake the deformed vertex positions into a new mesh file. "
            "Supports STL, OBJ, glTF, FBX, and other formats.",
            props,
            QJsonArray{"animation", "output_path"}
        );
    }

    // get_scene_info
    appendTool(
        "get_scene_info",
        "Get a summary of the current scene: all scene nodes (with names), entities (with materials), and material count. Use this to discover node/entity names for other tools.",
        QJsonObject());

    // take_screenshot
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        QJsonObject properties;
        properties["path"] = QJsonObject{{"type", "string"}, {"description", "Output file path (optional, uses temp if not specified)"}};
        inputSchema["properties"] = properties;

        tools.append(buildToolDefinition(
            "take_screenshot",
            "Capture a screenshot of the current 3D viewport. Optionally provide an output file path (PNG format). If no path is given, a temporary file is used. Returns the file path of the saved screenshot.",
            inputSchema
        ));
    }

    // create_primitive
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        QJsonObject properties;
        properties["type"] = QJsonObject{{"type", "string"}, {"description", "Type of primitive: sphere, cube, plane, cylinder, cone, torus, tube, capsule, icosphere, spring"}};
        properties["name"] = QJsonObject{{"type", "string"}, {"description", "Name for the primitive (auto-generated if not specified)"}};
        inputSchema["properties"] = properties;
        inputSchema["required"] = QJsonArray{"type"};

        tools.append(buildToolDefinition(
            "create_primitive",
            "Create a procedural 3D primitive and add it to the scene. Supported types: sphere, cube, plane, cylinder, cone, torus, tube, capsule, icosphere, spring. Optionally provide a name (auto-generated if omitted).",
            inputSchema
        ));
    }

    // animate
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        QJsonObject props;
        props["name"] = QJsonObject{{"type", "string"}, {"description", "Name of the scene node to animate"}};
        props["yaw"] = QJsonObject{{"type", "number"}, {"description", "Rotation speed around Y axis (degrees/sec)"}};
        props["pitch"] = QJsonObject{{"type", "number"}, {"description", "Rotation speed around X axis (degrees/sec)"}};
        props["roll"] = QJsonObject{{"type", "number"}, {"description", "Rotation speed around Z axis (degrees/sec)"}};
        props["stop"] = QJsonObject{{"type", "boolean"}, {"description", "Set true to stop animation on this node"}};
        inputSchema["properties"] = props;
        inputSchema["required"] = QJsonArray{"name"};

        tools.append(buildToolDefinition(
            "animate",
            "Start or stop continuous rotation animation on a scene node",
            inputSchema
        ));
    }

    // list_skeletal_animations
    {
        appendTool(
            "list_skeletal_animations",
            "List all skeletal animations across all entities in the scene. Returns entity names, animation names, durations, and number of tracks (bones). Use these names with get_animation_info, set_animation_time, and other animation tools.",
            QJsonObject()
        );
    }

    // get_animation_info
    {
        QJsonObject props;
        props["entity"] = QJsonObject{{"type", "string"}, {"description", "Name of the entity"}};
        props["animation"] = QJsonObject{{"type", "string"}, {"description", "Name of the animation"}};
        appendTool(
            "get_animation_info",
            "Get detailed animation info: length, tracks (bones), and all keyframes with transform data",
            props,
            QJsonArray{"entity", "animation"}
        );
    }

    // set_animation_length
    {
        QJsonObject props;
        props["entity"] = QJsonObject{{"type", "string"}, {"description", "Name of the entity"}};
        props["animation"] = QJsonObject{{"type", "string"}, {"description", "Name of the animation"}};
        props["length"] = QJsonObject{{"type", "number"}, {"description", "New animation length in seconds"}};
        appendTool(
            "set_animation_length",
            "Change the duration of a skeletal animation",
            props,
            QJsonArray{"entity", "animation", "length"}
        );
    }

    // set_animation_time
    {
        QJsonObject props;
        props["entity"] = QJsonObject{{"type", "string"}, {"description", "Name of the entity"}};
        props["animation"] = QJsonObject{{"type", "string"}, {"description", "Name of the animation"}};
        props["time"] = QJsonObject{{"type", "number"}, {"description", "Time position in seconds (use this OR navigate)"}};
        props["navigate"] = QJsonObject{{"type", "string"}, {"description", "Jump to keyframe: 'next', 'prev', 'first', or 'last' (requires 'track')"}};
        props["track"] = QJsonObject{{"type", "string"}, {"description", "Bone name for keyframe navigation"}};
        props["enabled"] = QJsonObject{{"type", "boolean"}, {"description", "Enable/disable the animation state (default: true)"}};
        props["loop"] = QJsonObject{{"type", "boolean"}, {"description", "Set loop mode"}};
        appendTool(
            "set_animation_time",
            "Set animation time position or navigate to prev/next/first/last keyframe",
            props,
            QJsonArray{"entity", "animation"}
        );
    }

    // add_keyframe
    {
        QJsonObject props;
        props["entity"] = QJsonObject{{"type", "string"}, {"description", "Name of the entity"}};
        props["animation"] = QJsonObject{{"type", "string"}, {"description", "Name of the animation"}};
        props["track"] = QJsonObject{{"type", "string"}, {"description", "Bone name for the track"}};
        props["time"] = QJsonObject{{"type", "number"}, {"description", "Keyframe time in seconds"}};
        props["translate"] = QJsonObject{{"type", "array"}, {"description", "Translation [x, y, z]"}};
        props["rotate"] = QJsonObject{{"type", "array"}, {"description", "Rotation quaternion [w, x, y, z]"}};
        props["scale"] = QJsonObject{{"type", "array"}, {"description", "Scale [x, y, z]"}};
        appendTool(
            "add_keyframe",
            "Add or update a keyframe on an animation track at a specific time with optional transform values",
            props,
            QJsonArray{"entity", "animation", "track", "time"}
        );
    }

    // remove_keyframe
    {
        QJsonObject props;
        props["entity"] = QJsonObject{{"type", "string"}, {"description", "Name of the entity"}};
        props["animation"] = QJsonObject{{"type", "string"}, {"description", "Name of the animation"}};
        props["track"] = QJsonObject{{"type", "string"}, {"description", "Bone name for the track"}};
        props["time"] = QJsonObject{{"type", "number"}, {"description", "Keyframe time in seconds to remove"}};
        appendTool(
            "remove_keyframe",
            "Remove a keyframe from an animation track at the specified time",
            props,
            QJsonArray{"entity", "animation", "track", "time"}
        );
    }

    // play_animation
    {
        QJsonObject props;
        props["entity"] = QJsonObject{{"type", "string"}, {"description", "Name of the entity"}};
        props["animation"] = QJsonObject{{"type", "string"}, {"description", "Name of the animation to play"}};
        props["play"] = QJsonObject{{"type", "boolean"}, {"description", "True to play, false to stop (default: true)"}};
        props["loop"] = QJsonObject{{"type", "boolean"}, {"description", "Loop the animation (default: true)"}};
        appendTool(
            "play_animation",
            "Play or pause a skeletal animation on an entity. When playing, the animation advances in real-time in the viewport. Use list_skeletal_animations to find entity and animation names.",
            props,
            QJsonArray{"entity", "animation"}
        );
    }

    // toggle_skeleton_debug
    {
        QJsonObject props;
        props["entity"] = QJsonObject{{"type", "string"}, {"description", "Name of the entity"}};
        props["show"] = QJsonObject{{"type", "boolean"}, {"description", "True to show, false to hide (toggles if omitted)"}};
        props["bones"] = QJsonObject{{"type", "boolean"}, {"description", "Show bone shapes (default: true)"}};
        props["axes"] = QJsonObject{{"type", "boolean"}, {"description", "Show bone axes (default: false)"}};
        props["names"] = QJsonObject{{"type", "boolean"}, {"description", "Show bone name labels (default: false)"}};
        appendTool(
            "toggle_skeleton_debug",
            "Show or hide skeleton bone visualization on an entity. Requires an entity with a skeleton. Optionally control bones, axes, and bone name labels independently.",
            props,
            QJsonArray{"entity"}
        );
    }

    // toggle_bone_weights
    {
        QJsonObject props;
        props["entity"] = QJsonObject{{"type", "string"}, {"description", "Name of the entity"}};
        props["show"] = QJsonObject{{"type", "boolean"}, {"description", "True to show, false to hide (toggles if omitted)"}};
        props["bone"] = QJsonObject{{"type", "string"}, {"description", "Bone name to highlight its weight influence"}};
        appendTool(
            "toggle_bone_weights",
            "Show or hide bone weight heat-map overlay on an entity. Colors range from blue (0) to red (1). Optionally select a specific bone to highlight its weight influence.",
            props,
            QJsonArray{"entity"}
        );
    }

    // toggle_normals
    {
        QJsonObject props;
        props["show"] = QJsonObject{{"type", "boolean"}, {"description", "True to show, false to hide. If omitted, toggles the current state."}};
        appendTool(
            "toggle_normals",
            "Show or hide vertex normal visualization on all entities in the scene. "
            "Normals are displayed as colored lines extending from each vertex, "
            "color-coded by direction (|X|=Red, |Y|=Green, |Z|=Blue). "
            "Normals follow skeletal animations in real-time.",
            props
        );
    }

    // toggle_mesh_info
    {
        QJsonObject props;
        props["show"] = QJsonObject{{"type", "boolean"}, {"description", "True to show, false to hide. If omitted, toggles the current state."}};
        appendTool(
            "toggle_mesh_info",
            "Show or hide the mesh info overlay on the active viewport. "
            "Displays statistics including vertex/triangle counts, submeshes, "
            "materials, bones, and animations. Shows stats for selected entities "
            "when a selection exists, otherwise shows aggregated scene stats.",
            props
        );
    }

    // merge_animations
    {
        QJsonObject props;
        props["base_entity"] = QJsonObject{{"type", "string"}, {"description", "Name of the base entity whose mesh receives all merged animations. If omitted, the first entity with a skeleton is used."}};
        appendTool(
            "merge_animations",
            "Merge skeletal animations from all loaded entities into a single base entity. All entities must have compatible skeletons (same bone names). "
            "Animations from non-base entities are prefixed with a slugified version of their scene node name. "
            "Load multiple mesh files first with load_mesh, then call this tool to combine all animations. "
            "Use list_skeletal_animations to see the result.",
            props
        );
    }

    // resample_animation
    {
        QJsonObject props;
        props["entity_name"] = QJsonObject{{"type", "string"}, {"description", "Name of the entity. If omitted, uses the first entity with a skeleton."}};
        props["animation_name"] = QJsonObject{{"type", "string"}, {"description", "Name of the animation to resample. If omitted, all animations are processed."}};
        props["target_keyframes"] = QJsonObject{{"type", "integer"}, {"description", "Resample to exactly N evenly-spaced keyframes (N >= 2). Mutually exclusive with decimate_step."}};
        props["decimate_step"] = QJsonObject{{"type", "integer"}, {"description", "Keep every Nth keyframe plus the last (N >= 2). Mutually exclusive with target_keyframes."}};
        appendTool(
            "resample_animation",
            "Resample or decimate animation keyframes. Use target_keyframes for uniform resampling (interpolated) "
            "or decimate_step to keep every Nth original keyframe. Reduces animation data size while preserving "
            "bone hierarchy. Use list_skeletal_animations to see the result.",
            props
        );
    }

    // simplify_animation
    {
        QJsonObject props;
        props["entity_name"] = QJsonObject{{"type", "string"}, {"description", "Name of the entity. If omitted, uses the first entity with a skeleton."}};
        props["animation_name"] = QJsonObject{{"type", "string"}, {"description", "Name of the animation to simplify. If omitted, all animations are processed."}};
        props["preset"] = QJsonObject{{"type", "string"}, {"description", "Tolerance preset: 'conservative' (~0.1mm/0.05°, default — destructive, so the safe choice), 'balanced' (~1mm/0.5°), or 'aggressive' (~1cm/1°). Higher tolerance removes more keys."}};
        props["tolerance"] = QJsonObject{{"type", "number"}, {"description", "Override translation tolerance in world units. Falls back to the preset value when omitted."}};
        props["rotation_tolerance_deg"] = QJsonObject{{"type", "number"}, {"description", "Override rotation tolerance in degrees. Falls back to the preset value when omitted."}};
        props["scale_tolerance"] = QJsonObject{{"type", "number"}, {"description", "Override scale tolerance (unitless). Falls back to the preset value when omitted."}};
        appendTool(
            "simplify_animation",
            "Remove redundant keyframes whose values are within tolerance of the lerp/slerp interpolation between their "
            "neighbors. Preserves first/last keys and any keyframe representing a sharp pose change. Tolerance-based, "
            "so it shrinks baked Mixamo-style clips dramatically while staying visually identical. Use 'analyze_animation' "
            "first to preview how many keys would be removed.",
            props
        );
    }

    // analyze_animation
    {
        QJsonObject props;
        props["entity_name"] = QJsonObject{{"type", "string"}, {"description", "Name of the entity. If omitted, uses the first entity with a skeleton."}};
        props["animation_name"] = QJsonObject{{"type", "string"}, {"description", "Name of the animation to analyze. If omitted, all animations are reported."}};
        props["preset"] = QJsonObject{{"type", "string"}, {"description", "Tolerance preset: 'conservative' (default — destructive, so the safe choice), 'balanced', or 'aggressive'."}};
        props["tolerance"] = QJsonObject{{"type", "number"}, {"description", "Override translation tolerance."}};
        props["rotation_tolerance_deg"] = QJsonObject{{"type", "number"}, {"description", "Override rotation tolerance in degrees."}};
        props["scale_tolerance"] = QJsonObject{{"type", "number"}, {"description", "Override scale tolerance."}};
        appendTool(
            "analyze_animation",
            "Report how many keyframes are redundant (would be removed by simplify_animation) under the given "
            "tolerances, without modifying the animation. Useful for previewing savings before committing.",
            props
        );
    }

    // bake_animation_fps
    {
        QJsonObject props;
        props["entity_name"] = QJsonObject{{"type", "string"}, {"description", "Name of the entity. If omitted, uses the first entity with a skeleton."}};
        props["animation_name"] = QJsonObject{{"type", "string"}, {"description", "Name of the animation to bake. If omitted, all animations on the skeleton are processed."}};
        props["fps"] = QJsonObject{{"type", "integer"}, {"description", "Target keyframes-per-second (10, 15, 30, 60 are typical). The animation is re-gridded to a uniform 1/fps spacing while preserving the existing curve shape via interpolation between original anchors."}};
        appendTool(
            "bake_animation_fps",
            "Re-grid every bone track in the animation to a uniform N FPS layout. Useful for export pipelines that "
            "require a fixed cadence (e.g. Mixamo's 30/60 FPS options) or for compressing dense mocap data down to "
            "a chosen rate. Densifies sparse tracks AND reduces dense ones — both directions converge to the target. "
            "Original keyframe values are linearly interpolated between bracketing original anchors to preserve curve "
            "shape; the clip's first and last keyframes are kept exactly so duration is unchanged.",
            props,
            QJsonArray{"fps"}
        );
    }

    // save_scene
    {
        QJsonObject props;
        props["file_path"] = QJsonObject{{"type", "string"}, {"description", "Absolute path to save the scene file (e.g. /tmp/scene.scene.glb). Use .scene.glb for binary glTF or .scene.gltf for text."}};
        appendTool(
            "save_scene",
            "Save the entire scene (all loaded meshes with positions, rotations, scales, materials, skeletons, and animations) to a glTF file. "
            "Use .scene.glb for binary glTF (recommended, embeds textures) or .scene.gltf for text format.",
            props,
            QJsonArray{"file_path"}
        );
    }

    // open_scene
    {
        QJsonObject props;
        props["file_path"] = QJsonObject{{"type", "string"}, {"description", "Absolute path to a scene file to open (*.scene.glb, *.scene.gltf, *.glb, *.gltf, *.vrm)"}};
        appendTool(
            "open_scene",
            "Open a scene file, replacing the current scene. Loads all meshes with their transforms, materials, skeletons, and animations. "
            "Reports what was loaded including entity names and animation counts.",
            props,
            QJsonArray{"file_path"}
        );
    }

    // validate_mesh
    {
        appendTool(
            "validate_mesh",
            "Validate the selected mesh for common issues: degenerate triangles (zero-area faces), "
            "non-finite UV coordinates (NaN/Inf), and extreme UV values outside ±10. "
            "Returns a list of issues tagged [OK], [WARN], or [ERROR]. "
            "Select a mesh first with load_mesh.",
            QJsonObject()
        );
    }

    // generate_lods
    {
        QJsonObject props;
        props["count"] = QJsonObject{{"type", "integer"}, {"description", "Number of LOD levels to generate (1–4, default 3)."}};
        props["reductions"] = QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "number"}}},
            {"description", "Optional array of reduction ratios per LOD level (0.0–1.0). E.g. [0.5, 0.25, 0.1]."}};
        props["algo"] = QJsonObject{{"type", "string"}, {"enum", QJsonArray{"ogre", "meshopt"}},
            {"description",
             "LOD backend. 'ogre' (default) uses Ogre's stock MeshLodGenerator. "
             "'meshopt' uses meshoptimizer's attribute-aware simplify — preserves UV "
             "seams + skin weights but typically gives a softer silhouette."}};
        appendTool(
            "generate_lods",
            "Generate LOD (Level of Detail) levels for the selected mesh, reducing polygon count at distance. "
            "Specify count (1–4), optional per-level reduction ratios, and optional algo backend. "
            "Select a mesh first with load_mesh.",
            props
        );
    }

    // generate_auto_lods
    {
        appendTool(
            "generate_auto_lods",
            "Automatically generate optimal LOD levels for the selected mesh using Ogre's built-in algorithm. "
            "LOD count and quality are chosen automatically based on mesh complexity. "
            "Select a mesh first with load_mesh.",
            QJsonObject()
        );
    }

    // remove_lods
    {
        appendTool(
            "remove_lods",
            "Remove all LOD levels from the selected mesh, reverting to the full-detail base mesh. "
            "Select a mesh first with load_mesh.",
            QJsonObject()
        );
    }

    // get_lod_info
    {
        appendTool(
            "get_lod_info",
            "Get LOD level information for the selected mesh: triangle count per LOD level. "
            "Shows the base mesh (LOD 0) and all reduced LOD levels. "
            "Select a mesh first with load_mesh.",
            QJsonObject()
        );
    }

    // get_memory_usage
    {
        QJsonObject props;
        props["budget"] = QJsonObject{{"type", "string"},
            {"description", "Optional memory budget (e.g. '50MB', '1GB'). When the report exceeds the budget the response flags 'overBudget' under the structured 'memory' field."}};
        appendTool(
            "get_memory_usage",
            "Report estimated GPU memory for every loaded mesh (vertex + index buffers) "
            "and VRAM for every resident texture. The response includes a human-readable summary "
            "in the standard content field and a structured 'memory' object with per-mesh, "
            "per-texture, totals, and optional budget fields for machine consumers. "
            "Use to spot heavy meshes/textures before exporting to a memory-constrained target.",
            props
        );
    }

    // analyze_draw_calls
    {
        appendTool(
            "analyze_draw_calls",
            "Estimate scene draw-call cost and surface merge opportunities. Groups every "
            "loaded entity by the materials its submeshes use, counts one draw call per "
            "SubEntity, and lists the materials shared by multiple entities (the merge "
            "candidates that would reduce draw-call count). The response includes a "
            "human-readable summary in 'content' and a structured 'drawCalls' object with "
            "totals, clusters, and ranked suggestions for machine consumers.",
            QJsonObject()
        );
    }

    // optimize_vertex_cache
    {
        QJsonObject props;
        props["rewrite"] = QJsonObject{{"type", "boolean"},
            {"description", "When true, rewrite each submesh's index buffer in place with Forsyth's optimal order. When false (default), only report ACMR — no mutation."}};
        appendTool(
            "optimize_vertex_cache",
            "Run Tom Forsyth's linear-time vertex-cache optimization on every loaded mesh. "
            "Reports per-submesh and weighted ACMR (Average Cache Miss Ratio) before / after. "
            "Pass `rewrite: true` to actually reorder the index buffers (analysis-only otherwise). "
            "The response includes a human-readable summary in 'content' and a structured "
            "'vertexCache' object with per-submesh ACMR plus totals for machine consumers.",
            props
        );
    }

    // auto_uv_unwrap
    {
        QJsonObject props;
        props["resolution"] = QJsonObject{{"type", "integer"},
            {"description", "Atlas resolution in texels (xatlas hint). Default 1024."}};
        props["padding"] = QJsonObject{{"type", "integer"},
            {"description", "Texels of padding around each chart. Default 4 (safe up to MIP level 2)."}};
        props["channel"] = QJsonObject{{"type", "integer"},
            {"description", "UV channel to write the unwrap into. Default 0 (overwrites UV0). "
                            "Pass 1 to keep UV0 and write a lightmap UV into UV1."}};
        props["preserve_original"] = QJsonObject{{"type", "boolean"},
            {"description", "When true (default), the original UV channel that's about to be "
                            "overwritten is preserved on UV{channel+1}."}};
        appendTool(
            "auto_uv_unwrap",
            "Auto UV-unwrap the selected entity via xatlas (the library Blender / Godot use). "
            "Splits vertices along chart seams and writes non-overlapping UVs. Skin weights "
            "survive the seam splits via xref remap. Response includes a structured 'unwrap' "
            "object with atlas size, chart count, vertex count before / after, and utilization.",
            props
        );
    }

    // compute_skin_weights
    {
        QJsonObject props;
        props["max_influences"] = QJsonObject{{"type", "integer"},
            {"description",
             "Max bones each vertex is influenced by (hardware skinning convention 4). "
             "Range [1, 8]. Default 4."}};
        props["falloff"] = QJsonObject{{"type", "number"},
            {"description",
             "Inverse-distance exponent. Higher = sharper bind (more like rigid). "
             "Range [0.5, 16]. Default 4.0."}};
        props["max_distance"] = QJsonObject{{"type", "number"},
            {"description",
             "Bones farther than this fraction of the mesh diagonal are excluded. "
             "Range [0, 10] (0 disables). Default 0.5."}};
        props["skip_unweighted"] = QJsonObject{{"type", "boolean"},
            {"description",
             "When true, bones with no existing vertex assignments are filtered out "
             "(useful for Mixamo helper bones). Default false."}};
        props["replace_existing"] = QJsonObject{{"type", "boolean"},
            {"description",
             "When true (default), overwrite existing bone assignments. When false, "
             "merge — keep existing weights and add new ones for unweighted vertices."}};
        appendTool(
            "compute_skin_weights",
            "Compute and apply skin weights for the currently selected mesh against "
            "its attached skeleton. Uses an inverse-distance heuristic (closest-point-"
            "on-bone smooth bind) — the same approach Maya / 3dsMax use as their "
            "default. The mesh must have a skeleton attached. Issue #402.",
            props
        );
    }

    // generate_mesh_texture — only advertised when Stable Diffusion is
    // compiled in; the handler hard-fails otherwise, so publishing it on
    // a non-SD build would imply a capability the server can't satisfy.
#ifdef ENABLE_STABLE_DIFFUSION
    {
        QJsonObject props;
        props["prompt"] = QJsonObject{{"type", "string"},
            {"description",
             "Text prompt for the texture. Note: conditioning covers the WHOLE mesh "
             "(the depth map is the full silhouette), so the prompt should describe the "
             "whole surface, e.g. 'character wearing blue denim jeans and a red shirt'."}};
        props["controlnet_strength"] = QJsonObject{{"type", "number"},
            {"description",
             "How strongly the mesh depth map steers generation, 0..1. Default 0.9. "
             "Lower = freer interpretation, higher = tighter shape adherence."}};
        props["width"]  = QJsonObject{{"type", "integer"},
            {"description", "Generation width (default 512)."}};
        props["height"] = QJsonObject{{"type", "integer"},
            {"description", "Generation height (default 512)."}};
        props["controlnet_path"] = QJsonObject{{"type", "string"},
            {"description",
             "Optional explicit path to a ControlNet depth model. If omitted, a "
             "depth ControlNet is auto-discovered in the SD models folder; if none "
             "is found, generation falls back to plain (unconditioned) txt2img."}};
        appendTool(
            "generate_mesh_texture",
            "Mesh-aware texture generation (issue #403). Renders the selected mesh's "
            "depth map and conditions sd.cpp on it via a ControlNet depth model so the "
            "generated texture follows the mesh shape, then applies the result to the "
            "active material's diffuse slot. Requires a loaded base SD model (SD 1.5 "
            "for the depth ControlNet) and a selected mesh. Asynchronous — returns "
            "immediately; the texture appears when generation finishes. The whole mesh "
            "is conditioned at once, so a single prompt covers the entire surface.",
            props,
            QJsonArray{"prompt"}
        );
    }
#endif // ENABLE_STABLE_DIFFUSION

    // generate_pbr_maps (#404) — only advertised when ONNX is compiled in.
#ifdef ENABLE_ONNX
    {
        QJsonObject props;
        props["albedo_path"] = QJsonObject{{"type", "string"},
            {"description",
             "Path to the source albedo/diffuse texture on disk. The generated "
             "maps are written next to it (<stem>_normal.png / _roughness.png / "
             "_height.png)."}};
        props["normal"]    = QJsonObject{{"type", "boolean"},
            {"description", "Generate the tangent-space normal map (default true)."}};
        props["roughness"] = QJsonObject{{"type", "boolean"},
            {"description", "Generate the roughness map (default true). Derived "
             "from albedo luminance — needs no model, works offline."}};
        props["height"]    = QJsonObject{{"type", "boolean"},
            {"description", "Generate the height map (default true)."}};
        props["tile_size"] = QJsonObject{{"type", "integer"},
            {"description", "Model input tile size (default 256). 0 = whole image."}};
        props["overwrite"] = QJsonObject{{"type", "boolean"},
            {"description", "Re-run even if cached output PNGs already exist."}};
        appendTool(
            "generate_pbr_maps",
            "AI PBR map synthesis (issue #404). Predicts normal + height maps "
            "from a single albedo texture via an ONNX UNet (downloaded on first "
            "use) and derives roughness from albedo luminance. Writes the maps "
            "next to the source albedo; if a mesh is selected, binds normal/"
            "roughness into the material's canonical PBR slots. Normal/height "
            "need the model (graceful error if unavailable); roughness works "
            "offline.",
            props,
            QJsonArray{"albedo_path"}
        );
    }
    // upscale_texture (#405)
    {
        QJsonObject props;
        props["texture_path"] = QJsonObject{{"type", "string"},
            {"description",
             "Path to the image to upscale. The result is written next to it as "
             "<stem>_upscaled.png."}};
        props["scale"] = QJsonObject{{"type", "integer"},
            {"description", "Upscale factor: 2 or 4 (default 4)."}};
        props["overwrite"] = QJsonObject{{"type", "boolean"},
            {"description", "Re-run even if a cached _upscaled.png already exists."}};
        appendTool(
            "upscale_texture",
            "AI texture super-resolution (issue #405). Upscales an image 2x or 4x "
            "with Real-ESRGAN via an ONNX model (downloaded on first use). Useful "
            "for low-res imported textures or AI-generated outputs. Fails "
            "gracefully when the model is unavailable/offline.",
            props,
            QJsonArray{"texture_path"}
        );
    }
#endif // ENABLE_ONNX

    // retopologize
    {
        QJsonObject props;
        props["target_faces"] = QJsonObject{{"type", "integer"},
            {"description",
             "Target face count. Triangle-pairing has a hard lower bound of ~50% of the "
             "input triangle count (every triangle paired). Set to -1 (default) to pair "
             "every viable candidate."}};
        props["max_angle_deg"] = QJsonObject{{"type", "number"},
            {"description",
             "Maximum angle (degrees) between two adjacent triangle normals for them to "
             "be considered for pairing. Lower = more curvature-preserving. Default 25."}};
        props["shape_tol_deg"] = QJsonObject{{"type", "number"},
            {"description",
             "Maximum deviation (degrees) of each interior quad angle from 90. Default 65."}};
        props["max_aspect_ratio"] = QJsonObject{{"type", "number"},
            {"description",
             "Maximum aspect ratio (longest edge / shortest edge) of an accepted quad. "
             "Default 6."}};
        appendTool(
            "retopologize",
            "Quad-dominant retopology of the selected mesh via triangle pairing. "
            "Walks every interior edge whose two adjacent faces are triangles and scores "
            "the merge by coplanarity + quad shape + aspect ratio; takes the best pairs "
            "greedily. Output faces are committed via the qtme.faces.<i> n-gon binding so "
            "FBX and glTF exporters round-trip the new quads. Issue #401.",
            props
        );
    }

    // decimate_mesh
    {
        QJsonObject props;
        props["reduction"] = QJsonObject{{"type", "number"},
            {"description", "Fraction of triangles to drop (0..0.95). Pass one of reduction / target_tris / target_verts."}};
        props["target_tris"] = QJsonObject{{"type", "integer"},
            {"description", "Reduce until total triangle count is approximately this value."}};
        props["target_verts"] = QJsonObject{{"type", "integer"},
            {"description", "Reduce until total vertex count is approximately this value."}};
        props["dry_run"] = QJsonObject{{"type", "boolean"},
            {"description", "When true, return a projected report without mutating the mesh."}};
        props["algo"] = QJsonObject{{"type", "string"}, {"enum", QJsonArray{"ogre", "meshopt"}},
            {"description",
             "Decimation backend. 'ogre' (default) uses Ogre's stock MeshLodGenerator. "
             "'meshopt' uses meshoptimizer's attribute-aware simplify — preserves UV "
             "seams + skin weights but typically gives a softer silhouette. Same option "
             "set as `generate_lods`."}};
        appendTool(
            "decimate_mesh",
            "Single-pass mesh decimation via edge-collapse. Reduces the base mesh in place "
            "(unlike generate_lods which builds a discrete LOD chain). Pass one of "
            "`reduction` (0..0.95), `target_tris`, or `target_verts`. Backend is "
            "selected via `algo` (default `ogre`). The response includes a human-readable "
            "summary in 'content' and a structured 'decimation' object with per-submesh "
            "and total triangle counts before / after.",
            props
        );
    }

    // delete_entity
    {
        QJsonObject props;
        props["name"] = QJsonObject{{"type", "string"}, {"description", "Name of the entity/node to delete from the scene"}};
        appendTool(
            "delete_entity",
            "Permanently delete an entity/node from the scene. Use get_scene_info to find node names. This cannot be undone.",
            props,
            QJsonArray{"name"}
        );
    }

    // duplicate_entity
    {
        QJsonObject props;
        props["name"] = QJsonObject{{"type", "string"}, {"description", "Name of the entity/node to duplicate. If omitted, duplicates the current selection."}};
        appendTool(
            "duplicate_entity",
            "Duplicate an entity/node in the scene, creating a clone with the same mesh, materials, and transform. The clone gets a '_copy' name suffix.",
            props
        );
    }

    // get_camera_info
    {
        appendTool(
            "get_camera_info",
            "Get the current camera position, direction, and orientation in the 3D viewport.",
            QJsonObject()
        );
    }

    // camera_control
    {
        QJsonObject props;
        props["position"] = QJsonObject{{"type", "array"}, {"description", "Set camera position [X, Y, Z]"}};
        props["target"] = QJsonObject{{"type", "array"}, {"description", "Set camera look-at target [X, Y, Z]"}};
        props["zoom"] = QJsonObject{{"type", "number"}, {"description", "Zoom by delta (positive = zoom in, negative = zoom out)"}};
        props["frame_selection"] = QJsonObject{{"type", "boolean"}, {"description", "Zoom to fit the currently selected objects in view (set to true)"}};
        appendTool(
            "camera_control",
            "Control the 3D viewport camera. Set position, look-at target, zoom, or frame the selection. "
            "Multiple actions can be combined in one call.",
            props
        );
    }

    // set_snap_settings
    {
        QJsonObject props;
        props["enabled"] = QJsonObject{{"type", "boolean"}, {"description", "Enable or disable persistent snapping. When enabled, transforms always snap. When disabled, hold Ctrl during drag to snap."}};
        props["grid_size"] = QJsonObject{{"type", "number"}, {"description", "Translation snap grid size. Presets: 0.1, 0.25, 0.5, 1.0, 2.0, 5.0"}};
        props["angle_step"] = QJsonObject{{"type", "number"}, {"description", "Rotation snap angle in degrees. Presets: 5, 15, 45, 90"}};
        props["scale_step"] = QJsonObject{{"type", "number"}, {"description", "Scale snap step size. Presets: 0.1, 0.25, 0.5"}};
        appendTool(
            "set_snap_settings",
            "Configure transform snapping. Snapping rounds translations to grid positions, rotations to angle increments, "
            "and scales to step sizes. Hold Ctrl during drag for temporary snap, or enable persistent snap.",
            props
        );
    }

    // get_snap_settings
    {
        appendTool(
            "get_snap_settings",
            "Get the current transform snap settings: enabled state, grid size, angle step, and scale step.",
            QJsonObject()
        );
    }

    // list_files
    {
        QJsonObject props;
        props["path"] = QJsonObject{{"type", "string"}, {"description", "Directory path to list (default: user home directory)"}};
        props["pattern"] = QJsonObject{{"type", "string"}, {"description", "Glob filter, e.g. '*.fbx' or '*.obj' (default: all files)"}};
        appendTool(
            "list_files",
            "List files and directories at a given path. Use to find mesh files, textures, or other assets on disk. "
            "Returns file names, sizes, and types (file/dir).",
            props
        );
    }

    // search_files
    {
        QJsonObject props;
        props["path"] = QJsonObject{{"type", "string"}, {"description", "Starting directory for search (default: user home)"}};
        props["query"] = QJsonObject{{"type", "string"}, {"description", "Glob pattern to match file names, e.g. '*.fbx', 'wood*', '*.obj'"}};
        props["max_depth"] = QJsonObject{{"type", "integer"}, {"description", "Max directory depth to recurse (default: 5, max: 10)"}};
        appendTool(
            "search_files",
            "Recursively search for files matching a glob pattern. Use to find mesh files, textures, or assets "
            "anywhere within a directory tree. Returns absolute paths with file sizes.",
            props,
            QJsonArray{"query"}
        );
    }

    // read_file
    {
        QJsonObject props;
        props["path"] = QJsonObject{{"type", "string"}, {"description", "Absolute path to the file to read"}};
        props["max_lines"] = QJsonObject{{"type", "integer"}, {"description", "Maximum lines to read (default: 100, max: 500)"}};
        appendTool(
            "read_file",
            "Read the contents of a text file. Useful for viewing material scripts, config files, or scene descriptions. "
            "Binary files (images, meshes) will be rejected. Max 500 lines.",
            props,
            QJsonArray{"path"}
        );
    }

    // group_nodes
    {
        QJsonObject props;
        props["names"] = QJsonObject{{"type", "array"}, {"description", "Array of scene node names to group. If omitted, groups the current selection."},
                                      {"items", QJsonObject{{"type", "string"}}}};
        appendTool(
            "group_nodes",
            "Group scene nodes under a new parent node. The group node is positioned at the centroid of the selected nodes. "
            "Transforming the group transforms all children. Requires at least 2 nodes.",
            props
        );
    }

    // ungroup_node
    {
        QJsonObject props;
        props["name"] = QJsonObject{{"type", "string"}, {"description", "Name of the group node to ungroup. If omitted, ungroups the current selection."}};
        appendTool(
            "ungroup_node",
            "Ungroup a group node: move its children to the group's parent and delete the empty group. "
            "Only works on group nodes (scene nodes with children and no attached meshes).",
            props
        );
    }

    // reparent_node
    {
        QJsonObject props;
        props["node_name"] = QJsonObject{{"type", "string"}, {"description", "Name of the scene node to reparent"}};
        props["new_parent_name"] = QJsonObject{{"type", "string"}, {"description", "Name of the new parent node, or 'root' for the root scene node. If omitted, reparents to root."}};
        QJsonArray required;
        required.append("node_name");
        appendTool(
            "reparent_node",
            "Reparent a scene node under a different parent in the scene hierarchy. "
            "Preserves the node's world-space transform by recalculating the local transform. "
            "Prevents invalid operations (reparenting into own subtree). Supports undo.",
            props,
            required
        );
    }

    // set_pivot_mode
    {
        QJsonObject props;
        props["mode"] = QJsonObject{{"type", "string"}, {"description", "Pivot mode: 'center' (bounding box center), 'bottom' (bottom of bounding box), or 'origin' (scene node position)"},
                                     {"enum", QJsonArray{"center", "bottom", "origin"}}};
        QJsonArray required;
        required.append("mode");
        appendTool(
            "set_pivot_mode",
            "Set the pivot point mode for rotation and scale operations. 'center' uses the bounding box center, "
            "'bottom' uses the bottom of the bounding box (floor level), 'origin' uses the scene node position.",
            props,
            required
        );
    }

    // get_pivot_mode
    {
        appendTool(
            "get_pivot_mode",
            "Get the current pivot point mode. Returns 'center', 'bottom', or 'origin'.",
            QJsonObject()
        );
    }

    // pack_textures (slice G)
    {
        QJsonObject props;
        props["red"] = QJsonObject{
            {"type", "string"},
            {"description", "Path to grayscale source for the R channel (sampled as Rec.601 luminance). Optional — leave empty to use 'red_constant' instead."}};
        props["green"] = QJsonObject{
            {"type", "string"},
            {"description", "Path to grayscale source for the G channel."}};
        props["blue"] = QJsonObject{
            {"type", "string"},
            {"description", "Path to grayscale source for the B channel."}};
        props["alpha"] = QJsonObject{
            {"type", "string"},
            {"description", "Path to grayscale source for the A channel."}};
        props["red_constant"] = QJsonObject{
            {"type", "number"},
            {"description", "Constant 0..1 to fill the R channel when no path is given."}};
        props["green_constant"] = QJsonObject{{"type", "number"}};
        props["blue_constant"]  = QJsonObject{{"type", "number"}};
        props["alpha_constant"] = QJsonObject{{"type", "number"}};
        props["invert_red"]   = QJsonObject{{"type", "boolean"},
            {"description", "Invert the R channel (1 - value). Useful for roughness ↔ glossiness conversion."}};
        props["invert_green"] = QJsonObject{{"type", "boolean"}};
        props["invert_blue"]  = QJsonObject{{"type", "boolean"}};
        props["invert_alpha"] = QJsonObject{{"type", "boolean"}};
        props["width"]  = QJsonObject{{"type", "integer"},
            {"description", "Optional output width. Defaults to the largest source width (256 if all channels are constants)."}};
        props["height"] = QJsonObject{{"type", "integer"}};
        props["include_alpha"] = QJsonObject{{"type", "boolean"},
            {"description", "Default true — output is RGBA8. Set false to write RGB888."}};
        props["output"] = QJsonObject{
            {"type", "string"},
            {"description", "Output file path. Extension determines format (PNG/TGA/JPG/BMP)."}};
        QJsonArray required;
        required.append("output");
        appendTool(
            "pack_textures",
            "Pack 1-4 grayscale source images into a single RGBA output texture. "
            "Useful for authoring channel-packed PBR maps (e.g. Unity ORM = AO+Roughness+Metallic, "
            "Unreal MR = Metallic+Roughness). Each output channel takes either a source image (sampled "
            "as luminance) or a constant 0..1 value. Smaller sources are bilinear-scaled to match the "
            "largest input. Returns the output dimensions on success.",
            props,
            required
        );
    }

    // generate_normal_map (slice H)
    {
        QJsonObject props;
        props["source"] = QJsonObject{
            {"type", "string"},
            {"description", "Path to the grayscale height/bump source image (PNG/TGA/JPG/BMP)."}};
        props["output"] = QJsonObject{
            {"type", "string"},
            {"description", "Output file path. Extension determines format (PNG/TGA/JPG/BMP)."}};
        props["strength"] = QJsonObject{
            {"type", "number"},
            {"description", "Sobel gradient multiplier — effective bump intensity. Default 2.0; clamped to 0..32."}};
        props["width"]  = QJsonObject{{"type", "integer"},
            {"description", "Optional output width. Defaults to the source width."}};
        props["height"] = QJsonObject{{"type", "integer"}};
        props["invert_r"] = QJsonObject{{"type", "boolean"},
            {"description", "Flip the red channel — rare; kept for pipeline parity."}};
        props["invert_g"] = QJsonObject{{"type", "boolean"},
            {"description", "Flip the green channel — switches OpenGL (+Y up, default) ↔ DirectX (+Y down)."}};
        props["directx"] = QJsonObject{{"type", "boolean"},
            {"description", "Alias for invert_g — DirectX convention output."}};
        QJsonArray required;
        required.append("source");
        required.append("output");
        appendTool(
            "generate_normal_map",
            "Generate a tangent-space normal map from a grayscale height/bump source via a 3x3 Sobel "
            "filter. Output is RGB8 with the OpenGL +Y-up convention by default; set invert_g (or "
            "directx) for DirectX +Y-down output. Returns the output dimensions on success.",
            props,
            required
        );
    }

    // pack_atlas (Phase 6 slice E)
    {
        QJsonObject props;
        props["inputs"] = QJsonObject{
            // Accept either a JSON array of strings or a single comma-separated
            // string. The handler at toolPackAtlas branches on the type.
            {"anyOf", QJsonArray{
                QJsonObject{
                    {"type", "array"},
                    {"items", QJsonObject{{"type", "string"}}}
                },
                QJsonObject{{"type", "string"}}
            }},
            {"description", "Paths to the input texture files (PNG/TGA/JPG/BMP). Pass either a JSON array of strings or a single comma-separated string."}};
        props["output"] = QJsonObject{
            {"type", "string"},
            {"description", "Output atlas image path. Extension determines format (PNG/TGA/JPG)."}};
        props["manifest"] = QJsonObject{
            {"type", "string"},
            {"description", "Optional path for the JSON manifest (per-tile UV remaps). Schema: { width, height, padding, tiles: [{ source, x, y, w, h, u0, v0, u1, v1 }] }."}};
        props["size"] = QJsonObject{
            {"type", "integer"},
            {"description", "Convenience: sets both width and height to a square atlas. Default 2048."}};
        props["width"]  = QJsonObject{{"type", "integer"}, {"description", "Atlas width in pixels. Defaults to 2048."}};
        props["height"] = QJsonObject{{"type", "integer"}, {"description", "Atlas height in pixels. Defaults to 2048."}};
        props["padding"] = QJsonObject{
            {"type", "integer"},
            {"description", "Padding in pixels around every tile so MIPs don't bleed across neighbours. Default 2."}};
        QJsonArray required;
        required.append("inputs");
        required.append("output");
        appendTool(
            "pack_atlas",
            "Pack N input textures into a single atlas image + JSON manifest of per-tile UV remaps. "
            "Uses shelf bin-packing (height-descending sort, deterministic). Useful for consolidating "
            "many small per-prop textures into one binding to reduce GPU draw-call count. Tiles are "
            "padded on every side (configurable) to prevent MIP bleed. Returns tile count + atlas "
            "dimensions on success.",
            props,
            required
        );
    }

    // apply_atlas (Phase 6 slice E2)
    {
        QJsonObject props;
        props["file"] = QJsonObject{
            {"type", "string"},
            {"description", "Source mesh file to apply the atlas to (FBX / glTF / glb / DAE / OBJ / PLY / STL / .mesh)."}};
        props["output"] = QJsonObject{
            {"type", "string"},
            {"description", "Output path. Extension determines format. Must differ from `file`."}};
        props["manifest"] = QJsonObject{
            {"type", "string"},
            {"description", "Path to the JSON manifest produced by pack_atlas."}};
        props["atlas"] = QJsonObject{
            {"type", "string"},
            {"description", "Path to the atlas image. Its filename is rebound onto every matched submesh's diffuse TUS."}};
        props["match"] = QJsonObject{
            {"type", "string"},
            {"enum", QJsonArray{"basename", "fullpath"}},
            {"description", "How to match the submesh's diffuse texture name against the manifest's 'source' fields. 'basename' (default) is robust to path differences; 'fullpath' is exact-match."}};
        props["no_clamp"] = QJsonObject{
            {"type", "boolean"},
            {"description", "When true, UVs outside [0..1] are left unchanged instead of being clamped before remapping. Default false (clamp on)."}};
        props["keep_extras"] = QJsonObject{
            {"type", "boolean"},
            {"description", "By default, normal/AO/emissive TUSes on affected materials are stripped because they sample UV0 — now diffuse-atlas-relative. Set true to keep them (only sensible when you've also atlased those channels to match)."}};
        QJsonArray required;
        required.append("file");
        required.append("output");
        required.append("manifest");
        required.append("atlas");
        appendTool(
            "apply_atlas",
            "Apply a previously-packed atlas to a mesh. Reads the manifest, scales+biases UV0 of "
            "every submesh whose diffuse texture matches a tile into the tile's sub-rect, and "
            "rebinds the diffuse TUS to the atlas image. Counterpart to pack_atlas — together they "
            "consolidate N per-prop bindings down to one. Returns a per-submesh rewritten/skipped "
            "report.",
            props,
            required
        );
    }

    // optimize_mesh (Phase 6 slice G)
    {
        QJsonObject props;
        props["file"] = QJsonObject{
            {"type", "string"},
            {"description", "Source mesh file (FBX / glTF / glb / DAE / OBJ / PLY / STL / .mesh)."}};
        props["output"] = QJsonObject{
            {"type", "string"},
            {"description", "Output path. Extension determines format. Must differ from `file`."}};
        props["vertex_cache"] = QJsonObject{
            {"type", "boolean"},
            {"description", "Run Forsyth vertex-cache reorder on every submesh. Default true."}};
        props["simplify_anim"] = QJsonObject{
            {"type", "boolean"},
            {"description", "Strip redundant animation keyframes via AnimationMerger::simplifyAnimation. Default true."}};
        props["all"] = QJsonObject{
            {"type", "boolean"},
            {"description", "Convenience: enable vertex_cache + simplify_anim together. Default false (defaults handle that)."}};
        props["reduction"] = QJsonObject{
            {"type", "number"},
            {"description", "Decimate by fraction (0..0.95). 0.5 = 50% triangle reduction. Mutually exclusive with target_tris / target_verts."}};
        props["target_tris"] = QJsonObject{
            {"type", "integer"},
            {"description", "Decimate to approximately this many triangles. Mutually exclusive with reduction / target_verts."}};
        props["target_verts"] = QJsonObject{
            {"type", "integer"},
            {"description", "Decimate to approximately this many vertices. Mutually exclusive with reduction / target_tris."}};
        props["simplify_translation_tol"] = QJsonObject{
            {"type", "number"},
            {"description", "Animation simplify translation tolerance in world units. Default 0.001 (~1mm on meter-scale rigs)."}};
        props["simplify_rotation_deg_tol"] = QJsonObject{
            {"type", "number"},
            {"description", "Animation simplify rotation tolerance in degrees. Default 0.5."}};
        props["simplify_scale_tol"] = QJsonObject{
            {"type", "number"},
            {"description", "Animation simplify scale tolerance (unitless multiplier delta). Default 0.001."}};
        QJsonArray required;
        required.append("file");
        required.append("output");
        appendTool(
            "optimize_mesh",
            "Batch-optimize a mesh asset end-to-end. Runs the slice C / C4 / D optimizations in "
            "sequence on the same loaded scene: vertex-cache reorder (Forsyth), single-pass decimation "
            "(if reduction / target_tris / target_verts is provided), and animation keyframe simplify. "
            "Writes the result to `output` (extension determines format). Returns a per-stage applied/"
            "summary report plus the input/output byte counts.",
            props,
            required
        );
    }

    // generate_isometric_sprites (#724)
    {
        QJsonObject props;
        props["file"] = QJsonObject{
            {"type", "string"},
            {"description", "Source mesh file (FBX / glTF / glb / DAE / OBJ / PLY / STL / .mesh)."}};
        props["output"] = QJsonObject{
            {"type", "string"},
            {"description", "Output PNG path for the directions × frames sprite atlas."}};
        props["animation"] = QJsonObject{
            {"type", "string"},
            {"description", "Optional animation name. When set, samples evenly spaced frames across the clip."}};
        props["frames"] = QJsonObject{
            {"type", "integer"},
            {"description", "Animation frame columns (default 8 when animation is set, else 1)."}};
        props["directions"] = QJsonObject{
            {"type", "integer"},
            {"description", "Compass direction rows (default 8)."}};
        props["elevation"] = QJsonObject{
            {"type", "number"},
            {"description", "Camera elevation in degrees above the orbit plane (default 30)."}};
        props["resolution"] = QJsonObject{
            {"type", "integer"},
            {"description", "Per-cell square resolution in pixels (sets width and height). Default 512. Range [16..8192]."}};
        props["width"] = QJsonObject{{"type", "integer"}, {"description", "Per-cell width in pixels (overrides resolution width). Default 512."}};
        props["height"] = QJsonObject{{"type", "integer"}, {"description", "Per-cell height in pixels (overrides resolution height). Default 512."}};
        props["start_azimuth"] = QJsonObject{
            {"type", "number"},
            {"description", "Rotate row 0 to align with your game's facing direction (degrees, default 0)."}};
        props["camera_distance"] = QJsonObject{
            {"type", "number"},
            {"description", "Fixed orbit distance in world units. Omit or 0 for auto-fit from bounds."}};
        props["camera_padding"] = QJsonObject{
            {"type", "number"},
            {"description", "Multiplier on auto-fit distance when camera_distance is unset (default 1.25)."}};
        QJsonArray required;
        required.append("file");
        required.append("output");
        appendTool(
            "generate_isometric_sprites",
            "Render an isometric / 8-direction animated sprite atlas from a mesh file. Rows are fixed "
            "compass directions (row 0 = front, clockwise from above); columns are evenly spaced "
            "animation frames. Static mesh when `animation` is omitted. Same renderer as "
            "`qtmesh isometric`. Returns output path, grid dimensions, and the direction-order convention.",
            props,
            required);
    }

    // cloud_status / cloud_login / cloud_logout / cloud_list_projects / cloud_delete_project / cloud_upload
    {
        appendTool(
            "cloud_status",
            "Return whether a QtMesh Cloud session is stored locally (never includes the token).",
            QJsonObject{},
            QJsonArray{});
        QJsonObject loginProps;
        loginProps["api_key"] = QJsonObject{
            {"type", "string"},
            {"description", "Bearer token / API key to store in the OS keychain. Device flow is CLI-only."}};
        appendTool(
            "cloud_login",
            "Store a QtMesh Cloud API key in secure local storage.",
            loginProps,
            QJsonArray{"api_key"});
        appendTool(
            "cloud_logout",
            "Sign out of QtMesh Cloud and clear the locally stored session.",
            QJsonObject{},
            QJsonArray{});
        appendTool(
            "cloud_list_projects",
            "List QtMesh Cloud projects for the signed-in account.",
            QJsonObject{},
            QJsonArray{});
        QJsonObject deleteProps;
        deleteProps["project_id"] = QJsonObject{
            {"type", "string"},
            {"description", "Cloud project id to delete."}};
        appendTool(
            "cloud_delete_project",
            "Delete a QtMesh Cloud project by id.",
            deleteProps,
            QJsonArray{"project_id"});
        QJsonObject uploadProps;
        uploadProps["file"] = QJsonObject{
            {"type", "string"},
            {"description", "Main asset file to package and upload (dependencies are auto-detected)."}};
        uploadProps["name"] = QJsonObject{
            {"type", "string"},
            {"description", "Optional cloud project display name (defaults to the file basename)."}};
        uploadProps["scan"] = QJsonObject{
            {"type", "boolean"},
            {"description", "Run a local asset scan before upload and attach the report. Default true."}};
        appendTool(
            "cloud_upload",
            "Package an asset plus detected dependencies and upload to QtMesh Cloud. "
            "Returns the project URL on success.",
            uploadProps,
            QJsonArray{"file"});
    }

    // list_morph_targets
    {
        QJsonObject props;
        props["file"] = QJsonObject{{"type", "string"}, {"description", "Path to the source mesh file."}};
        QJsonArray required;
        required.append("file");
        appendTool(
            "list_morph_targets",
            "List named morph targets / blend shapes on a mesh file. Loads the file, "
            "enumerates Ogre::Pose entries the importer produced (one per shape), and "
            "returns the de-duplicated names across all imported entities. The mesh is "
            "torn down before the tool returns, so the editor's live scene is unchanged.",
            props,
            required
        );
    }

    // set_morph_weight
    {
        QJsonObject props;
        props["name"]   = QJsonObject{{"type", "string"}, {"description", "Morph target name (use list_morph_targets to enumerate)."}};
        props["weight"] = QJsonObject{{"type", "number"}, {"description", "Weight in [0..1]. Values outside the range are clamped."}};
        QJsonArray required;
        required.append("name");
        required.append("weight");
        appendTool(
            "set_morph_weight",
            "Set a morph-target weight on the first selected entity in the live editor. "
            "Drives the matching Ogre::AnimationState so the mesh deforms in real time. "
            "Returns the actual weight after clamping. Returns an error if no entity is "
            "selected or the named target doesn't exist on the selection.",
            props,
            required
        );
    }

    // list_node_animations
    {
        QJsonObject props;
        appendTool(
            "list_node_animations",
            "List node-animation clips on the live scene. Returns the clip names "
            "in scene-creation order. Use add_node_animation_clip to create new clips "
            "and set_node_keyframe to populate them.",
            props
        );
    }

    // add_node_animation_clip
    {
        QJsonObject props;
        props["name"]   = QJsonObject{{"type", "string"}, {"description", "Unique clip name. Must not collide with any existing animation on the scene."}};
        props["length"] = QJsonObject{{"type", "number"}, {"description", "Clip duration in seconds. Must be > 0."}};
        QJsonArray required;
        required.append("name");
        required.append("length");
        appendTool(
            "add_node_animation_clip",
            "Create a new node-animation clip on the live scene. The clip is empty "
            "(no tracks) until set_node_keyframe writes one. Returns an error on "
            "name collision with an existing animation or non-positive length.",
            props,
            required
        );
    }

    // set_node_keyframe
    {
        QJsonObject props;
        props["clip"]      = QJsonObject{{"type", "string"}, {"description", "Clip name (use list_node_animations to enumerate)."}};
        props["node"]      = QJsonObject{{"type", "string"}, {"description", "SceneNode name to animate. Must exist on the live scene."}};
        props["time"]      = QJsonObject{{"type", "number"}, {"description", "Keyframe time in seconds, 0..clip length."}};
        props["translate"] = QJsonObject{{"type", "array"},  {"description", "Position [x, y, z]. Defaults to [0,0,0] when omitted."}};
        props["rotation"]  = QJsonObject{{"type", "array"},  {"description", "Rotation quaternion [w, x, y, z]. Defaults to identity when omitted."}};
        props["scale"]     = QJsonObject{{"type", "array"},  {"description", "Scale [x, y, z]. Defaults to [1,1,1] when omitted."}};
        QJsonArray required;
        required.append("clip");
        required.append("node");
        required.append("time");
        appendTool(
            "set_node_keyframe",
            "Write a transform keyframe on a (clip, node) track. If a keyframe within "
            "1ms of `time` already exists, it's updated in place — same idempotency "
            "behaviour as the Inspector. Returns an error on missing clip / missing "
            "node / out-of-range time / malformed TRS arrays.",
            props,
            required
        );
    }

    // list_poses
    {
        QJsonObject props;
        appendTool(
            "list_poses",
            "List saved bone-TRS poses on the first selected entity. Returns "
            "the names in insertion order. Use save_pose to capture and "
            "apply_pose to snap back.",
            props
        );
    }

    // save_pose
    {
        QJsonObject props;
        props["name"] = QJsonObject{{"type", "string"}, {"description", "Pose name. Overwrites any existing same-name pose in place."}};
        QJsonArray required;
        required.append("name");
        appendTool(
            "save_pose",
            "Capture the current bone-TRS state of the first selected entity as "
            "a named pose. Use cases: T-pose / A-pose / neutral reference frames, "
            "named facial expressions. Returns error when no entity is selected "
            "or the entity has no skeleton.",
            props,
            required
        );
    }

    // apply_pose
    {
        QJsonObject props;
        props["name"] = QJsonObject{{"type", "string"}, {"description", "Pose name (use list_poses to enumerate)."}};
        QJsonArray required;
        required.append("name");
        appendTool(
            "apply_pose",
            "Snap the first selected entity back to a saved pose — writes every "
            "captured bone TRS onto the skeleton instance. Bones present at save "
            "time but missing now are skipped silently (handles LOD changes). "
            "Snap-apply only; time-blended apply lands in D6.",
            props,
            required
        );
    }

    // delete_pose
    {
        QJsonObject props;
        props["name"] = QJsonObject{{"type", "string"}, {"description", "Pose name to remove."}};
        QJsonArray required;
        required.append("name");
        appendTool(
            "delete_pose",
            "Drop a saved pose from the first selected entity's library. "
            "Returns error when the pose name isn't found.",
            props,
            required
        );
    }

    // mirror_pose
    {
        QJsonObject props;
        props["src"] = QJsonObject{{"type", "string"}, {"description", "Existing pose name to mirror (use list_poses to enumerate)."}};
        props["dst"] = QJsonObject{{"type", "string"}, {"description", "Output pose name. **Overwrites any existing pose at this name**, including poses unrelated to `src` — pick a unique name (or list_poses first) to avoid silent clobber."}};
        QJsonArray required;
        required.append("src");
        required.append("dst");
        appendTool(
            "mirror_pose",
            "Mirror a saved pose across the YZ plane using the bone-name "
            "heuristic. Recognises `_l`/`_r`, `.L`/`.R`, and `Left*`/`Right*` "
            "naming. TRS flip: pos.x → -pos.x, rotation (w,x,y,z) → (w,x,-y,-z), "
            "scale.x → -scale.x. Centre-line bones (Spine, Hips, Head) get "
            "the X-flipped TRS in place. Writes the result under `dst`.",
            props,
            required
        );
    }

    // save_pose_library
    {
        QJsonObject props;
        props["path"] = QJsonObject{{"type", "string"}, {"description", "Destination `.poselib` file path. Written atomically via QSaveFile. Side-by-side with the asset is the recommended location."}};
        QJsonArray required;
        required.append("path");
        appendTool(
            "save_pose_library",
            "Persist the first selected entity's pose library to a "
            "`.poselib` sidecar JSON (schema `qtmesheditor.poselib.v1`). "
            "Returns error when there's no selection, the library is "
            "empty, or the path is unwritable.",
            props,
            required
        );
    }

    // load_pose_library
    {
        QJsonObject props;
        props["path"] = QJsonObject{{"type", "string"}, {"description", "Source `.poselib` file path. Schema-validated; malformed files don't disturb in-memory library."}};
        QJsonArray required;
        required.append("path");
        appendTool(
            "load_pose_library",
            "Load a `.poselib` sidecar JSON into the first selected "
            "entity's library, **replacing** the in-memory contents. "
            "Returns error when there's no selection, the file is "
            "missing, or the JSON / schema is malformed (in-memory "
            "library is preserved on parse failure).",
            props,
            required
        );
    }

    // apply_pose_masked
    {
        QJsonObject props;
        props["name"] = QJsonObject{{"type", "string"}, {"description", "Pose name (use list_poses to enumerate)."}};
        props["bones"] = QJsonObject{{"type", "array"},  {"description", "Bone names to apply. Bones NOT in this list keep their current TRS. Empty list = no-op (returns success but touches no bones)."}};
        QJsonArray required;
        required.append("name");
        required.append("bones");
        appendTool(
            "apply_pose_masked",
            "Apply a saved pose to ONLY the listed bones. Use case: "
            "snap to a facial expression without disturbing the "
            "current body pose, or apply an arm gesture without "
            "re-posing legs. Bones in the list but missing from the "
            "skeleton (LOD change) are skipped silently.",
            props,
            required
        );
    }

    // bake_vat
    {
        QJsonObject props;
        props["file"]       = QJsonObject{{"type", "string"}, {"description", "Path to the source mesh (any format the importer accepts: .mesh, .fbx, .gltf, etc.). Mesh must expose per-vertex normals."}};
        props["anim"]       = QJsonObject{{"type", "string"}, {"description", "Animation clip name to bake (use list_skeletal_animations to enumerate)."}};
        props["fps"]        = QJsonObject{{"type", "number"}, {"description", "Sample rate in frames per second. Default 30."}};
        props["output_dir"] = QJsonObject{{"type", "string"}, {"description", "Directory to write the OpenVAT texture + sidecar into. Created if missing."}};
        props["basename"]   = QJsonObject{{"type", "string"}, {"description", "Base filename (no extension) for the outputs. Defaults to `anim` when empty."}};
        QJsonArray required;
        required.append("file");
        required.append("anim");
        required.append("output_dir");
        appendTool(
            "bake_vat",
            "Bake a skeletal animation into a Vertex Animation Texture in OpenVAT format "
            "(sharpen3d/openvat). Output: a single 16-bit RGB PNG (height = 2 × frames; top half "
            "positions, bottom half normals) plus `<basename>-remap_info.json` with the canonical "
            "`os-remap` sidecar shape. Off-the-shelf openvat reference shaders for Godot / Unity / "
            "Unreal / Blender consume the output unmodified.",
            props,
            required
        );
    }

    return tools;
}

QJsonObject MCPServer::buildToolDefinition(const QString &name, const QString &description,
                                            const QJsonObject &inputSchema)
{
    QJsonObject tool;
    tool["name"] = name;
    tool["description"] = description;
    tool["inputSchema"] = inputSchema;
    return tool;
}

// HTTP REST API

bool MCPServer::startHttp(int port)
{
    m_httpPort = port;
    m_httpServer = new QTcpServer(this);
    connect(m_httpServer, &QTcpServer::newConnection, this, &MCPServer::onHttpConnection);

    if (m_httpServer->listen(QHostAddress::Any, m_httpPort)) {
        m_httpPort = m_httpServer->serverPort(); // update to actual port (important when port=0)
        qDebug() << "HTTP REST API listening on port" << m_httpPort;
        return true;
    } else {
        qWarning() << "Failed to start HTTP server on port" << m_httpPort
                    << ":" << m_httpServer->errorString();
        delete m_httpServer;
        m_httpServer = nullptr;
        return false;
    }
}

void MCPServer::onHttpConnection()
{
    while (m_httpServer->hasPendingConnections()) {
        QTcpSocket *socket = m_httpServer->nextPendingConnection();
        m_httpBuffers[socket].clear();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            handleHttpRequest(socket);
        });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
            m_httpBuffers.remove(socket);
            socket->deleteLater();
        });
    }
}

void MCPServer::handleHttpRequest(QTcpSocket *socket)
{
    m_httpBuffers[socket].append(socket->readAll());
    QByteArray &buf = m_httpBuffers[socket];

    // Check if we have the full headers (terminated by \r\n\r\n)
    int headerEnd = buf.indexOf("\r\n\r\n");
    if (headerEnd == -1)
        return; // wait for more data

    // Parse request line
    int firstLineEnd = buf.indexOf("\r\n");
    QString requestLine = QString::fromUtf8(buf.left(firstLineEnd));
    QStringList parts = requestLine.split(' ');
    if (parts.size() < 3) {
        socket->write("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n");
        socket->flush();
        socket->deleteLater();
        return;
    }

    QString method = parts[0];
    QString path = parts[1];

    // Parse Content-Length from headers
    int contentLength = 0;
    QString headers = QString::fromUtf8(buf.mid(firstLineEnd + 2, headerEnd - firstLineEnd - 2));
    for (const QString &line : headers.split("\r\n")) {
        if (line.startsWith("Content-Length:", Qt::CaseInsensitive)) {
            contentLength = line.mid(15).trimmed().toInt();
        }
    }

    int bodyStart = headerEnd + 4;
    if (buf.size() < bodyStart + contentLength)
        return; // wait for full body

    QByteArray body = buf.mid(bodyStart, contentLength);
    buf.clear();

    // Disconnect all signals from socket to prevent re-entrant calls.
    // Tool calls (e.g. create_primitive) trigger Ogre signals that spin the
    // Qt event loop, which can re-deliver socket events and crash.
    disconnect(socket, nullptr, this, nullptr);

    // Handle CORS preflight
    if (method == "OPTIONS") {
        QByteArray resp = "HTTP/1.1 204 No Content\r\n"
                          "Access-Control-Allow-Origin: *\r\n"
                          "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                          "Access-Control-Allow-Headers: Content-Type\r\n"
                          "Connection: close\r\n\r\n";
        socket->write(resp);
        socket->flush();
        socket->deleteLater();
        return;
    }

    // Parse route and arguments, then defer execution via QTimer::singleShot
    // so the socket event is fully processed before any tool runs.
    QString toolName;
    QJsonObject args;
    int httpStatus = 200;
    QJsonObject responseJson;

    // Route: GET /api/tools - list tools
    if (method == "GET" && path == "/api/tools") {
        QJsonObject toolsList = handleToolsList();
        responseJson["tools"] = toolsList["tools"];
    }
    // Route: POST /api/tools/:name - call a tool
    else if (method == "POST" && path.startsWith("/api/tools/")) {
        toolName = path.mid(11);
        int qmark = toolName.indexOf('?');
        if (qmark >= 0) toolName = toolName.left(qmark);

        if (!body.isEmpty()) {
            QJsonParseError parseError;
            QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
            if (parseError.error != QJsonParseError::NoError) {
                httpStatus = 400;
                responseJson["error"] = "Invalid JSON: " + parseError.errorString();
            } else {
                args = doc.object();
            }
        }
    }
    // Route: GET /api/tools/:name - call tool with no args
    else if (method == "GET" && path.startsWith("/api/tools/")) {
        toolName = path.mid(11);
        int qmark = toolName.indexOf('?');
        if (qmark >= 0) toolName = toolName.left(qmark);
    }
    else {
        httpStatus = 404;
        responseJson["error"] = "Not found. Use GET /api/tools or POST /api/tools/<name>";
    }

    // If we need to call a tool, defer it so socket events are fully drained first
    if (!toolName.isEmpty() && httpStatus == 200) {
        // Reject if another tool is already running (re-entrant call from Ogre event processing)
        if (m_httpBusy) {
            QByteArray errJson = R"({"error":"Server busy - another tool is executing"})";
            QByteArray resp;
            resp.append("HTTP/1.1 503 Service Unavailable\r\n");
            resp.append("Content-Type: application/json\r\n");
            resp.append("Access-Control-Allow-Origin: *\r\n");
            resp.append("Connection: close\r\n");
            resp.append(QString("Content-Length: %1\r\n").arg(errJson.size()).toUtf8());
            resp.append("\r\n");
            resp.append(errJson);
            socket->write(resp);
            socket->flush();
            socket->deleteLater();
            return;
        }

        QTimer::singleShot(0, this, [this, socket, toolName, args]() {
            m_httpBusy = true;
            QJsonObject result = callTool(toolName, args);
            m_httpBusy = false;

            QByteArray jsonData = QJsonDocument(result).toJson(QJsonDocument::Compact);
            QByteArray httpResponse;
            httpResponse.append("HTTP/1.1 200 OK\r\n");
            httpResponse.append("Content-Type: application/json\r\n");
            httpResponse.append("Access-Control-Allow-Origin: *\r\n");
            httpResponse.append("Connection: close\r\n");
            httpResponse.append(QString("Content-Length: %1\r\n").arg(jsonData.size()).toUtf8());
            httpResponse.append("\r\n");
            httpResponse.append(jsonData);

            socket->write(httpResponse);
            socket->flush();
            socket->deleteLater();
        });
        return;
    }

    // Send HTTP response immediately for non-tool requests
    QByteArray jsonData = QJsonDocument(responseJson).toJson(QJsonDocument::Compact);
    QByteArray httpResponse;
    httpResponse.append(QString("HTTP/1.1 %1 %2\r\n")
        .arg(httpStatus)
        .arg(httpStatus == 200 ? "OK" : (httpStatus == 400 ? "Bad Request" : "Not Found"))
        .toUtf8());
    httpResponse.append("Content-Type: application/json\r\n");
    httpResponse.append("Access-Control-Allow-Origin: *\r\n");
    httpResponse.append("Connection: close\r\n");
    httpResponse.append(QString("Content-Length: %1\r\n").arg(jsonData.size()).toUtf8());
    httpResponse.append("\r\n");
    httpResponse.append(jsonData);

    socket->write(httpResponse);
    socket->flush();
    socket->deleteLater();
}
