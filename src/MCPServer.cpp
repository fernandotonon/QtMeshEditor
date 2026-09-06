#include "MCPServer.h"
#include "mainwindow.h"
#include "GamificationManager.h"
#include "Manager.h"
#include "MaterialEditorQML.h"
#include "MaterialPresetLibrary.h"
#include "TextureChannelPacker.h"
#include "TextureAtlasPacker.h"
#include "ApplyAtlas.h"
#include "NormalMapGenerator.h"
#include "VATBaker.h"
#include "MorphAnimationManager.h"
#include "AlembicImporter.h"
#ifdef ENABLE_MOCAP
#include "Mocap/FaceCapMapper.h"
#include "Mocap/FaceCapPredictor.h"
#include "Mocap/MocapRecorder.h"
#include "Mocap/OneEuroFilter.h"
#include "Mocap/PoseCapPredictor.h"
#include "Mocap/PoseIKSolver.h"
#include "Mocap/VideoFrameSource.h"
#include "commands/RecordMocapClipCommand.h"
#include "Mocap/MocapController.h"
#endif
#include "NodeAnimationManager.h"
#include "PoseLibrary.h"
#include "PrimitiveObject.h"
#include "SelectionSet.h"
#include "TransformOperator.h"
#include "MeshImporterExporter.h"
#include "CLIPipeline.h"
#include "ImageTo3D/MeshGenPredictor.h"
#include "ImageTo3D/TripoSGPredictor.h"
#include "ImageTo3D/Trellis2Predictor.h"
#include "ImageTo3D/MeshGenBuilder.h"
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
#include "UvPipeline.h"
#include "UvProject.h"
#include "AssetScanController.h"
#include "CloudCredentialStore.h"
#include "CloudUploadPlanner.h"
#include "DependencyResolver.h"
#include "ProjectPackager.h"
#include "QtMeshCloudClient.h"
#include "QtMeshCloudSession.h"
#include "ScanConfig.h"
#include "ScanEngine.h"
#include "QuadRetopo.h"
#include "SkinWeights.h"
#include "SkinningDisplay.h"
#include "AutoRig.h"
#include "FaceRig/FaceRigAttach.h"
#include "MeshDepthRenderer.h"
#include "ModelIsometricRenderer.h"
#ifdef ENABLE_STABLE_DIFFUSION
#include "SDManager.h"
#endif
#ifdef ENABLE_ONNX
#include "AIAssistManager.h"
#include "PbrMapSynth.h"
#include "TextureUpscaler.h"
#endif
#include "HDR/HDREnvironmentManager.h"
#include "HDR/HdrEnvironmentController.h"
#include "HDR/HdrTonemap.h"
#include "LightManager.h"
#include "LightRigLibrary.h"
#include "SceneLightsIO.h"
#include "RTShaderHelper.h"
#include <QEventLoop>
#include <QElapsedTimer>
#include <QThread>
#ifdef ENABLE_PS1_RIP
#include "PS1/runtime/PS1RipManager.h"
#include "PS1/runtime/PS1RipWorker.h"
#include "PS1/runtime/PS1CapturedAssets.h"
#include "PS1/runtime/Ps1CoordinateNormalizer.h"
#include "PS1/runtime/Gp0CaptureStats.h"
#include "PS1/runtime/PsxVramMirrorMode.h"
#include <OgreEntity.h>
#include <OgreSubMesh.h>
#endif
#include <QDebug>
#include <QFile>
#include <QDir>
#include <QScopeGuard>
#include <QTemporaryFile>
#include <QImage>
#include <QBuffer>
#include <QColor>
#include "SentryReporter.h"
#include <QTimer>
#include <QDateTime>
#include <QMetaObject>
#include <QPixmap>
#include <QSet>
#include <QUuid>
#include <optional>
#include <OgreException.h>
#include <OgreLight.h>
#include <OgreTextureManager.h>
#include <OgreHardwarePixelBuffer.h>
#include <OgreRenderTexture.h>
#include <OgreViewport.h>
#include <OgreCamera.h>
#include "OgreRenderTargetUtil.h"
#include "SpaceCamera.h"
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
#include <limits>
#include <set>
#include <OgreSkeleton.h>
#include <OgreAnimation.h>
#include <OgreAnimationState.h>
#include <OgreKeyFrame.h>
#include <OgreBone.h>
#include "AnimationMerger.h"
#include "MotionLibrary.h"
#include "MotionGenerator.h"
#include "MotionInbetween.h"
#include "MeshSegmenter.h"
#include "AnimationControlController.h"
#include "SubMeshTransform.h"
#include "UndoManager.h"
#include "SubMeshOps.h"
#include "PartOpsMesh.h"
#include "commands/SplitMeshCommand.h"
#include "commands/ExplodePartsCommand.h"
#include "commands/SkeletonBoneCommands.h"
#include "commands/JoinPartsCommand.h"
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
static Ogre::Entity* firstResolvedSelectedEntity();
static bool mcpJsonIntValue(const QJsonValue& value, int* out, QString* err, const char* field);
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
        {QStringLiteral("set_hdr_environment"), &MCPServer::toolSetHdrEnvironment},
        {QStringLiteral("get_hdr_environment"), &MCPServer::toolGetHdrEnvironment},
        {QStringLiteral("set_tonemap"), &MCPServer::toolSetTonemap},
        {QStringLiteral("set_env_intensity"), &MCPServer::toolSetEnvIntensity},
        {QStringLiteral("set_env_tint"), &MCPServer::toolSetEnvTint},
        {QStringLiteral("describe_material"), &MCPServer::toolDescribeMaterial},
        {QStringLiteral("load_mesh"), &MCPServer::toolLoadMesh},
        {QStringLiteral("get_mesh_info"), &MCPServer::toolGetMeshInfo},
        {QStringLiteral("transform_mesh"), &MCPServer::toolTransformMesh},
        {QStringLiteral("transform_submesh"), &MCPServer::toolTransformSubMesh},
        {QStringLiteral("list_textures"), &MCPServer::toolListTextures},
        {QStringLiteral("set_texture"), &MCPServer::toolSetTexture},
        {QStringLiteral("export_mesh"), &MCPServer::toolExportMesh},
        {QStringLiteral("auto_uv_unwrap"),       &MCPServer::toolAutoUvUnwrap},
        {QStringLiteral("uv_info"),              &MCPServer::toolUvInfo},
        {QStringLiteral("uv_project"),           &MCPServer::toolUvProject},
        {QStringLiteral("uv_set_seams"),           &MCPServer::toolUvSetSeams},
        {QStringLiteral("uv_unwrap_selection"),   &MCPServer::toolUvUnwrapSelection},
        {QStringLiteral("retopologize"),         &MCPServer::toolRetopologize},
        {QStringLiteral("compute_skin_weights"), &MCPServer::toolComputeSkinWeights},
        {QStringLiteral("set_skinning_display"), &MCPServer::toolSetSkinningDisplay},
        {QStringLiteral("auto_rig"), &MCPServer::toolAutoRig},
        {QStringLiteral("remove_skeleton"), &MCPServer::toolRemoveSkeleton},
        {QStringLiteral("add_arkit_blendshapes"), &MCPServer::toolAddArkitBlendshapes},
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
        {QStringLiteral("motion_in_between"), &MCPServer::toolMotionInBetween},
        {QStringLiteral("generate_motion"), &MCPServer::toolGenerateMotion},
        {QStringLiteral("adjust_arm_space"), &MCPServer::toolAdjustArmSpace},
        {QStringLiteral("pin_feet"), &MCPServer::toolPinFeet},
        {QStringLiteral("segment_mesh"), &MCPServer::toolSegmentMesh},
        {QStringLiteral("split_mesh_by_segments"), &MCPServer::toolSplitMeshBySegments},
        {QStringLiteral("explode_mesh_parts"), &MCPServer::toolExplodeMeshParts},
        {QStringLiteral("join_mesh_parts"), &MCPServer::toolJoinMeshParts},
        {QStringLiteral("generate_mesh_from_image"), &MCPServer::toolGenerateMeshFromImage},
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
        {QStringLiteral("create_light"), &MCPServer::toolCreateLight},
        {QStringLiteral("delete_light"), &MCPServer::toolDeleteLight},
        {QStringLiteral("list_lights"), &MCPServer::toolListLights},
        {QStringLiteral("set_light_property"), &MCPServer::toolSetLightProperty},
        {QStringLiteral("apply_light_rig"), &MCPServer::toolApplyLightRig},
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
        {QStringLiteral("import_alembic"), &MCPServer::toolImportAlembic},
        {QStringLiteral("capture_face_from_video"), &MCPServer::toolCaptureFaceFromVideo},
        {QStringLiteral("capture_body_from_video"), &MCPServer::toolCaptureBodyFromVideo},
        {QStringLiteral("list_capture_devices"), &MCPServer::toolListCaptureDevices},
        {QStringLiteral("start_live_capture"), &MCPServer::toolStartLiveCapture},
        {QStringLiteral("stop_live_capture"), &MCPServer::toolStopLiveCapture},
        {QStringLiteral("set_capture_channels"), &MCPServer::toolSetCaptureChannels},
        {QStringLiteral("play_vertex_animation"), &MCPServer::toolPlayVertexAnimation},
        {QStringLiteral("list_node_animations"), &MCPServer::toolListNodeAnimations},
        {QStringLiteral("add_node_animation_clip"), &MCPServer::toolAddNodeAnimationClip},
        {QStringLiteral("set_node_keyframe"), &MCPServer::toolSetNodeKeyframe},
        {QStringLiteral("set_node_animation_playing"), &MCPServer::toolSetNodeAnimationPlaying},
        {QStringLiteral("delete_node_animation_clip"), &MCPServer::toolDeleteNodeAnimationClip},
        {QStringLiteral("move_node_keyframe"), &MCPServer::toolMoveNodeKeyframe},
        {QStringLiteral("delete_node_keyframe"), &MCPServer::toolDeleteNodeKeyframe},
        {QStringLiteral("get_node_animation"), &MCPServer::toolGetNodeAnimation},
        {QStringLiteral("set_playback_speed"), &MCPServer::toolSetPlaybackSpeed},
        {QStringLiteral("set_loop_region"), &MCPServer::toolSetLoopRegion},
        {QStringLiteral("get_playback_state"), &MCPServer::toolGetPlaybackState},
        {QStringLiteral("select_animation"), &MCPServer::toolSelectAnimation},
        {QStringLiteral("select_bone"), &MCPServer::toolSelectBone},
        {QStringLiteral("set_morph_weight_keyframe"), &MCPServer::toolSetMorphWeightKeyframe},
        {QStringLiteral("clear_morph_weight_keyframe"), &MCPServer::toolClearMorphWeightKeyframe},
        {QStringLiteral("set_keyframe_value"), &MCPServer::toolSetKeyframeValue},
        {QStringLiteral("move_bone_keyframe"), &MCPServer::toolMoveBoneKeyframe},
        {QStringLiteral("step_keyframe"), &MCPServer::toolStepKeyframe},
        {QStringLiteral("get_channel_values"), &MCPServer::toolGetChannelValues},
        {QStringLiteral("list_poses"), &MCPServer::toolListPoses},
        {QStringLiteral("save_pose"), &MCPServer::toolSavePose},
        {QStringLiteral("apply_pose"), &MCPServer::toolApplyPose},
        {QStringLiteral("delete_pose"), &MCPServer::toolDeletePose},
        {QStringLiteral("mirror_pose"), &MCPServer::toolMirrorPose},
        {QStringLiteral("save_pose_library"), &MCPServer::toolSavePoseLibrary},
        {QStringLiteral("load_pose_library"), &MCPServer::toolLoadPoseLibrary},
        {QStringLiteral("apply_pose_masked"), &MCPServer::toolApplyPoseMasked},
        {QStringLiteral("cloud_status"), &MCPServer::toolCloudStatus},
        {QStringLiteral("cloud_limits"), &MCPServer::toolCloudLimits},
        {QStringLiteral("cloud_login"), &MCPServer::toolCloudLogin},
        {QStringLiteral("cloud_logout"), &MCPServer::toolCloudLogout},
        {QStringLiteral("cloud_list_projects"), &MCPServer::toolCloudListProjects},
        {QStringLiteral("cloud_delete_project"), &MCPServer::toolCloudDeleteProject},
        {QStringLiteral("cloud_upload"), &MCPServer::toolCloudUpload},
        {QStringLiteral("ps1rip_start"), &MCPServer::toolPs1RipStart},
        {QStringLiteral("ps1rip_stop"), &MCPServer::toolPs1RipStop},
        {QStringLiteral("ps1rip_status"), &MCPServer::toolPs1RipStatus},
        {QStringLiteral("ps1rip_run_frames"), &MCPServer::toolPs1RipRunFrames},
        {QStringLiteral("ps1rip_capture"), &MCPServer::toolPs1RipCapture},
        {QStringLiteral("ps1rip_stats"), &MCPServer::toolPs1RipStats},
        {QStringLiteral("ps1rip_clear"), &MCPServer::toolPs1RipClear}
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
        QStringLiteral("motion_in_between"),
        QStringLiteral("generate_motion"),
        QStringLiteral("segment_mesh"),
        QStringLiteral("split_mesh_by_segments"),
        QStringLiteral("explode_mesh_parts"),
        QStringLiteral("join_mesh_parts"),
        QStringLiteral("add_arkit_blendshapes"),
        QStringLiteral("generate_mesh_from_image"),
        QStringLiteral("save_scene"),
        QStringLiteral("open_scene"),
        QStringLiteral("bake_vat"),
        QStringLiteral("list_morph_targets"),
        QStringLiteral("import_alembic"),
        QStringLiteral("capture_face_from_video"),
        QStringLiteral("capture_body_from_video"),
        QStringLiteral("cloud_upload")
    };
    return heavyTools.contains(name);
}

QJsonObject MCPServer::callTool(const QString &name, const QJsonObject &args)
{
    qDebug() << "MCP Tool Call:" << name;

    const QString invocationId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QElapsedTimer telemetryTimer;
    telemetryTimer.start();
    SentryReporter::captureInvocationEvent(QStringLiteral("mcp"), name, QStringLiteral("started"),
        -1, false, QString(), invocationId);
    SentryReporter::addBreadcrumb("ai.tool_call", QStringLiteral("Tool call: %1").arg(name));

    uintptr_t txn = 0;
    if (isHeavyTool(name)) {
        txn = SentryReporter::startTransaction(QStringLiteral("mcp.%1").arg(name), "mcp.tool");
    }

    // Lazily initialize Ogre/Manager on first scene-dependent tool call
    if (!name.startsWith(QStringLiteral("cloud_")) && !ensureOgreInitialized()) {
        if (txn) SentryReporter::finishTransaction(txn);
        SentryReporter::captureInvocationEvent(QStringLiteral("mcp"), name, QStringLiteral("failed"),
            telemetryTimer.elapsed(), false, QStringLiteral("renderer"), invocationId);
        return makeErrorResult("Error: Ogre 3D engine could not be initialized (no OpenGL available)");
    }

    const auto& handlers = toolHandlers();
    const auto handlerIt = handlers.constFind(name);
    if (handlerIt == handlers.constEnd()) {
        if (txn) SentryReporter::finishTransaction(txn);
        SentryReporter::captureInvocationEvent(QStringLiteral("mcp"), name, QStringLiteral("failed"),
            telemetryTimer.elapsed(), false, QStringLiteral("unknown_tool"), invocationId);
        return makeErrorResult(QString("Unknown tool: %1").arg(name));
    }

    QJsonObject toolResult = (this->*(handlerIt.value()))(args);

    // Track errors as breadcrumbs
    if (toolResult.contains("isError") && toolResult["isError"].toBool()) {
        SentryReporter::addBreadcrumb("ai.tool_call",
            QStringLiteral("Tool error: %1").arg(name), "error");
    } else {
        // Gamification discovery (#798): first successful MCP tool call marks
        // the mcp_server cluster; tools with a mapped editor cluster also
        // count toward that cluster's discovery (deduped per session).
        GamificationManager::noteFeature(QStringLiteral("mcp_server"),
                                         GamificationManager::Surface::Mcp);
        static const QHash<QString, QString> toolFeatureMap = {
            {QStringLiteral("retopologize"), QStringLiteral("retopo")},
            {QStringLiteral("decimate_mesh"), QStringLiteral("decimate_lod")},
            {QStringLiteral("generate_lods"), QStringLiteral("decimate_lod")},
            {QStringLiteral("optimize_mesh"), QStringLiteral("decimate_lod")},
            {QStringLiteral("auto_uv_unwrap"), QStringLiteral("uv_unwrap")},
            {QStringLiteral("uv_unwrap_selection"), QStringLiteral("uv_unwrap")},
            {QStringLiteral("uv_project"), QStringLiteral("uv_unwrap")},
            {QStringLiteral("uv_set_seams"), QStringLiteral("uv_unwrap")},
            {QStringLiteral("compute_skin_weights"), QStringLiteral("skin_weights")},
            {QStringLiteral("auto_rig"), QStringLiteral("auto_rig")},
            {QStringLiteral("add_arkit_blendshapes"), QStringLiteral("auto_rig")},
            {QStringLiteral("motion_in_between"), QStringLiteral("motion_inbetween")},
            {QStringLiteral("generate_motion"), QStringLiteral("animation_blend")},
            {QStringLiteral("merge_animations"), QStringLiteral("animation_blend")},
            {QStringLiteral("segment_mesh"), QStringLiteral("ai_assist")},
            {QStringLiteral("split_mesh_by_segments"), QStringLiteral("ai_assist")},
            {QStringLiteral("explode_mesh_parts"), QStringLiteral("ai_assist")},
            {QStringLiteral("join_mesh_parts"), QStringLiteral("ai_assist")},
            {QStringLiteral("capture_face_from_video"), QStringLiteral("ai_assist")},
            {QStringLiteral("capture_body_from_video"), QStringLiteral("ai_assist")},
            {QStringLiteral("generate_mesh_from_image"), QStringLiteral("image_to_3d")},
            {QStringLiteral("generate_pbr_maps"), QStringLiteral("pbr_synth")},
            {QStringLiteral("upscale_texture"), QStringLiteral("pbr_synth")},
            {QStringLiteral("generate_normal_map"), QStringLiteral("pbr_synth")},
            {QStringLiteral("pack_textures"), QStringLiteral("texture_atlas")},
            {QStringLiteral("pack_atlas"), QStringLiteral("texture_atlas")},
            {QStringLiteral("apply_atlas"), QStringLiteral("texture_atlas")},
            {QStringLiteral("generate_isometric_sprites"), QStringLiteral("isometric_sprites")},
            {QStringLiteral("bake_vat"), QStringLiteral("vat_bake")},
            {QStringLiteral("list_morph_targets"), QStringLiteral("morph")},
            {QStringLiteral("describe_material"), QStringLiteral("material_editor")},
            {QStringLiteral("apply_material_preset"), QStringLiteral("material_editor")},
            {QStringLiteral("create_material"), QStringLiteral("material_editor")},
            {QStringLiteral("generate_mesh_texture"), QStringLiteral("stable_diffusion")},
            {QStringLiteral("cloud_upload"), QStringLiteral("cloud_upload")},
        };
        const QString feature = toolFeatureMap.value(name);
        if (!feature.isEmpty())
            GamificationManager::noteFeature(feature, GamificationManager::Surface::Mcp);
    }

    const bool failed = toolResult.contains("isError") && toolResult["isError"].toBool();
    static const auto sceneChangingTools = QSet{
        QStringLiteral("load_mesh"), QStringLiteral("transform_mesh"), QStringLiteral("transform_submesh"),
        QStringLiteral("apply_material"), QStringLiteral("create_material"), QStringLiteral("modify_material"),
        QStringLiteral("export_mesh"), QStringLiteral("auto_uv_unwrap"), QStringLiteral("uv_project"),
        QStringLiteral("uv_set_seams"), QStringLiteral("uv_unwrap_selection"), QStringLiteral("retopologize"),
        QStringLiteral("compute_skin_weights"), QStringLiteral("auto_rig"), QStringLiteral("generate_mesh_texture"),
        QStringLiteral("generate_pbr_maps"), QStringLiteral("upscale_texture"), QStringLiteral("create_primitive"),
        QStringLiteral("animate"), QStringLiteral("add_keyframe"), QStringLiteral("remove_keyframe"),
        QStringLiteral("merge_animations"), QStringLiteral("resample_animation"), QStringLiteral("simplify_animation"),
        QStringLiteral("bake_animation_fps"), QStringLiteral("motion_in_between"), QStringLiteral("generate_motion"),
        QStringLiteral("adjust_arm_space"), QStringLiteral("segment_mesh"), QStringLiteral("generate_mesh_from_image"),
        QStringLiteral("save_scene"), QStringLiteral("open_scene"), QStringLiteral("generate_lods"),
        QStringLiteral("generate_auto_lods"), QStringLiteral("remove_lods"), QStringLiteral("decimate_mesh"),
        QStringLiteral("delete_entity"), QStringLiteral("create_light"), QStringLiteral("delete_light"),
        QStringLiteral("set_light_property"), QStringLiteral("apply_light_rig"), QStringLiteral("duplicate_entity"),
        QStringLiteral("group_nodes"), QStringLiteral("ungroup_node"), QStringLiteral("reparent_node"),
        QStringLiteral("apply_atlas"), QStringLiteral("optimize_mesh"), QStringLiteral("bake_vat"),
        QStringLiteral("set_morph_weight"), QStringLiteral("import_alembic"), QStringLiteral("set_node_keyframe"),
        QStringLiteral("apply_pose"), QStringLiteral("delete_pose"), QStringLiteral("mirror_pose"),
        QStringLiteral("load_pose_library")
    };
    SentryReporter::captureInvocationEvent(QStringLiteral("mcp"), name,
        failed ? QStringLiteral("failed") : QStringLiteral("completed"),
        telemetryTimer.elapsed(), sceneChangingTools.contains(name) && !failed,
        failed ? QStringLiteral("tool_error") : QString(), invocationId);

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

namespace {

QString hdrTonemapOperatorSlug(HdrTonemap::Operator op)
{
    switch (op) {
    case HdrTonemap::Operator::Reinhard:
        return QStringLiteral("reinhard");
    case HdrTonemap::Operator::AgX:
        return QStringLiteral("agx");
    case HdrTonemap::Operator::ACES:
    default:
        return QStringLiteral("aces");
    }
}

std::optional<HdrTonemap::Operator> parseHdrTonemapOperator(const QString& raw)
{
    const QString t = raw.trimmed().toLower();
    if (t.isEmpty())
        return std::nullopt;
    if (t == QStringLiteral("reinhard"))
        return HdrTonemap::Operator::Reinhard;
    if (t == QStringLiteral("agx"))
        return HdrTonemap::Operator::AgX;
    if (t == QStringLiteral("aces"))
        return HdrTonemap::Operator::ACES;
    return std::nullopt;
}

bool parseHexTint(const QString& raw, QColor& out)
{
    const QColor c(raw);
    if (!c.isValid())
        return false;
    out = c;
    return true;
}

} // namespace

QJsonObject MCPServer::toolSetHdrEnvironment(const QJsonObject &args)
{
    QString path = args[QStringLiteral("path_or_name")].toString();
    if (path.isEmpty()) path = args[QStringLiteral("path")].toString();
    if (path.isEmpty()) path = args[QStringLiteral("name")].toString();
    if (path.isEmpty())
        return makeErrorResult(QStringLiteral("Error: 'path_or_name' is required"));

    auto* ctrl = HdrEnvironmentController::instance();
    if (!ctrl->loadEnvironment(path))
        return makeErrorResult(QStringLiteral("Error: Failed to load environment '%1'").arg(path));

    if (auto* hdrMgr = HDREnvironmentManager::getSingletonPtr()) {
        return makeSuccessResult(QStringLiteral("Loaded HDR environment: %1")
                                     .arg(hdrMgr->currentEnvironment()));
    }
    return makeSuccessResult(QStringLiteral("Loaded HDR environment: %1").arg(path));
}

QJsonObject MCPServer::toolGetHdrEnvironment(const QJsonObject &)
{
    auto* hdrMgr = HDREnvironmentManager::getSingletonPtr();
    if (!hdrMgr)
        return makeErrorResult(QStringLiteral("Error: HDREnvironmentManager not available"));

    QJsonObject root;
    root[QStringLiteral("environment")] = hdrMgr->currentEnvironment();
    root[QStringLiteral("cache_key")] = hdrMgr->currentCacheKey();
    root[QStringLiteral("ibl_ready")] = hdrMgr->isIblReady();
    root[QStringLiteral("has_environment")] = hdrMgr->hasEnvironment();

    QJsonObject tonemap;
    tonemap[QStringLiteral("operator")] = hdrTonemapOperatorSlug(hdrMgr->tonemapOperator());
    tonemap[QStringLiteral("exposure_ev")] = hdrMgr->exposureEv();
    tonemap[QStringLiteral("white_point")] = hdrMgr->whitePoint();
    root[QStringLiteral("tonemap")] = tonemap;
    root[QStringLiteral("skybox_visible")] = hdrMgr->defaultSkyBoxVisible();
    root[QStringLiteral("background_blur")] = hdrMgr->backgroundBlur();

    return makeSuccessResult(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolSetTonemap(const QJsonObject &args)
{
    auto* ctrl = HdrEnvironmentController::instance();
    auto* hdrMgr = HDREnvironmentManager::getSingletonPtr();
    if (!hdrMgr || !ctrl)
        return makeErrorResult(QStringLiteral("Error: HDREnvironmentManager not available"));

    QString opStr = args[QStringLiteral("operator")].toString();
    if (opStr.isEmpty())
        opStr = args[QStringLiteral("tonemap")].toString();

    QStringList changes;
    if (!opStr.isEmpty()) {
        const auto parsed = parseHdrTonemapOperator(opStr);
        if (!parsed)
            return makeErrorResult(QStringLiteral("Error: Unknown tonemap operator '%1' (use aces, reinhard, or agx)").arg(opStr));
        ctrl->setTonemapOperator(static_cast<int>(*parsed));
        changes << QStringLiteral("operator=%1").arg(hdrTonemapOperatorSlug(*parsed));
    }
    if (args.contains(QStringLiteral("exposure")) || args.contains(QStringLiteral("exposure_ev"))) {
        const float exposure = static_cast<float>(
            args.contains(QStringLiteral("exposure"))
                ? args[QStringLiteral("exposure")].toDouble()
                : args[QStringLiteral("exposure_ev")].toDouble());
        ctrl->setExposureEv(exposure);
        changes << QStringLiteral("exposure_ev=%1").arg(exposure);
    }
    if (args.contains(QStringLiteral("white_point"))) {
        const float whitePoint = static_cast<float>(args[QStringLiteral("white_point")].toDouble());
        ctrl->setWhitePoint(whitePoint);
        changes << QStringLiteral("white_point=%1").arg(whitePoint);
    }

    if (changes.isEmpty())
        return makeErrorResult(QStringLiteral("Error: Provide operator and/or exposure and/or white_point"));

    return makeSuccessResult(QStringLiteral("Updated tonemap: %1").arg(changes.join(QStringLiteral(", "))));
}

QJsonObject MCPServer::toolSetEnvIntensity(const QJsonObject &args)
{
    QString name = args[QStringLiteral("material")].toString();
    if (name.isEmpty()) name = args[QStringLiteral("name")].toString();
    if (name.isEmpty())
        return makeErrorResult(QStringLiteral("Error: 'material' is required"));

    if (!args.contains(QStringLiteral("value")) && !args.contains(QStringLiteral("intensity")))
        return makeErrorResult(QStringLiteral("Error: 'value' (or 'intensity') is required"));

    const float intensity = static_cast<float>(args.contains(QStringLiteral("value"))
                                                   ? args[QStringLiteral("value")].toDouble()
                                                   : args[QStringLiteral("intensity")].toDouble());

    try {
        Ogre::MaterialPtr material =
            Ogre::MaterialManager::getSingleton().getByName(name.toStdString());
        if (!material)
            return makeErrorResult(QStringLiteral("Error: Material '%1' not found").arg(name));
        if (material->getNumTechniques() == 0 || material->getTechnique(0)->getNumPasses() == 0)
            return makeErrorResult(QStringLiteral("Error: Material '%1' has no passes").arg(name));

        Ogre::Pass* pass = material->getTechnique(0)->getPass(0);
        RTShaderHelper::setPbrEnvIntensity(pass, intensity);
        RTShaderHelper::wirePbrSlotsForFFP(material.get());
        material->compile(false);
        material->load();

        SentryReporter::addBreadcrumb(QStringLiteral("ai.tool_call"),
                                      QStringLiteral("set_env_intensity %1=%2").arg(name).arg(intensity));
        return makeSuccessResult(QStringLiteral("Set env intensity on '%1' to %2").arg(name).arg(intensity));
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QStringLiteral("Ogre error: %1")
                                   .arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolSetEnvTint(const QJsonObject &args)
{
    QString name = args[QStringLiteral("material")].toString();
    if (name.isEmpty()) name = args[QStringLiteral("name")].toString();
    if (name.isEmpty())
        return makeErrorResult(QStringLiteral("Error: 'material' is required"));

    const QString hex = args[QStringLiteral("hex")].toString();
    if (hex.isEmpty())
        return makeErrorResult(QStringLiteral("Error: 'hex' is required (e.g. #fff5e6)"));

    QColor tint;
    if (!parseHexTint(hex, tint))
        return makeErrorResult(QStringLiteral("Error: Invalid hex color '%1'").arg(hex));

    try {
        Ogre::MaterialPtr material =
            Ogre::MaterialManager::getSingleton().getByName(name.toStdString());
        if (!material)
            return makeErrorResult(QStringLiteral("Error: Material '%1' not found").arg(name));
        if (material->getNumTechniques() == 0 || material->getTechnique(0)->getNumPasses() == 0)
            return makeErrorResult(QStringLiteral("Error: Material '%1' has no passes").arg(name));

        Ogre::Pass* pass = material->getTechnique(0)->getPass(0);
        RTShaderHelper::setPbrEnvTint(
            pass, Ogre::ColourValue(tint.redF(), tint.greenF(), tint.blueF()));
        RTShaderHelper::wirePbrSlotsForFFP(material.get());
        material->compile(false);
        material->load();

        SentryReporter::addBreadcrumb(QStringLiteral("ai.tool_call"),
                                      QStringLiteral("set_env_tint %1=%2").arg(name, hex));
        return makeSuccessResult(QStringLiteral("Set env tint on '%1' to %2").arg(name, hex));
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QStringLiteral("Ogre error: %1")
                                   .arg(QString::fromStdString(e.getFullDescription())));
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
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QStringLiteral("Ogre error: %1")
            .arg(QString::fromStdString(e.getFullDescription())));
    }
    // Frame the camera on what was just loaded so a subsequent take_screenshot
    // shows the mesh (headless MCP has no user to frame it manually). Done
    // OUTSIDE the import try + swallowing its own errors, so a framing failure
    // never masks a SUCCESSFUL import (CodeRabbit). It also restores the prior
    // selection, so it doesn't disrupt an interactive --with-mcp session.
    frameSceneInActiveViewport();
    return makeSuccessResult(QString("Loaded mesh from: %1").arg(path));
}

void MCPServer::frameSceneInActiveViewport()
{
    Manager* mgr = Manager::getSingletonPtr();
    SelectionSet* sel = SelectionSet::getSingleton();
    if (!mgr || !sel)
        return;

    auto* top = TransformOperator::getSingleton();
    OgreWidget* ogreWidget = top ? top->getActiveWidget() : nullptr;
    if (!ogreWidget && m_mainWindow)
        ogreWidget = m_mainWindow->findChild<OgreWidget*>();
    if (!ogreWidget || !ogreWidget->getSpaceCamera())
        return;

    // Snapshot the current node selection so we can restore it — framing needs a
    // selection (frameSelection() no-ops on empty) but must not clobber what an
    // interactive user had selected (CodeRabbit).
    const QList<Ogre::SceneNode*> prevSelection = sel->getNodesSelectionList();

    try {
        sel->clearList();
        bool any = false;
        for (Ogre::SceneNode* node : mgr->getSceneNodes()) {
            if (node && node->numAttachedObjects() > 0) {
                sel->append(node);
                any = true;
            }
        }
        if (any)
            ogreWidget->getSpaceCamera()->frameSelection();
    } catch (const Ogre::Exception&) {
        // best-effort framing; fall through to restore selection.
    }

    // Restore the prior selection (empty list == deselect, matching before).
    sel->clearList();
    for (Ogre::SceneNode* node : prevSelection)
        if (node) sel->append(node);
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

    Ogre::Entity* entity = firstResolvedSelectedEntity();
    if (!entity)
        return makeErrorResult("No selected entity.");

    SentryReporter::addBreadcrumb(QStringLiteral("mesh.uv.unwrap"),
        QStringLiteral("auto_uv_unwrap entity=%1 res=%2 pad=%3 ch=%4")
            .arg(QString::fromStdString(entity->getName()))
            .arg(opts.resolution).arg(opts.padding).arg(opts.channel));

    UvUnwrapReport report;
    try {
        report = UvPipeline::unwrapEntity(entity, opts);
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

QJsonObject MCPServer::toolUvInfo(const QJsonObject &args)
{
    if (!hasSelectedEntities())
        return makeErrorResult("No mesh selected. Load a mesh first with load_mesh.");

    Ogre::Entity* entity = firstResolvedSelectedEntity();
    if (!entity)
        return makeErrorResult("No selected entity.");

    const int channel = args.contains("channel") ? args["channel"].toInt(0) : 0;
    SentryReporter::addBreadcrumb(QStringLiteral("mesh.uv.info"),
        QString::fromStdString(entity->getName()));

    const auto info = UvPipeline::analyzeEntity(entity, channel);
    QJsonObject result = makeSuccessResult(UvPipeline::infoToText(
        QString::fromStdString(entity->getName()), info));
    result["uv"] = UvPipeline::infoToJson(QString::fromStdString(entity->getName()), info);
    return result;
}

QJsonObject MCPServer::toolUvProject(const QJsonObject &args)
{
    if (!hasSelectedEntities())
        return makeErrorResult("No mesh selected. Load a mesh first with load_mesh.");

    Ogre::Entity* entity = firstResolvedSelectedEntity();
    if (!entity)
        return makeErrorResult("No selected entity.");

    const QString modeName = args.value(QStringLiteral("mode")).toString(QStringLiteral("box"));
    bool ok = false;
    const UvProject::Mode mode = UvPipeline::parseProjectMode(modeName, &ok);
    if (!ok)
        return makeErrorResult("Error: mode must be box, cylinder, sphere, or reset.");

    const int channel = args.contains("channel") ? args["channel"].toInt(0) : 0;
    UvProject::Options opts;
    if (args.contains("axis")) opts.axis = args["axis"].toInt(1);
    if (args.contains("scale")) {
        const double scale = args["scale"].toDouble(1.0);
        opts.boxScale = static_cast<float>(scale > 0.0 ? scale : 1.0);
    }

    SentryReporter::addBreadcrumb(QStringLiteral("mesh.uv.project"), modeName);

    UvProject::Report report;
    try {
        report = UvPipeline::projectEntity(entity, mode, channel, opts);
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QStringLiteral("Ogre error: %1")
            .arg(QString::fromStdString(e.getFullDescription())));
    }

    if (!report.applied)
        return makeErrorResult(QStringLiteral("UV projection failed: %1").arg(report.error));

    QJsonObject result = makeSuccessResult(
        QStringLiteral("Projected %1 vertices (%2)").arg(report.vertsChanged).arg(modeName));
    result["vertsChanged"] = report.vertsChanged;
    result["mode"] = modeName;
    return result;
}

QJsonObject MCPServer::toolUvSetSeams(const QJsonObject &args)
{
    if (!hasSelectedEntities())
        return makeErrorResult("No mesh selected. Load a mesh first with load_mesh.");

    Ogre::Entity* entity = firstResolvedSelectedEntity();
    if (!entity)
        return makeErrorResult("No selected entity.");

    const QString spec = args.value(QStringLiteral("edges")).toString();
    if (spec.isEmpty())
        return makeErrorResult("Error: 'edges' is required (e.g. \"0:1-2,0:2-3\").");

    std::vector<UvPipeline::SeamEdge> edges;
    QString parseError;
    if (!UvPipeline::parseSeamEdgeList(spec, edges, &parseError))
        return makeErrorResult(QStringLiteral("Invalid edges: %1").arg(parseError));

    SentryReporter::addBreadcrumb(QStringLiteral("mesh.uv.seam"),
        QStringLiteral("uv_set_seams count=%1").arg(edges.size()));

    QString seamError;
    if (!UvPipeline::setSeamsOnEntity(entity, edges, &seamError))
        return makeErrorResult(QStringLiteral("Set seams failed: %1").arg(seamError));

    QJsonObject result = makeSuccessResult(
        QStringLiteral("Marked %1 seam edges").arg(edges.size()));
    result["seamCount"] = static_cast<int>(edges.size());
    return result;
}

QJsonObject MCPServer::toolUvUnwrapSelection(const QJsonObject &args)
{
    if (!hasSelectedEntities())
        return makeErrorResult("No mesh selected. Load a mesh first with load_mesh.");

    Ogre::Entity* entity = firstResolvedSelectedEntity();
    if (!entity)
        return makeErrorResult("No selected entity.");

    int subMesh = 0;
    if (args.contains("submesh")) {
        QString intErr;
        if (!mcpJsonIntValue(args.value("submesh"), &subMesh, &intErr, "submesh"))
            return makeErrorResult(intErr);
    }

    std::vector<int> triangles;
    if (!args.contains("triangles") || !args["triangles"].isArray())
        return makeErrorResult("Error: 'triangles' array is required.");
    for (const QJsonValue& v : args["triangles"].toArray()) {
        int ti = 0;
        QString intErr;
        if (!mcpJsonIntValue(v, &ti, &intErr, "triangles[]"))
            return makeErrorResult(intErr);
        triangles.push_back(ti);
    }
    if (triangles.empty())
        return makeErrorResult("Error: 'triangles' array is required.");

    UvUnwrapOptions opts;
    if (args.contains("resolution")) opts.resolution = args["resolution"].toInt(1024);
    if (args.contains("padding"))    opts.padding    = args["padding"].toInt(4);
    if (args.contains("channel"))    opts.channel    = args["channel"].toInt(0);
    if (args.contains("preserve_original"))
        opts.preserveOriginalAsBackup = args["preserve_original"].toBool(true);

    SentryReporter::addBreadcrumb(QStringLiteral("mesh.uv.unwrap"),
        QStringLiteral("uv_unwrap_selection sub=%1 tris=%2").arg(subMesh).arg(triangles.size()));

    UvUnwrapReport report;
    try {
        report = UvPipeline::unwrapTriangles(entity, subMesh, triangles, opts);
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QStringLiteral("Ogre error: %1")
            .arg(QString::fromStdString(e.getFullDescription())));
    }

    if (!report.applied)
        return makeErrorResult(QStringLiteral("UV unwrap failed: %1").arg(report.error));

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
    if (args.contains("algo") && !args["algo"].isString())
        return makeErrorResult("Error: 'algo' must be a string.");
    if (args.contains("voxel_resolution") && !args["voxel_resolution"].isDouble())
        return makeErrorResult("Error: 'voxel_resolution' must be a number.");
    if (args.contains("smooth_iterations") && !args["smooth_iterations"].isDouble())
        return makeErrorResult("Error: 'smooth_iterations' must be a number.");

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
    if (args.contains("voxel_resolution"))
        opts.voxelResolution = args["voxel_resolution"].toInt(64);
    if (args.contains("smooth_iterations"))
        opts.smoothIterations = args["smooth_iterations"].toInt(3);

    QString algoName = QStringLiteral("skintokens");
    if (args.contains("algo")) {
        algoName = args["algo"].toString().toLower();
        if (algoName != "skintokens" && algoName != "geodesic-voxel"
            && algoName != "inverse-distance"
            && algoName != "unirig")   // deprecated alias of skintokens
            return makeErrorResult("Error: 'algo' must be 'skintokens', "
                                   "'geodesic-voxel', or 'inverse-distance'.");
    }
    const SkinWeights::Algorithm algo = SkinWeights::algorithmFromString(algoName);

    if (opts.maxInfluencesPerVertex < 1 || opts.maxInfluencesPerVertex > 8)
        return makeErrorResult("Error: 'max_influences' must be in [1, 8].");
    if (opts.falloff < 0.5 || opts.falloff > 16.0)
        return makeErrorResult("Error: 'falloff' must be in [0.5, 16].");
    if (opts.maxInfluenceDistance < 0.0 || opts.maxInfluenceDistance > 10.0)
        return makeErrorResult("Error: 'max_distance' must be in [0, 10].");
    if (opts.voxelResolution < 8 || opts.voxelResolution > 256)
        return makeErrorResult("Error: 'voxel_resolution' must be in [8, 256].");
    if (opts.smoothIterations < 0 || opts.smoothIterations > 50)
        return makeErrorResult("Error: 'smooth_iterations' must be in [0, 50].");

    SelectionSet* sel = SelectionSet::getSingleton();
    const QList<Ogre::Entity*> resolved = sel ? sel->getResolvedEntities()
                                              : QList<Ogre::Entity*>{};
    if (resolved.isEmpty())
        return makeErrorResult("No selected entity.");
    Ogre::Entity* entity = resolved.first();
    if (!entity) return makeErrorResult("Selected entity is null.");

    SentryReporter::addBreadcrumb(
        QStringLiteral("ai.assist.skin.%1").arg(algoName),
        QStringLiteral("compute_skin_weights entity=%1 maxInf=%2 falloff=%3 voxelRes=%4 smooth=%5")
            .arg(QString::fromStdString(entity->getName()))
            .arg(opts.maxInfluencesPerVertex).arg(opts.falloff)
            .arg(opts.voxelResolution).arg(opts.smoothIterations));

    SkinWeightsReport report;
    try {
        report = SkinWeights::computeAndApply(entity, opts, algo);
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

QJsonObject MCPServer::toolSetSkinningDisplay(const QJsonObject &args)
{
    // #819 Slice D: per-entity Linear / Dual-Quaternion display
    // toggle. Runtime shading only — exported weights are unchanged.
    if (!args.contains("mode") || !args["mode"].isString())
        return makeErrorResult("Error: 'mode' (string) is required.");
    const QString modeStr = args["mode"].toString().toLower();
    if (modeStr != "linear" && modeStr != "dual-quaternion")
        return makeErrorResult("Error: 'mode' must be 'linear' or "
                               "'dual-quaternion'.");

    SelectionSet* sel = SelectionSet::getSingleton();
    const QList<Ogre::Entity*> resolved = sel ? sel->getResolvedEntities()
                                              : QList<Ogre::Entity*>{};
    if (resolved.isEmpty())
        return makeErrorResult("No selected entity. Load a mesh first with load_mesh.");
    Ogre::Entity* entity = resolved.first();
    if (!entity) return makeErrorResult("Selected entity is null.");

    SentryReporter::addBreadcrumb(QStringLiteral("render.skinning"),
        QStringLiteral("set_skinning_display mode=%1 entity=%2")
            .arg(modeStr, QString::fromStdString(entity->getName())));

    QString err;
    if (!SkinningDisplay::apply(entity,
                                SkinningDisplay::modeFromString(modeStr), &err))
        return makeErrorResult(QStringLiteral("Failed: %1").arg(err));

    return makeSuccessResult(QStringLiteral(
        "Skinning display set to %1 on '%2'. Display only — exported "
        "weights are unchanged.")
        .arg(modeStr, QString::fromStdString(entity->getName())));
}

QJsonObject MCPServer::toolAutoRig(const QJsonObject &args)
{
    // Issue #407: native auto-rig of the selected STATIC mesh. Generates a
    // skeleton from a template, binds it, optionally chains skin weights, and
    // optionally re-exports.
    if (!hasSelectedEntities())
        return makeErrorResult("No mesh selected. Load a mesh first with load_mesh.");

    if (args.contains("skin") && !args["skin"].isBool())
        return makeErrorResult("Error: 'skin' must be a boolean.");

    AutoRig::Options opts;
    if (args.contains("template")) {
        if (!args["template"].isString())
            return makeErrorResult("Error: 'template' must be a string.");
        opts.tmpl = AutoRig::templateFromString(args["template"].toString());
    }
    if (args.contains("algo")) {
        if (!args["algo"].isString())
            return makeErrorResult("Error: 'algo' must be a string.");
        const QString a = args["algo"].toString().toLower();
        if (a != "pinocchio" && a != "unirig" && a != "rignet")
            return makeErrorResult("Error: 'algo' must be 'pinocchio' or 'unirig'.");
        opts.algorithm = AutoRig::algorithmFromString(a);
    }
    if (args.contains("up_axis")) {
        const QString a = args["up_axis"].toString().toLower();
        if (a == "x") opts.upAxis = 0;
        else if (a == "y") opts.upAxis = 1;
        else if (a == "z") opts.upAxis = 2;
        else return makeErrorResult("Error: 'up_axis' must be 'x', 'y', or 'z'.");
    }
    const bool alsoSkin = args.value("skin").toBool(false);

    SelectionSet* sel = SelectionSet::getSingleton();
    const QList<Ogre::Entity*> resolved = sel ? sel->getResolvedEntities()
                                              : QList<Ogre::Entity*>{};
    if (resolved.isEmpty())
        return makeErrorResult("No selected entity.");
    Ogre::Entity* entity = resolved.first();
    if (!entity) return makeErrorResult("Selected entity is null.");

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.auto_rig"),
        QStringLiteral("auto_rig entity=%1 template=%2 algo=%3 skin=%4")
            .arg(QString::fromStdString(entity->getName()),
                 AutoRig::templateToString(opts.tmpl),
                 AutoRig::algorithmToString(opts.algorithm))
            .arg(alsoSkin));

    // Validate output_path type up front (like 'skin'/'template') — a
    // non-string would otherwise coerce to "" and silently skip the export
    // while still reporting success.
    if (args.contains("output_path") && !args["output_path"].isString())
        return makeErrorResult("Error: 'output_path' must be a string.");
    const QString outputPath = args.value("output_path").toString();

    AutoRig::Report report;
    bool skinned = false;
    // Wrap the full mutating + export section so export failures and
    // std::runtime_error (not just Ogre::Exception) reach the MCP error path.
    try {
        report = AutoRig::rigEntity(entity, opts);
        if (!report.applied)
            return makeErrorResult(
                QStringLiteral("Auto-rig failed: %1").arg(report.error));

        if (alsoSkin) {
            const auto sw = SkinWeights::computeAndApply(entity, {});
            skinned = sw.applied;
            // A requested skin that failed is a hard error — don't export an
            // unskinned asset and report success.
            if (!sw.applied)
                return makeErrorResult(QStringLiteral(
                    "Auto-rig succeeded, but the requested skinning failed: %1")
                    .arg(sw.error));
        }

        // Optional re-export of the now-rigged mesh.
        if (!outputPath.isEmpty()) {
            Ogre::SceneNode* node = entity->getParentSceneNode();
            if (!node)
                return makeErrorResult(
                    QStringLiteral("Error: rigged, but the entity has no scene "
                                   "node to export from"));
            // Don't leak the full local path (usernames / private dirs) to Sentry.
            SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
                QStringLiteral("auto_rig export requested"));
            const int rc = MeshImporterExporter::exporter(
                node, outputPath, CLIPipeline::formatForExtension(outputPath));
            if (rc != 0)
                return makeErrorResult(
                    QStringLiteral("Error: rigged but export to '%1' failed (code %2)")
                        .arg(outputPath).arg(rc));
        }
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QStringLiteral("Ogre error: %1")
            .arg(QString::fromStdString(e.getFullDescription())));
    } catch (const std::exception& e) {
        return makeErrorResult(QStringLiteral("Auto-rig error: %1")
            .arg(QString::fromUtf8(e.what())));
    }

    QJsonObject result = makeSuccessResult(AutoRig::reportToText(report));
    QJsonObject j = AutoRig::reportToJson(report);
    j["skinned"] = skinned;
    result["rig"] = j;
    return result;
}

QJsonObject MCPServer::toolRemoveSkeleton(const QJsonObject &args)
{
    Q_UNUSED(args);
    if (!hasSelectedEntities())
        return makeErrorResult("No mesh selected. Load a mesh first with load_mesh.");
    SelectionSet* sel = SelectionSet::getSingleton();
    const QList<Ogre::Entity*> resolved = sel ? sel->getResolvedEntities()
                                              : QList<Ogre::Entity*>{};
    if (resolved.isEmpty() || !resolved.first())
        return makeErrorResult("No selected entity.");
    Ogre::Entity* entity = resolved.first();
    if (!entity->getMesh() || !entity->getMesh()->hasSkeleton())
        return makeErrorResult("Selected entity has no skeleton.");

    SentryReporter::addBreadcrumb(QStringLiteral("ai.tool_call"),
        QStringLiteral("remove_skeleton entity=%1")
            .arg(QString::fromStdString(entity->getName())));

    // Same undoable command the Inspector's "Delete Skeleton" button pushes.
    auto* cmd = new RemoveSkeletonCommand(entity->getName());
    UndoManager::getSingleton()->push(cmd);
    if (!cmd->applied())
        return makeErrorResult("Remove skeleton failed.");
    return makeSuccessResult(
        QStringLiteral("Skeleton removed from '%1' — the mesh is static again "
                       "(auto_rig can regenerate a rig).")
            .arg(QString::fromStdString(entity->getName())));
}

QJsonObject MCPServer::toolAddArkitBlendshapes(const QJsonObject &args)
{
    // #889: fit the ARKit blendshape template onto the selected face mesh and
    // attach the 52 ARKit morph targets, optionally re-exporting.
    if (!hasSelectedEntities())
        return makeErrorResult("No mesh selected. Load a mesh first with load_mesh.");

    FaceRig::FaceRigOptions opts;
    if (args.contains("max_shapes")) {
        if (!args["max_shapes"].isDouble())
            return makeErrorResult("Error: 'max_shapes' must be a number.");
        opts.maxShapes = args["max_shapes"].toInt();
    }
    if (args.contains("max_residual_pct")) {
        if (!args["max_residual_pct"].isDouble())
            return makeErrorResult("Error: 'max_residual_pct' must be a number.");
        opts.maxFitResidualPct = args["max_residual_pct"].toDouble();
    }
    if (args.contains("output_path") && !args["output_path"].isString())
        return makeErrorResult("Error: 'output_path' must be a string.");
    const QString outputPath = args.value("output_path").toString();

    SelectionSet* sel = SelectionSet::getSingleton();
    const QList<Ogre::Entity*> resolved = sel ? sel->getResolvedEntities()
                                              : QList<Ogre::Entity*>{};
    if (resolved.isEmpty() || !resolved.first())
        return makeErrorResult("No selected entity.");
    Ogre::Entity* entity = resolved.first();

    SentryReporter::addBreadcrumb(QStringLiteral("ai.tool_call"),
        QStringLiteral("add_arkit_blendshapes max_shapes=%1").arg(opts.maxShapes));

    FaceRig::AttachReport rep;
    try {
        rep = FaceRig::attachFaceRigWithBundledTemplate(entity, opts);
        if (!rep.ok)
            return makeErrorResult(
                QStringLiteral("Face-rig failed: %1").arg(rep.error));

        if (!outputPath.isEmpty()) {
            Ogre::SceneNode* node = entity->getParentSceneNode();
            if (!node)
                return makeErrorResult(QStringLiteral(
                    "Error: rigged, but the entity has no scene node to export from"));
            SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
                QStringLiteral("add_arkit_blendshapes export requested"));
            const int rc = MeshImporterExporter::exporter(
                node, outputPath, CLIPipeline::formatForExtension(outputPath));
            if (rc != 0)
                return makeErrorResult(
                    QStringLiteral("Error: blendshapes attached but export to '%1' "
                                   "failed (code %2)").arg(outputPath).arg(rc));
            // NOTE: no sidecar write here — MeshImporterExporter::exporter
            // already writes the FULL deduplicated pose-name sidecar;
            // overwriting it with only the newly-attached names would drop
            // pre-existing morph targets and misalign indices.
        }
    } catch (const Ogre::Exception& e) {
        return makeErrorResult(QStringLiteral("Ogre error: %1")
            .arg(QString::fromStdString(e.getFullDescription())));
    } catch (const std::exception& e) {
        return makeErrorResult(QStringLiteral("Face-rig error: %1")
            .arg(QString::fromUtf8(e.what())));
    }

    QJsonObject result = makeSuccessResult(
        QStringLiteral("Attached %1 ARKit blendshape(s) (fit residual mean %2%, "
                       "max %3%).")
            .arg(rep.shapesAttached)
            .arg(rep.fitMeanResidualPct, 0, 'f', 3)
            .arg(rep.fitMaxResidualPct, 0, 'f', 3));
    QJsonObject j;
    j["shapes_attached"] = rep.shapesAttached;
    j["user_vertex_count"] = rep.userVertexCount;
    j["fit_mean_residual_pct"] = rep.fitMeanResidualPct;
    j["fit_max_residual_pct"] = rep.fitMaxResidualPct;
    result["facerig"] = j;
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

QJsonObject MCPServer::toolGenerateMeshFromImage(const QJsonObject &args)
{
    // NOT blanket-gated on ENABLE_ONNX: the TRELLIS.2 backend runs through an
    // external runtime (trellis.cpp / Python sidecar) and works in ONNX-off
    // builds — only the local TripoSR/TripoSG paths require the ONNX build.
    const QString imagePath = args.value("image_path").toString();
    if (imagePath.trimmed().isEmpty())
        return makeErrorResult("'image_path' is required.");
    if (!QFileInfo::exists(imagePath))
        return makeErrorResult(QStringLiteral("image not found: %1").arg(imagePath));

    MeshGenPredictor::Options opts;
    if (args.contains("resolution")) opts.sdfResolution = args["resolution"].toInt(256);
    if (args.contains("vertex_color")) opts.vertexColor = args["vertex_color"].toBool();
    if (args.contains("remove_bg")) opts.removeBackground = args["remove_bg"].toBool();
    if (opts.sdfResolution < 16 || opts.sdfResolution > 1024)
        return makeErrorResult("'resolution' must be between 16 and 1024.");
    if (args.contains("quality")) {
        const QString q = args["quality"].toString().toLower();
        if (q == "int8") opts.quality = MeshGenPredictor::Quality::Int8;
        else if (q == "fp32" || q.isEmpty()) opts.quality = MeshGenPredictor::Quality::Fp32;
        else return makeErrorResult("'quality' must be 'fp32' or 'int8'.");
    }
    // Quality pass (defaults ON — see MeshGenPredictor::Options).
    if (args.contains("smooth")) opts.smoothMesh = args["smooth"].toBool(true);
    if (args.contains("refine")) opts.refineSurface = args["refine"].toBool(true);
    if (args.contains("bake_texture")) opts.bakeTexture = args["bake_texture"].toBool(true);
    if (args.contains("texture_size")) {
        opts.textureSize = args["texture_size"].toInt(1024);
        if (opts.textureSize < 64 || opts.textureSize > 8192)
            return makeErrorResult("'texture_size' must be between 64 and 8192.");
    }
    const bool upscaleTex  = args.value("upscale_texture").toBool();
    const bool generatePbr = args.contains("generate_pbr")
        ? args["generate_pbr"].toBool(true) : true;
    // Default backend: TRELLIS.2 when its sidecar runtime is installed on
    // this machine, else TripoSR. An explicit 'backend' arg always wins.
    opts.backend = MeshGenPredictor::defaultBackend();
    if (args.contains("backend")) {
        const QString b = args["backend"].toString().toLower();
        if (b == "triposg")      opts.backend = MeshGenPredictor::Backend::TripoSG;
        else if (b == "trellis2" || b == "trellis.2" || b == "trellis")
            opts.backend = MeshGenPredictor::Backend::Trellis2;
        else if (b == "triposr")
            opts.backend = MeshGenPredictor::Backend::TripoSR;
        else if (!b.isEmpty())
            return makeErrorResult("'backend' must be 'trellis2', 'triposr' or 'triposg'.");
    }
    if (args.contains("seed")) {
        const int s = args["seed"].toInt(42);
        if (s < 0) return makeErrorResult("'seed' must be >= 0.");
        opts.seed = static_cast<unsigned>(s);
    }
    if (args.contains("preset")) {
        const QString p = args["preset"].toString().toLower();
        if (p != "fast" && p != "balanced" && p != "high")
            return makeErrorResult("'preset' must be 'fast', 'balanced' or 'high'.");
        opts.trellis2Preset = p;
    }
    if (args.contains("target_tris")) {
        opts.targetTriangles = args["target_tris"].toInt(0);
        if (opts.targetTriangles < 0 || opts.targetTriangles > 10000000)
            return makeErrorResult("'target_tris' must be in [0, 10000000] (0 = original).");
    }
    opts.bakeNormalMap = generatePbr && opts.bakeTexture;
    if (args.contains("flow_steps")) {
        opts.flowSteps = args["flow_steps"].toInt(25);
        if (opts.flowSteps < 1 || opts.flowSteps > 200)
            return makeErrorResult("'flow_steps' must be between 1 and 200.");
    }
    if (args.contains("guidance")) {
        opts.guidanceScale = static_cast<float>(args["guidance"].toDouble(7.0));
        if (opts.guidanceScale < 0.0f || opts.guidanceScale > 30.0f)
            return makeErrorResult("'guidance' must be between 0 and 30.");
    }

    const QString backendName =
        opts.backend == MeshGenPredictor::Backend::Trellis2 ? QStringLiteral("trellis2")
        : opts.backend == MeshGenPredictor::Backend::TripoSG ? QStringLiteral("triposg")
                                                             : QStringLiteral("triposr");
    SentryReporter::addBreadcrumb(QStringLiteral("ai.tool_call"),
        QStringLiteral("generate_mesh_from_image %1 res=%2 backend=%3")
            .arg(QFileInfo(imagePath).fileName()).arg(opts.sdfResolution)
            .arg(backendName));

#ifndef ENABLE_ONNX
    if (opts.backend != MeshGenPredictor::Backend::Trellis2)
        return makeErrorResult(
            "This build was compiled without local AI image-to-3D generation "
            "(rebuild with -DENABLE_ONNX=ON, or install the TRELLIS.2 runtime "
            "and use backend 'trellis2').");
#endif
    if (opts.backend == MeshGenPredictor::Backend::Trellis2) {
        if (!Trellis2Predictor::runtimeAvailable())
            return makeErrorResult(Trellis2Predictor::runtimeDescription());
        opts.removeBackground = true;   // trellis2 needs an alpha matte; the
                                        // predictor skips it when the input
                                        // already carries one
        const QString outPath = args.value("output").toString();
        if (!outPath.isEmpty()) {
            // Phase 9: keep the raw full-res generation next to the export.
            opts.trellis2SourceKeepDir = QFileInfo(outPath).absolutePath();
            opts.trellis2SourceKeepBaseName = QFileInfo(outPath).completeBaseName();
        }
    } else if (opts.backend == MeshGenPredictor::Backend::TripoSG) {
        // TripoSG always runs the fp32 DiT (int8 tier dropped — degraded
        // geometry, no ARM speed win); 'quality' still selects the TripoSR
        // tier used for the colour bake.
        const QString enc = TripoSGPredictor::ensureModelBlocking(false);
        if (enc.isEmpty())
            return makeErrorResult(
                "TripoSG models unavailable — they download on first use; if not "
                "hosted yet, set QTMESH_TRIPOSG_MODEL_BASE_URL / ai/triposgModelBaseUrl "
                "or drop the files in the ai_models/triposg/ cache.");
        // TripoSG's colour bake queries TripoSR's colour field — best-effort
        // ensure (absent models fall back to clay with a warning).
        if (opts.bakeTexture)
            MeshGenPredictor::ensureModelBlocking(opts.quality);
    } else {
        const QString enc = MeshGenPredictor::ensureModelBlocking(opts.quality);
        if (enc.isEmpty() || !MeshGenPredictor::modelsPresent(opts.quality))
            return makeErrorResult(
                "TripoSR model unavailable — it downloads on first use; if it is not "
                "hosted yet, set QTMESH_TRIPOSR_MODEL_BASE_URL / ai/triposrModelBaseUrl "
                "or drop the files in the ai_models/triposr/ cache.");
    }

    QImage image(imagePath);
    if (image.isNull())
        return makeErrorResult(QStringLiteral("failed to read image: %1").arg(imagePath));

    MeshGenPredictor::Result res = MeshGenPredictor::predict(
        image, MeshGenPredictor::encoderModelPath(opts.quality),
        MeshGenPredictor::decoderModelPath(), opts);
    if (!res.ok)
        return makeErrorResult(res.error.isEmpty()
            ? QStringLiteral("image-to-3D failed") : res.error);

    // Optional Real-ESRGAN 2x on the baked diffuse (best-effort; keeps the
    // un-upscaled texture on any failure — same policy as the CLI).
#ifdef ENABLE_ONNX
    if (upscaleTex && !res.uvs.empty() && !res.texture.isNull()) {
        const QString upModel = AIAssistManager::instance()->ensureUpscaleModel(2);
        if (!upModel.isEmpty()) {
            const TextureUpscaler::Result ur =
                TextureUpscaler::upscale(res.texture, upModel);
            if (ur.ok && !ur.image.isNull())
                res.texture = ur.image;
        }
    }
#else
    Q_UNUSED(upscaleTex);   // Real-ESRGAN is ONNX-only; best-effort, skipped.
#endif

    // Baked texture (+ synthesized PBR maps): land next to the export target
    // when one is given so the references survive outside the app; else the
    // AppData default.
    const QString output = args.value("output").toString();
    MeshGenBuilder::BuildOptions buildOpts;
    if (!output.trimmed().isEmpty())
        buildOpts.textureDir = QFileInfo(output).absolutePath();
    buildOpts.generatePbrMaps = generatePbr && opts.bakeTexture && opts.vertexColor;
    Ogre::SceneNode* node = MeshGenBuilder::buildSceneNode(
        res, QStringLiteral("qtmesh_gen3d"), buildOpts);
    if (!node)
        return makeErrorResult("failed to build mesh from prediction.");

    QString meshPath;
    if (!output.trimmed().isEmpty()) {
        const QString fmt = CLIPipeline::formatForExtension(output);
        SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
            QStringLiteral("Exporting %1").arg(QFileInfo(output).absoluteFilePath()));
        if (MeshImporterExporter::exporter(node, QFileInfo(output).absoluteFilePath(), fmt) != 0)
            return makeErrorResult(QStringLiteral("export failed: %1").arg(output));
        meshPath = QFileInfo(output).absoluteFilePath();
    }

    QJsonObject result = makeSuccessResult(QStringLiteral(
        "Generated a 3D mesh from '%1' (%2 verts, %3 tris)%4.")
        .arg(QFileInfo(imagePath).fileName())
        .arg(res.vertexCount).arg(res.triangleCount)
        .arg(meshPath.isEmpty() ? QStringLiteral(" and loaded it into the scene")
                                : QStringLiteral(" and saved it")));
    result["vertexCount"]   = res.vertexCount;
    result["triangleCount"] = res.triangleCount;
    if (!meshPath.isEmpty()) result["meshPath"] = meshPath;
    result["backend"] = backendName;
    // Phase 9 (trellis2): the preserved full-resolution generation.
    if (!res.sourceInterchangePath.isEmpty())
        result["sourcePath"] = res.sourceInterchangePath;
    // Surface non-fatal degradations (bake fell back to vertex colours, …) so
    // the MCP caller can tell a textured result from a fallback one.
    if (!res.warning.isEmpty()) result["warning"] = res.warning;
    return result;
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

    auto* top = TransformOperator::getSingleton();
    OgreWidget* ogreWidget = top ? top->getActiveWidget() : nullptr;
    if (!ogreWidget)
        ogreWidget = m_mainWindow->findChild<OgreWidget*>();
    if (!ogreWidget) {
        // Keep the established error string (an existing test asserts it).
        return makeErrorResult("Error: OgreWidget not found");
    }
    if (!ogreWidget->getSpaceCamera() || !ogreWidget->getSpaceCamera()->getCamera()) {
        return makeErrorResult("Error: No active viewport camera");
    }
    Ogre::Camera* cam = ogreWidget->getSpaceCamera()->getCamera();

    // Ogre renders straight to the native window surface (WA_PaintOnScreen), so
    // QWidget::grab() returns an empty/black buffer. Capture the ACTUAL viewport
    // camera into an offscreen RTT and read that back — the same path the
    // isometric/turntable renderers use. Size follows the widget (or args).
    int width = args.contains("width") ? args["width"].toInt()
                                       : std::max(16, ogreWidget->width());
    int height = args.contains("height") ? args["height"].toInt()
                                         : std::max(16, ogreWidget->height());
    width = std::clamp(width, 16, 4096);
    height = std::clamp(height, 16, 4096);

    // State touched during the capture that MUST be restored on EVERY exit path
    // (success OR exception) so a throw can't leak a temp light / boosted ambient
    // / altered camera aspect / leftover RTT into the live scene (CodeRabbit).
    Ogre::SceneManager* sm = cam->getSceneManager();
    const Ogre::ColourValue savedAmbient = sm ? sm->getAmbientLight()
                                              : Ogre::ColourValue::Black;
    const Ogre::Real prevAspect = cam->getAspectRatio();
    Ogre::Light* capLight = nullptr;
    Ogre::SceneNode* capLightNode = nullptr;
    Ogre::TexturePtr rtt;
    auto cleanup = [&]() {
        cam->setAspectRatio(prevAspect);
        if (sm) {
            sm->setAmbientLight(savedAmbient);
            if (capLightNode) { capLightNode->detachAllObjects();
                sm->getRootSceneNode()->removeAndDestroyChild(capLightNode); capLightNode = nullptr; }
            if (capLight) { sm->destroyLight(capLight); capLight = nullptr; }
        }
        if (rtt) { Ogre::TextureManager::getSingleton().remove(rtt); rtt.reset(); }
        // The capture light changed the scene's light set — regenerate the
        // cached RTSS programs so the LIVE viewport goes back to rendering
        // against its own lighting (mirror of the pre-capture invalidation).
        RTShaderHelper::invalidateShadergenScheme();
    };

    try {
        const std::string rttName = "MCPScreenshotRTT";
        if (auto existing = Ogre::TextureManager::getSingleton().getByName(rttName))
            Ogre::TextureManager::getSingleton().remove(existing);
        rtt = Ogre::TextureManager::getSingleton().createManual(
            rttName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
            Ogre::TEX_TYPE_2D, static_cast<Ogre::uint32>(width),
            static_cast<Ogre::uint32>(height), 0, Ogre::PF_BYTE_RGBA,
            Ogre::TU_RENDERTARGET);
        Ogre::RenderTarget* target = rtt->getBuffer()->getRenderTarget();
        OgreRenderTargetUtil::configureOffscreenRenderTarget(target);

        // The RTT sees the live scene, which may render dark without adequate
        // lighting (imported FBX materials come back black otherwise). Bump
        // ambient + add a temporary directional key light for the capture
        // (restored by cleanup()) — same recipe as the isometric renderer.
        if (sm) {
            sm->setAmbientLight(Ogre::ColourValue(0.6f, 0.6f, 0.6f));
            capLight = sm->createLight();
            capLight->setType(Ogre::Light::LT_DIRECTIONAL);
            capLight->setDiffuseColour(Ogre::ColourValue(0.9f, 0.9f, 0.9f));
            capLightNode = sm->getRootSceneNode()->createChildSceneNode();
            capLightNode->attachObject(capLight);
            capLightNode->setDirection(Ogre::Vector3(-0.3f, -0.5f, -0.8f).normalisedCopy(),
                                       Ogre::Node::TS_WORLD);
            // RTSS bakes the light configuration into its generated shaders.
            // Without this, materials whose shaders were cached BEFORE the
            // capture light existed render it as a no-op — the screenshot
            // comes back flat/ambient-only (the "dark model over MCP" bug).
            RTShaderHelper::invalidateShadergenScheme();
        }

        Ogre::Viewport* vp = target->addViewport(cam);
        vp->setClearEveryFrame(true);
        vp->setBackgroundColour(Ogre::ColourValue(0.16f, 0.16f, 0.16f));
        vp->setOverlaysEnabled(false);
        // Use the RTSS shadergen scheme + shadows so materials render lit (else
        // everything comes back an unlit black silhouette), matching the live
        // viewport and the isometric/turntable capture path.
        vp->setMaterialScheme(Ogre::MSN_SHADERGEN);
        vp->setShadowsEnabled(true);
        cam->setAspectRatio(static_cast<Ogre::Real>(width) / static_cast<Ogre::Real>(height));
        target->update();

        QImage image(width, height, QImage::Format_RGBA8888);
        Ogre::PixelBox pb(static_cast<Ogre::uint32>(width),
                          static_cast<Ogre::uint32>(height), 1,
                          Ogre::PF_BYTE_RGBA, image.bits());
        target->copyContentsToMemory(Ogre::Box(0, 0, width, height), pb,
                                     Ogre::RenderTarget::FB_AUTO);
        cleanup();

        if (!image.save(path))
            return makeErrorResult(QString("Error: Failed to save screenshot to: %1").arg(path));
        SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
            QStringLiteral("mcp screenshot %1x%2 -> %3").arg(width).arg(height).arg(path));
        return makeSuccessResult(QString("Screenshot saved to: %1 (%2x%3)")
            .arg(path).arg(width).arg(height));
    } catch (const Ogre::Exception& e) {
        cleanup();
        return makeErrorResult(QStringLiteral("Error capturing screenshot: %1")
            .arg(QString::fromStdString(e.getDescription())));
    }
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

    // Node-transform clip (#517): SceneManager-level, not an entity state.
    // Scrub the node clip's own AnimationState (enabling it so setTimePosition
    // poses the node). The entity name equals the animated scene-node name.
    if (auto* nam = NodeAnimationManager::instance();
        nam && nam->listClips().contains(animName)
        && nam->animatedNodes(animName).contains(entityName)) {
        if (!args["time"].isDouble())
            return makeErrorResult("Error: 'time' must be a number for node clips");
        const double t = args["time"].toDouble();
        // Reject non-finite / negative times: static_cast<int>(t*1000) below is
        // UB when the product overflows int, and a negative time is meaningless.
        // Mirrors the node-keyframe handlers' contract. (#517)
        if (!std::isfinite(t) || t < 0.0)
            return makeErrorResult("Error: 'time' must be a non-negative finite number");
        auto* mgr = Manager::getSingletonPtr();
        auto* scene = mgr ? mgr->getSceneMgr() : nullptr;
        if (!scene || !scene->hasAnimationState(animName.toStdString()))
            return makeErrorResult("Error: node clip state missing");
        auto* nstate = scene->getAnimationState(animName.toStdString());
        nstate->setEnabled(true);
        nstate->setTimePosition(static_cast<float>(t));
        // Keep the controller's selection + slider in sync so the GUI reflects it.
        AnimationControlController::instance()->selectAnimation(entityName, animName);
        // Clamp the ms slider value to int range so a large (finite) time can't
        // overflow the cast.
        const double ms = std::min(t * 1000.0, static_cast<double>(std::numeric_limits<int>::max()));
        AnimationControlController::instance()->setSliderValue(static_cast<int>(ms));
        QJsonObject c; c["ok"] = true; c["animation"] = animName; c["time"] = t; c["node_clip"] = true;
        return makeSuccessResult(QString::fromUtf8(QJsonDocument(c).toJson(QJsonDocument::Indented)));
    }

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

static Ogre::Entity* firstResolvedSelectedEntity()
{
    SelectionSet* sel = SelectionSet::getSingleton();
    const QList<Ogre::Entity*> resolved = sel ? sel->getResolvedEntities() : QList<Ogre::Entity*>{};
    return resolved.isEmpty() ? nullptr : resolved.first();
}

static bool mcpJsonIntValue(const QJsonValue& value, int* out, QString* err, const char* field)
{
    if (!value.isDouble()) {
        if (err)
            *err = QStringLiteral("Error: '%1' must be an integer.").arg(QLatin1String(field));
        return false;
    }
    const double d = value.toDouble();
    if (d < static_cast<double>(std::numeric_limits<int>::min())
        || d > static_cast<double>(std::numeric_limits<int>::max())
        || d != std::trunc(d)) {
        if (err)
            *err = QStringLiteral("Error: '%1' must be an integer.").arg(QLatin1String(field));
        return false;
    }
    *out = static_cast<int>(d);
    return true;
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

QJsonObject MCPServer::toolMotionInBetween(const QJsonObject &args)
{
    try {
        SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.in_between"),
            QStringLiteral("MCP motion_in_between"));

        Manager* mgr = Manager::getSingletonPtr();
        if (!mgr)
            return makeErrorResult("Error: Manager not available");

        const int gapFrames = args.value("gap_frames").toInt(0);
        if (gapFrames < 1)
            return makeErrorResult("Error: 'gap_frames' must be a positive integer.");

        QString entityName = args["entity_name"].toString();
        Ogre::Entity* entity = nullptr;
        QList<Ogre::Entity*> allEntities = mgr->getEntities();
        if (!entityName.isEmpty()) {
            for (auto* ent : allEntities)
                if (ent && QString::fromStdString(ent->getName()) == entityName) { entity = ent; break; }
            if (!entity)
                return makeErrorResult(QString("Error: Entity '%1' not found").arg(entityName));
        } else {
            for (auto* ent : allEntities)
                if (ent && ent->hasSkeleton()) { entity = ent; break; }
        }
        if (!entity || !entity->hasSkeleton())
            return makeErrorResult("Error: No entity with skeleton found");

        Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();
        if (!skel)
            return makeErrorResult("Error: No skeleton found");

        QString animName = args["animation_name"].toString();
        if (animName.isEmpty()) {
            if (skel->getNumAnimations() == 0)
                return makeErrorResult("Error: Skeleton has no animations");
            animName = QString::fromStdString(skel->getAnimation(0)->getName());
        } else if (!skel->hasAnimation(animName.toStdString())) {
            return makeErrorResult(QString("Error: Animation '%1' not found").arg(animName));
        }
        Ogre::Animation* anim = skel->getAnimation(animName.toStdString());

        const bool noModel = args.value("no_model").toBool(false);
        const float clipLen = anim ? anim->getLength() : 0.0f;
        const float t0 = args.contains("start_time")
            ? static_cast<float>(args.value("start_time").toDouble()) : 0.0f;
        const float t1 = args.contains("end_time")
            ? static_cast<float>(args.value("end_time").toDouble()) : clipLen;

        QString modelPath;
        if (!noModel) modelPath = MotionInbetween::ensureModelBlocking();

        const auto r = AnimationMerger::inbetweenAnimation(
            skel.get(), animName.toStdString(), t0, t1, gapFrames, modelPath, noModel);
        if (!r.ok)
            return makeErrorResult(QString("Error: %1").arg(r.error));

        entity->refreshAvailableAnimationState();
        // In --with-mcp mode the dope sheet / keyframe caches point into the
        // live skeleton; a keyframe insert can dangle them, so tell the
        // controller to drop its cached pointers + refresh.
        if (auto* acc = AnimationControlController::instance())
            acc->notifyExternalAnimationEdit();

        QString result = QString("In-betweened '%1' [%2..%3]: inserted %4 keyframes "
                                 "across %5 track(s) via %6")
            .arg(animName).arg(t0).arg(t1)
            .arg(r.keyframesInserted).arg(r.tracksAffected)
            .arg(r.usedModel ? "RMIB model" : "spline fallback");
        if (!r.usedModel && !r.fallbackReason.isEmpty())
            result += QString(" (%1)").arg(r.fallbackReason);
        return makeSuccessResult(result);

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Error: Ogre exception — %1").arg(e.getFullDescription().c_str()));
    } catch (std::exception& e) {
        return makeErrorResult(QString("Error: %1").arg(e.what()));
    }
}

QJsonObject MCPServer::toolGenerateMotion(const QJsonObject &args)
{
    try {
        SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.text_to_motion"),
            QStringLiteral("MCP generate_motion"));

        Manager* mgr = Manager::getSingletonPtr();
        if (!mgr) return makeErrorResult("Error: Manager not available");

        const QString prompt = args.value("prompt").toString();
        // #838: a variant_index selects an EXACT curated clip (parity with the
        // CLI --variant flag and the GUI picker), forcing the template path.
        // prompt is then optional; without either, we can't pick a clip.
        const bool hasVariant = args.contains("variant_index");
        const int variantIndex = hasVariant
            ? args.value("variant_index").toInt(-1) : -1;
        if (hasVariant && variantIndex < 0)
            return makeErrorResult("Error: 'variant_index' must be a non-negative integer.");
        if (!hasVariant && prompt.trimmed().isEmpty())
            return makeErrorResult("Error: 'prompt' is required (e.g. \"walking\") unless 'variant_index' is given.");

        QString entityName = args["entity_name"].toString();
        Ogre::Entity* entity = nullptr;
        for (auto* ent : mgr->getEntities()) {
            if (!ent || ent->getMovableType() != "Entity" || !ent->hasSkeleton()) continue;
            if (entityName.isEmpty()
                || QString::fromStdString(ent->getName()) == entityName) { entity = ent; break; }
        }
        if (!entity)
            return makeErrorResult(entityName.isEmpty()
                ? QString("Error: no skinned mesh found — text-to-motion needs a rigged skeleton")
                : QString("Error: skinned entity '%1' not found").arg(entityName));
        Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();

        const double duration = args.value("duration").toDouble(0.0);
        // A variant_index forces the template path (no model, no random match).
        const bool useModel = !hasVariant && args.value("model").toBool(false);

        // Acquire the canonical clip: EXPERIMENTAL trained model first (when
        // model:true), else the reliable TEMPLATE library (also the fallback).
        QString action, clipSource;
        std::vector<std::vector<std::array<float, 4>>> quats;
        int fps = 30; bool worldFrame = false;
        std::vector<std::array<float, 4>> cmuRest;
        std::vector<std::array<float, 3>> clipDirs;
        std::vector<float> clipRootY;          // #838 non-locomotion hip drop
        bool gotClip = false;

        if (useModel) {
            const QString mp = MotionGenerator::ensureModelBlocking();
            if (!mp.isEmpty()) {
                const auto mr = MotionGenerator::generate(prompt, mp,
                                                          MotionGenerator::vocabPath(), duration);
                if (mr.ok) {
                    action = mr.matchedAction; quats = mr.clip.quats; fps = mr.clip.fps;
                    worldFrame = mr.worldFrame; clipSource = QStringLiteral("model"); gotClip = true;
                    if (!mr.clip.restWorld.empty() && !mr.clip.restDir.empty()) {
                        // v5 models (#858) ship their canonical reference
                        // triple — same bind-referenced retarget as templates.
                        cmuRest = mr.clip.restWorld;
                        clipDirs = mr.clip.restDir;
                    } else {
                        // Legacy v4: borrow a template clip's reference
                        // directions so the retarget synthesizes a
                        // BIND-referenced base pose (no harvest from the
                        // rig's other animations).
                        clipDirs = MotionLibrary::referenceDirsForPrompt(prompt);
                    }
                }
            }
        }

        if (!gotClip) {
            const QString libPath = MotionLibrary::ensureLibraryBlocking();
            if (libPath.isEmpty())
                return makeErrorResult("Error: motion library unavailable (offline or download disabled)");
            MotionLibrary lib;
            if (!lib.loadFromFile(libPath))
                return makeErrorResult(QString("Error: %1").arg(lib.error()));
            int idx;
            if (hasVariant) {
                if (variantIndex >= lib.clipCount())
                    return makeErrorResult(QString("Error: variant_index %1 out of range (0..%2)")
                                               .arg(variantIndex).arg(lib.clipCount() - 1));
                idx = variantIndex;
                action = lib.clip(idx).action;
            } else {
                idx = lib.matchPrompt(prompt, &action);
                if (idx < 0) {
                    QString known; for (const QString& a : lib.actions()) known += " " + a;
                    return makeErrorResult(QString("Error: no motion matched \"%1\". Known actions:%2")
                                               .arg(prompt, known));
                }
            }
            const MotionLibrary::Clip& clip = lib.clip(idx);
            quats = clip.quats; fps = clip.fps;
            worldFrame = lib.isWorldFrame();
            cmuRest = clip.restWorld.empty() ? lib.cmuRestWorld()
                                             : clip.restWorld;
            clipDirs = clip.restDir;
            clipRootY = clip.rootY;
            clipSource = QStringLiteral("template");
            if (duration > 0.05) {
                const int want = std::max(2, int(duration * clip.fps));
                std::vector<std::vector<std::array<float,4>>> retimed(want);
                std::vector<float> retimedY;
                const bool hadY = static_cast<int>(clipRootY.size()) == clip.frames;
                if (hadY) retimedY.resize(want);
                for (int f = 0; f < want; ++f) {
                    const float src = (clip.frames - 1) * (float(f) / float(want - 1));
                    const int si = std::min(clip.frames - 1, int(src + 0.5f));
                    retimed[f] = quats[si];
                    if (hadY) retimedY[f] = clipRootY[si];
                }
                quats.swap(retimed);
                if (hadY) clipRootY.swap(retimedY);
            }
        }

        const std::string animName = ("generated_" + action).toStdString();
        // Auto-rigged (no prior animation) meshes that face −Z would walk
        // backward — detect facing from the mesh's foot region.
        const bool yaw180 = AnimationMerger::detectBackwardFacing(entity);
        const auto r = AnimationMerger::applyMotionClip(skel.get(), animName, quats, fps,
                                                        worldFrame, cmuRest,
                                                        /*refineWithModel=*/false,
                                                        /*refineStride=*/8, yaw180,
                                                        clipDirs,
                                                        clipSource == QStringLiteral("model"),
                                                        clipRootY,
                                                        args.value("vertical_descent").toBool(true)
                                                        && MotionLibrary::isVerticalDescentAction(action));
        if (!r.ok) return makeErrorResult(QString("Error: %1").arg(r.error));

        // #837 quality post-pass (ON by default): sparse-bake temporal
        // low-pass — removes retarget trembling. Runs before arm-space and
        // foot pinning so the pin targets stay exact.
        const int smoothFps = args.value("smooth_bake").toBool(true)
            ? args.value("smooth_fps").toInt(12) : 0;
        if (smoothFps > 0)
            AnimationMerger::smoothBakeAnimation(skel.get(), animName,
                                                 smoothFps, fps);

        // #838: ground crouch/kneel/work clips (plant the lowest foot on the
        // floor — fixes the "floating worker"). Descent actions only.
        if (args.value("vertical_descent").toBool(true)
            && MotionLibrary::isVerticalDescentAction(action))
            AnimationMerger::groundRootToFeet(skel.get(), animName);

        // #854: optional Mixamo-style arm-space post-process. Echo whether it
        // took effect so an MCP caller can tell the rig had no arm roles
        // (rather than silently getting an unadjusted clip).
        const double armSpace = args.value("arm_space").toDouble(0.0);
        bool armSpaceApplied = false;
        if (std::abs(armSpace) > 1e-4)
            armSpaceApplied = AnimationMerger::adjustArmSpace(
                skel.get(), animName, static_cast<float>(armSpace));

        // #856: foot-contact cleanup — ON by default (foot_pin:false opts out).
        int footPinSpans = -1;
        if (args.value("foot_pin").toBool(true)) {
            const auto fp = AnimationMerger::pinFeet(skel.get(), animName);
            footPinSpans = fp.ok ? fp.spans : -1;
        }

        entity->refreshAvailableAnimationState();
        // Exclusively enable the generated clip — enabled states BLEND in
        // Ogre, and mixing with the import's auto-enabled animation renders
        // as a shaking mid-pose (same fix as the GUI generate path).
        if (auto* animSet = entity->getAllAnimationStates()) {
            for (const auto& [key, state] : animSet->getAnimationStates())
                state->setEnabled(false);
            if (animSet->hasAnimationState(animName)) {
                auto* gen = animSet->getAnimationState(animName);
                gen->setEnabled(true);
                gen->setLoop(true);
                gen->setTimePosition(0.0f);
            }
        }
        if (auto* acc = AnimationControlController::instance())
            acc->notifyExternalAnimationEdit();

        // Optional re-export.
        const QString outPath = args.value("output_path").toString();
        if (!outPath.isEmpty()) {
            auto* node = entity->getParentSceneNode();
            const int rc = MeshImporterExporter::exporter(
                node, outPath, CLIPipeline::formatForExtension(outPath));
            if (rc != 0)
                return makeErrorResult(QString("Error: applied motion but export to %1 failed").arg(outPath));
        }

        QJsonObject content;
        content["ok"] = true;
        content["prompt"] = prompt; content["action"] = action; content["source"] = clipSource;
        content["animation"] = QString::fromStdString(animName);
        content["frames"] = r.frames; content["length"] = r.length;
        content["tracks_written"] = r.tracksWritten; content["canonical_joints"] = r.canonicalJoints;
        content["entity"] = QString::fromStdString(entity->getName());
        if (std::abs(armSpace) > 1e-4) content["arm_space_applied"] = armSpaceApplied;
        if (footPinSpans >= 0) content["foot_pin_spans"] = footPinSpans;
        if (!outPath.isEmpty()) content["exported"] = outPath;
        return makeSuccessResult(
            QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Error: Ogre exception — %1").arg(e.getFullDescription().c_str()));
    } catch (std::exception& e) {
        return makeErrorResult(QString("Error: %1").arg(e.what()));
    }
}

QJsonObject MCPServer::toolAdjustArmSpace(const QJsonObject &args)
{
    try {
        SentryReporter::addBreadcrumb(QStringLiteral("ai.tool_call"),
            QStringLiteral("MCP adjust_arm_space"));

        Manager* mgr = Manager::getSingletonPtr();
        if (!mgr) return makeErrorResult("Error: Manager not available");

        const QString animName = args.value("animation_name").toString();
        if (animName.isEmpty())
            return makeErrorResult("Error: animation_name is required.");
        const double degrees = args.value("arm_space").toDouble(
            args.value("degrees").toDouble(0.0));

        const QString entityName = args.value("entity_name").toString();
        Ogre::Entity* entity = nullptr;
        for (auto* ent : mgr->getEntities()) {
            if (!ent || ent->getMovableType() != "Entity" || !ent->hasSkeleton())
                continue;
            if (entityName.isEmpty()
                || QString::fromStdString(ent->getName()) == entityName) {
                entity = ent; break;
            }
        }
        if (!entity)
            return makeErrorResult(entityName.isEmpty()
                ? QString("Error: no skinned mesh found to adjust.")
                : QString("Error: skinned entity '%1' not found.").arg(entityName));

        // Edit the mesh's MASTER skeleton, not the entity's SkeletonInstance:
        // the exporter serializes the master (same as every other animation-
        // edit tool), so editing the instance would return success while the
        // written file kept the unadjusted clip. Animations are shared between
        // master and instance, so the live viewport still updates.
        Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();
        const std::string an = animName.toStdString();
        if (!skel || !skel->hasAnimation(an))
            return makeErrorResult(
                QString("Error: animation '%1' not found on entity.").arg(animName));

        if (!AnimationMerger::adjustArmSpace(skel.get(), an,
                                             static_cast<float>(degrees)))
            return makeErrorResult(
                "Error: arm-space adjustment failed (no arm roles on this rig).");

        if (auto* acc = AnimationControlController::instance())
            acc->notifyExternalAnimationEdit();

        const QString outPath = args.value("output_path").toString();
        if (!outPath.isEmpty()) {
            auto* node = entity->getParentSceneNode();
            if (MeshImporterExporter::exporter(
                    node, outPath, CLIPipeline::formatForExtension(outPath)) != 0)
                return makeErrorResult(
                    QString("Error: adjusted arm space but export to %1 failed")
                        .arg(outPath));
        }

        QJsonObject content;
        content["ok"] = true;
        content["animation"] = animName;
        content["arm_space"] = degrees;
        content["entity"] = QString::fromStdString(entity->getName());
        if (!outPath.isEmpty()) content["exported"] = outPath;
        return makeSuccessResult(
            QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Error: Ogre exception — %1").arg(e.getFullDescription().c_str()));
    } catch (std::exception& e) {
        return makeErrorResult(QString("Error: %1").arg(e.what()));
    }
}

QJsonObject MCPServer::toolPinFeet(const QJsonObject &args)
{
    try {
        SentryReporter::addBreadcrumb(QStringLiteral("ai.tool_call"),
            QStringLiteral("MCP pin_feet"));

        Manager* mgr = Manager::getSingletonPtr();
        if (!mgr) return makeErrorResult("Error: Manager not available");

        const QString animName = args.value("animation_name").toString();
        if (animName.isEmpty())
            return makeErrorResult("Error: animation_name is required.");

        const QString entityName = args.value("entity_name").toString();
        Ogre::Entity* entity = nullptr;
        for (auto* ent : mgr->getEntities()) {
            if (!ent || ent->getMovableType() != "Entity" || !ent->hasSkeleton())
                continue;
            if (entityName.isEmpty()
                || QString::fromStdString(ent->getName()) == entityName) {
                entity = ent; break;
            }
        }
        if (!entity)
            return makeErrorResult(entityName.isEmpty()
                ? QString("Error: no skinned mesh found.")
                : QString("Error: skinned entity '%1' not found.").arg(entityName));

        // Edit the mesh's MASTER skeleton (exporter serializes the master;
        // animations are shared, so the live viewport still updates).
        Ogre::SkeletonPtr skel = entity->getMesh()->getSkeleton();
        const std::string an = animName.toStdString();
        if (!skel || !skel->hasAnimation(an))
            return makeErrorResult(
                QString("Error: animation '%1' not found on entity.").arg(animName));

        const auto fp = AnimationMerger::pinFeet(skel.get(), an);
        if (!fp.ok)
            return makeErrorResult(
                QString("Error: foot-pin failed: %1").arg(fp.error));

        if (auto* acc = AnimationControlController::instance())
            acc->notifyExternalAnimationEdit();

        const QString outPath = args.value("output_path").toString();
        if (!outPath.isEmpty()) {
            auto* node = entity->getParentSceneNode();
            if (MeshImporterExporter::exporter(
                    node, outPath, CLIPipeline::formatForExtension(outPath)) != 0)
                return makeErrorResult(
                    QString("Error: pinned feet but export to %1 failed")
                        .arg(outPath));
        }

        QJsonObject content;
        content["ok"] = true;
        content["animation"] = animName;
        content["spans"] = fp.spans;
        content["keyframes_adjusted"] = fp.keyframesAdjusted;
        content["entity"] = QString::fromStdString(entity->getName());
        if (!outPath.isEmpty()) content["exported"] = outPath;
        return makeSuccessResult(
            QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Error: Ogre exception — %1").arg(e.getFullDescription().c_str()));
    } catch (std::exception& e) {
        return makeErrorResult(QString("Error: %1").arg(e.what()));
    }
}

QJsonObject MCPServer::toolSegmentMesh(const QJsonObject &args)
{
    try {
        SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.segment"),
            QStringLiteral("MCP segment_mesh"));
        QElapsedTimer segmentTimer;
        segmentTimer.start();

        Manager* mgr = Manager::getSingletonPtr();
        if (!mgr) return makeErrorResult("Error: Manager not available");

        QString entityName = args["entity_name"].toString();
        Ogre::Entity* entity = nullptr;
        for (auto* ent : mgr->getEntities()) {
            if (!ent || ent->getMovableType() != "Entity") continue;
            if (entityName.isEmpty()
                || QString::fromStdString(ent->getName()) == entityName) { entity = ent; break; }
        }
        if (!entity) {
            return makeErrorResult(entityName.isEmpty()
                ? QString("Error: No mesh entity found")
                : QString("Error: Entity '%1' not found").arg(entityName));
        }

        std::vector<float> verts;
        std::vector<uint32_t> indices;
        if (!AutoRig::gatherGeometry(entity, verts, indices) || verts.empty())
            return makeErrorResult("Error: no readable geometry");
        const int vertexCount = static_cast<int>(verts.size() / 3);
        std::vector<int> rigLabels =
            AutoRig::rigPriorPartLabels(entity, vertexCount);

        const bool noModel = args.value("no_model").toBool(false);

        MeshSegmenter::Options opts;
        opts.forceFallback = noModel;
        opts.cleanupIslands = !args.value("no_cleanup").toBool(false);  // #863 opt-out
        const QString upAxisStr = args.value("up_axis").toString().toLower();
        if (!upAxisStr.isEmpty()) {
            if (upAxisStr == "x") opts.upAxis = 0;
            else if (upAxisStr == "y") opts.upAxis = 1;
            else if (upAxisStr == "z") opts.upAxis = 2;
            else return makeErrorResult("Error: up_axis must be 'x', 'y', or 'z'");
        }
        const QString categoryStr = args.value("category").toString();
        QString requestedCategory = categoryStr.isEmpty() ? QStringLiteral("auto") : categoryStr.toLower();
        if (!categoryStr.isEmpty()) {
            bool okCat = false;
            opts.category = MeshSegmenter::categoryFromName(categoryStr, &okCat);
            if (!okCat)
                return makeErrorResult("Error: category must be 'auto', 'body', "
                                       "'vegetation', 'vehicle', or 'building'");
        }
        SentryReporter::captureTelemetryEvent(QStringLiteral("segmentation.started"),
            QJsonObject{{QStringLiteral("source_surface"), QStringLiteral("mcp")},
                        {QStringLiteral("requested_category"), requestedCategory},
                        {QStringLiteral("automatic"), categoryStr.isEmpty() || requestedCategory == QStringLiteral("auto")},
                        {QStringLiteral("manual"), !(categoryStr.isEmpty() || requestedCategory == QStringLiteral("auto"))},
                        {QStringLiteral("capability"), QStringLiteral("segmentation")}});

        // Auto → classifier (first-use download); offline/no_model → body.
        if (!noModel)
            opts.category = MeshSegmenter::resolveCategoryBlocking(
                verts.data(), vertexCount, opts);
        else if (opts.category == MeshSegmenter::Category::Auto)
            opts.category = MeshSegmenter::Category::Body;

        QString modelPath;
        if (!noModel) modelPath = MeshSegmenter::ensureModelBlocking(opts.category);

        const MeshSegmenter::Result r = MeshSegmenter::predict(
            verts.data(), vertexCount, indices.data(),
            static_cast<int>(indices.size()), modelPath, opts,
            rigLabels.empty() ? nullptr : rigLabels.data());
        if (!r.ok) {
            SentryReporter::captureTelemetryEvent(QStringLiteral("segmentation.failed"),
                QJsonObject{{QStringLiteral("source_surface"), QStringLiteral("mcp")},
                            {QStringLiteral("requested_category"), requestedCategory},
                            {QStringLiteral("resolved_category"), MeshSegmenter::categoryName(opts.category)},
                            {QStringLiteral("automatic"), categoryStr.isEmpty() || requestedCategory == QStringLiteral("auto")},
                            {QStringLiteral("manual"), !(categoryStr.isEmpty() || requestedCategory == QStringLiteral("auto"))},
                            {QStringLiteral("ai"), !noModel},
                            {QStringLiteral("geometric_fallback"), noModel},
                            {QStringLiteral("duration_ms"), segmentTimer.elapsed()},
                            {QStringLiteral("success"), false},
                            {QStringLiteral("failure_category"), SentryReporter::sanitizedErrorCategory(r.error)},
                            {QStringLiteral("capability"), QStringLiteral("segmentation")}},
                QStringLiteral("error"));
            return makeErrorResult(QString("Error: %1")
                .arg(r.error.isEmpty() ? QStringLiteral("segmentation failed") : r.error));
        }

        const int P = MeshSegmenter::partCount();
        std::vector<int> vCount(P, 0);
        for (int l : r.vertexLabels) if (l >= 0 && l < P) ++vCount[l];
        std::vector<int> fCount(P, 0);
        for (int l : r.faceLabels) if (l >= 0 && l < P) ++fCount[l];

        int resultPartCount = 0;
        for (int p = 0; p < P; ++p)
            if (vCount[p] > 0 || fCount[p] > 0)
                ++resultPartCount;
        SentryReporter::captureTelemetryEvent(QStringLiteral("segmentation.completed"),
            QJsonObject{{QStringLiteral("source_surface"), QStringLiteral("mcp")},
                        {QStringLiteral("requested_category"), requestedCategory},
                        {QStringLiteral("resolved_category"), MeshSegmenter::categoryName(r.category)},
                        {QStringLiteral("automatic"), categoryStr.isEmpty() || requestedCategory == QStringLiteral("auto")},
                        {QStringLiteral("manual"), !(categoryStr.isEmpty() || requestedCategory == QStringLiteral("auto"))},
                        {QStringLiteral("ai"), r.usedModel},
                        {QStringLiteral("geometric_fallback"), !r.usedModel},
                        {QStringLiteral("result_part_count"), resultPartCount},
                        {QStringLiteral("duration_ms"), segmentTimer.elapsed()},
                        {QStringLiteral("success"), true},
                        {QStringLiteral("capability"), QStringLiteral("segmentation")}});

        QString summary = QString("Segmented '%1' (%2 verts, category %3) via %4:")
            .arg(QString::fromStdString(entity->getName())).arg(vertexCount)
            .arg(MeshSegmenter::categoryName(r.category))
            .arg(r.usedModel ? "model" : "geometric fallback");
        for (int p = 0; p < P; ++p)
            if (vCount[p] > 0)
                summary += QString(" %1=%2").arg(MeshSegmenter::partName(p)).arg(vCount[p]);
        if (!r.usedModel && !r.fallbackReason.isEmpty())
            summary += QString(" (%1)").arg(r.fallbackReason);

        // Return the actual label map so callers can act on it (assign a
        // material to a part, drive a selection, etc.) rather than parse prose.
        QJsonObject content;
        content["ok"]              = true;
        content["entity"]          = QString::fromStdString(entity->getName());
        content["vertex_count"]    = vertexCount;
        content["face_count"]      = static_cast<int>(r.faceLabels.size());
        content["used_model"]      = r.usedModel;
        content["category"]        = MeshSegmenter::categoryName(r.category);
        if (!r.usedModel && !r.fallbackReason.isEmpty())
            content["fallback_reason"] = r.fallbackReason;
        content["up_axis"]         = QString(QChar("xyz"[opts.upAxis]));
        content["summary"]         = summary;

        QJsonArray parts, perVert, perFace;
        for (int p = 0; p < P; ++p) {
            QJsonObject part;
            part["index"]        = p;
            part["name"]         = MeshSegmenter::partName(p);
            part["vertex_count"] = vCount[p];
            part["face_count"]   = fCount[p];
            parts.append(part);
        }
        content["parts"] = parts;

        // Per-face labels (index into `parts`) — the primary actionable output
        // for face-level part selection / per-part material assignment.
        QJsonArray faceLabels;
        for (int l : r.faceLabels) faceLabels.append(l);
        content["face_labels"] = faceLabels;

        return makeSuccessResult(
            QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Error: Ogre exception — %1").arg(e.getFullDescription().c_str()));
    } catch (std::exception& e) {
        return makeErrorResult(QString("Error: %1").arg(e.what()));
    }
}

QJsonObject MCPServer::toolSplitMeshBySegments(const QJsonObject &args)
{
    // PartOps split (#859/#861/#864): segment the selected/named entity and
    // replace it with one submesh per detected part, via the SAME undoable
    // SplitMeshCommand the GUI button uses (so Ctrl+Z / undo works identically).
    try {
        Manager* mgr = Manager::getSingletonPtr();
        if (!mgr) return makeErrorResult("Error: Manager not available");

        const QString entityName = args["entity_name"].toString();
        Ogre::Entity* entity = nullptr;
        for (auto* ent : mgr->getEntities()) {
            if (!ent || ent->getMovableType() != "Entity") continue;
            if (entityName.isEmpty()
                || QString::fromStdString(ent->getName()) == entityName) { entity = ent; break; }
        }
        if (!entity)
            return makeErrorResult(entityName.isEmpty()
                ? QString("Error: No mesh entity found")
                : QString("Error: Entity '%1' not found").arg(entityName));

        int axis = 1;
        const QString upAxisStr = args.value("up_axis").toString().toLower();
        if (upAxisStr == "x") axis = 0;
        else if (upAxisStr == "z") axis = 2;
        else if (!upAxisStr.isEmpty() && upAxisStr != "y")
            return makeErrorResult("Error: up_axis must be 'x', 'y', or 'z'");

        const QString category = args.value("category").toString().isEmpty()
            ? QStringLiteral("auto") : args.value("category").toString();
        const bool noModel = args.value("no_model").toBool(false);
        const bool solidify = args.value("solidify").toBool(false);

        SentryReporter::addBreadcrumb(QStringLiteral("mesh.parts.split_segments"),
                                      QStringLiteral("MCP split_mesh_by_segments"));

        // Capture the entity NAME before push(): push runs redo() synchronously,
        // which destroys this Ogre::Entity (mesh swap). Reading entity->getName()
        // after would dereference the freed pointer (CodeRabbit Critical).
        const QString entityNameOut = QString::fromStdString(entity->getName());
        auto* cmd = new SplitMeshCommand(entity->getName(), axis, category, noModel,
                                         QStringLiteral("Body"), solidify);
        UndoManager::getSingleton()->push(cmd); // runs redo() synchronously
        if (!cmd->ok())
            return makeErrorResult(cmd->error().isEmpty()
                ? QString("Error: split failed") : ("Error: " + cmd->error()));

        QJsonObject o;
        o["entity"] = entityNameOut;
        o["createdSubMeshes"] = cmd->createdSubMeshes();
        QJsonArray names;
        for (const QString& n : cmd->partNames()) names.append(n);
        o["partNames"] = names;
        return makeSuccessResult(
            QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented)));
    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Error: Ogre exception — %1").arg(e.getFullDescription().c_str()));
    } catch (std::exception& e) {
        return makeErrorResult(QString("Error: %1").arg(e.what()));
    }
}

QJsonObject MCPServer::toolExplodeMeshParts(const QJsonObject &args)
{
    // PartOps explode (#862/#864): split an already-multi-part entity into one
    // scene node per part, offset outward — via the SAME undoable
    // ExplodePartsCommand the GUI button uses.
    try {
        Manager* mgr = Manager::getSingletonPtr();
        if (!mgr) return makeErrorResult("Error: Manager not available");

        const QString entityName = args["entity_name"].toString();
        Ogre::Entity* entity = nullptr;
        for (auto* ent : mgr->getEntities()) {
            if (!ent || ent->getMovableType() != "Entity" || !ent->getMesh()) continue;
            // Auto-pick (empty name) must skip mesh-less movables so it doesn't
            // grab a non-mesh entity and miss a valid multi-part one later in the
            // list (mirrors toolJoinMeshParts's filter).
            if (entityName.isEmpty()
                || QString::fromStdString(ent->getName()) == entityName) { entity = ent; break; }
        }
        if (!entity)
            return makeErrorResult(entityName.isEmpty()
                ? QString("Error: No mesh entity found")
                : QString("Error: Entity '%1' not found").arg(entityName));
        if (entity->getMesh()->getNumSubMeshes() < 2)
            return makeErrorResult("Error: mesh has a single part — split it into parts first");

        double distance = args.value("distance").toDouble(0.15);
        if (distance < 0.0) distance = 0.0;

        SentryReporter::addBreadcrumb(QStringLiteral("mesh.parts.explode"),
                                      QStringLiteral("MCP explode_mesh_parts"));
        const std::string entName = entity->getName();
        auto* cmd = new ExplodePartsCommand(entName, static_cast<float>(distance));
        UndoManager::getSingleton()->push(cmd); // runs redo() synchronously
        if (!cmd->ok())
            return makeErrorResult(cmd->error().isEmpty()
                ? QString("Error: explode failed") : ("Error: " + cmd->error()));

        QJsonObject o;
        o["explodedParts"] = cmd->createdParts();
        o["distance"] = distance;
        return makeSuccessResult(
            QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented)));
    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Error: Ogre exception — %1").arg(e.getFullDescription().c_str()));
    } catch (std::exception& e) {
        return makeErrorResult(QString("Error: %1").arg(e.what()));
    }
}

QJsonObject MCPServer::toolJoinMeshParts(const QJsonObject &args)
{
    // PartOps join (#862/#864): merge 2+ named part entities (world transforms
    // baked in) into one fused static mesh — via the undoable JoinPartsCommand.
    try {
        Manager* mgr = Manager::getSingletonPtr();
        if (!mgr) return makeErrorResult("Error: Manager not available");

        // Resolve the target entities: an explicit "entity_names" array, else
        // every mesh entity in the scene.
        std::vector<std::string> names;
        QString fusedBase;
        std::set<std::string> seen;   // reject duplicates (CodeRabbit)
        auto pushName = [&](const std::string& n) {
            if (seen.insert(n).second) {
                names.push_back(n);
                if (fusedBase.isEmpty())
                    fusedBase = QString::fromStdString(n) + QStringLiteral("_fused");
            }
        };
        const QJsonArray requested = args.value("entity_names").toArray();
        if (!requested.isEmpty()) {
            // Every requested name MUST resolve to a mesh entity — a typo would
            // otherwise silently join a subset while reporting success. A repeated
            // name would duplicate that entity's geometry AND break the undo
            // (JoinPartsCommand can't recreate two same-named source nodes).
            for (const QJsonValue& v : requested) {
                const QString want = v.toString();
                Ogre::Entity* found = nullptr;
                for (auto* ent : mgr->getEntities()) {
                    if (!ent || ent->getMovableType() != "Entity" || !ent->getMesh()) continue;
                    if (QString::fromStdString(ent->getName()) == want) { found = ent; break; }
                }
                if (!found)
                    return makeErrorResult(QString("Error: entity '%1' not found").arg(want));
                if (seen.count(found->getName()))
                    return makeErrorResult(QString("Error: entity '%1' listed more than once").arg(want));
                pushName(found->getName());
            }
        } else {
            for (auto* ent : mgr->getEntities()) {
                if (!ent || ent->getMovableType() != "Entity" || !ent->getMesh()) continue;
                pushName(ent->getName());
            }
        }
        if (names.size() < 2)
            return makeErrorResult("Error: need two or more part entities to join");

        const int partCount = static_cast<int>(names.size());
        SentryReporter::addBreadcrumb(QStringLiteral("mesh.parts.join"),
                                      QStringLiteral("MCP join_mesh_parts"));
        auto* cmd = new JoinPartsCommand(std::move(names), fusedBase);
        UndoManager::getSingleton()->push(cmd); // runs redo() synchronously
        if (!cmd->ok())
            return makeErrorResult(cmd->error().isEmpty()
                ? QString("Error: join failed") : ("Error: " + cmd->error()));

        QJsonObject o;
        o["joinedParts"] = partCount;
        o["createdSubMeshes"] = cmd->createdSubMeshes();
        return makeSuccessResult(
            QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented)));
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

namespace
{

bool parseMcpLightType(const QString& text, Ogre::Light::LightTypes& out)
{
    const QString lower = text.trimmed().toLower();
    if (lower == QStringLiteral("directional"))
    {
        out = Ogre::Light::LT_DIRECTIONAL;
        return true;
    }
    if (lower == QStringLiteral("point"))
    {
        out = Ogre::Light::LT_POINT;
        return true;
    }
    if (lower == QStringLiteral("spot") || lower == QStringLiteral("spotlight"))
    {
        out = Ogre::Light::LT_SPOTLIGHT;
        return true;
    }
    return false;
}

Ogre::ColourValue parseMcpColour(const QJsonValue& value)
{
    if (value.isString())
    {
        QColor colour(value.toString());
        if (colour.isValid())
            return Ogre::ColourValue(colour.redF(), colour.greenF(), colour.blueF(), colour.alphaF());
    }
    if (value.isArray())
    {
        const QJsonArray arr = value.toArray();
        if (arr.size() >= 3)
        {
            return Ogre::ColourValue(static_cast<float>(arr.at(0).toDouble()),
                                     static_cast<float>(arr.at(1).toDouble()),
                                     static_cast<float>(arr.at(2).toDouble()),
                                     arr.size() > 3 ? static_cast<float>(arr.at(3).toDouble()) : 1.0f);
        }
    }
    return Ogre::ColourValue::White;
}

} // namespace

QJsonObject MCPServer::toolCreateLight(const QJsonObject& args)
{
    const QString typeText = args.value(QStringLiteral("type")).toString();
    if (typeText.isEmpty())
        return makeErrorResult(QStringLiteral("Error: 'type' is required (directional|point|spot)."));

    Ogre::Light::LightTypes type = Ogre::Light::LT_POINT;
    if (!parseMcpLightType(typeText, type))
        return makeErrorResult(QStringLiteral("Error: Unknown light type '%1'.").arg(typeText));

    if (!args.contains(QStringLiteral("position")))
        return makeErrorResult(QStringLiteral("Error: 'position' is required ([x,y,z])."));

    const Ogre::Vector3 position = parseVector3(args.value(QStringLiteral("position")));
    Ogre::Vector3 direction(0.0f, -1.0f, 0.0f);
    if (args.contains(QStringLiteral("direction")))
        direction = parseVector3(args.value(QStringLiteral("direction")));

    auto* lights = LightManager::getSingleton();
    lights->tryConnectToManager();

    const bool setDirection =
        type == Ogre::Light::LT_DIRECTIONAL || type == Ogre::Light::LT_SPOTLIGHT;
    LightHandle handle = lights->createLightAt(
        type, LightManager::defaultBaseNameForType(type), position, direction, setDirection);
    if (!handle.isValid())
        return makeErrorResult(QStringLiteral("Error: Failed to create light."));

    if (args.contains(QStringLiteral("colour")) || args.contains(QStringLiteral("color")))
    {
        const QJsonValue colourValue =
            args.contains(QStringLiteral("colour")) ? args.value(QStringLiteral("colour"))
                                                    : args.value(QStringLiteral("color"));
        handle.light->setDiffuseColour(parseMcpColour(colourValue));
    }
    if (args.contains(QStringLiteral("intensity")))
        handle.light->setPowerScale(static_cast<Ogre::Real>(args.value(QStringLiteral("intensity")).toDouble(1.0)));
    if (args.contains(QStringLiteral("range")) && type != Ogre::Light::LT_DIRECTIONAL)
    {
        const float range = static_cast<float>(args.value(QStringLiteral("range")).toDouble(10.0));
        handle.light->setAttenuation(range, 1.0f, 0.0f, 0.0f);
    }
    if (args.contains(QStringLiteral("cone")) && type == Ogre::Light::LT_SPOTLIGHT)
    {
        const QJsonValue cone = args.value(QStringLiteral("cone"));
        float innerDeg = 30.0f;
        float outerDeg = 40.0f;
        if (cone.isArray())
        {
            const QJsonArray arr = cone.toArray();
            if (arr.size() >= 1)
                innerDeg = static_cast<float>(arr.at(0).toDouble(innerDeg));
            if (arr.size() >= 2)
                outerDeg = static_cast<float>(arr.at(1).toDouble(outerDeg));
        }
        else if (cone.isObject())
        {
            const QJsonObject obj = cone.toObject();
            innerDeg = static_cast<float>(obj.value(QStringLiteral("inner")).toDouble(innerDeg));
            outerDeg = static_cast<float>(obj.value(QStringLiteral("outer")).toDouble(outerDeg));
        }
        handle.light->setSpotlightRange(Ogre::Degree(innerDeg), Ogre::Degree(outerDeg), 1.0f);
    }

    SentryReporter::addBreadcrumb(QStringLiteral("scene.light.create"),
                                  QStringLiteral("MCP create %1").arg(handle.name));

    QJsonObject payload;
    payload.insert(QStringLiteral("name"), handle.name);
    payload.insert(QStringLiteral("type"), typeText);
    payload.insert(QStringLiteral("position"),
                   QJsonArray{position.x, position.y, position.z});
    return makeSuccessResult(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolDeleteLight(const QJsonObject& args)
{
    const QString name = args.value(QStringLiteral("name")).toString();
    if (name.isEmpty())
        return makeErrorResult(QStringLiteral("Error: 'name' is required."));

    auto* lights = LightManager::getSingleton();
    lights->tryConnectToManager();
    if (!lights->deleteLight(name))
        return makeErrorResult(QStringLiteral("Error: Light '%1' not found.").arg(name));

    SentryReporter::addBreadcrumb(QStringLiteral("scene.light.delete"),
                                  QStringLiteral("MCP delete %1").arg(name));
    return makeSuccessResult(QStringLiteral("Deleted light '%1'.").arg(name));
}

QJsonObject MCPServer::toolListLights(const QJsonObject& args)
{
    Q_UNUSED(args);
    auto* lights = LightManager::getSingleton();
    lights->tryConnectToManager();

    const QJsonObject payload = SceneLightsIO::documentToListJson(SceneLightsIO::captureFromScene());
    return makeSuccessResult(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolSetLightProperty(const QJsonObject& args)
{
    const QString name = args.value(QStringLiteral("name")).toString();
    const QString key = args.value(QStringLiteral("key")).toString().trimmed().toLower();
    if (name.isEmpty() || key.isEmpty())
        return makeErrorResult(QStringLiteral("Error: 'name' and 'key' are required."));
    if (!args.contains(QStringLiteral("value")))
        return makeErrorResult(QStringLiteral("Error: 'value' is required."));

    auto* lights = LightManager::getSingleton();
    lights->tryConnectToManager();
    LightHandle* handle = lights->findLight(name);
    if (!handle || !handle->isValid())
        return makeErrorResult(QStringLiteral("Error: Light '%1' not found.").arg(name));

    const QJsonValue value = args.value(QStringLiteral("value"));
    LightSnapshot snapshot = LightSnapshot::fromHandle(*handle);

    if (key == QStringLiteral("position"))
        handle->sceneNode->setPosition(parseVector3(value));
    else if (key == QStringLiteral("direction"))
        handle->sceneNode->setDirection(parseVector3(value));
    else if (key == QStringLiteral("colour") || key == QStringLiteral("color") || key == QStringLiteral("diffuse"))
        snapshot.diffuse = parseMcpColour(value);
    else if (key == QStringLiteral("intensity") || key == QStringLiteral("powerscale"))
        snapshot.powerScale = static_cast<float>(value.toDouble(snapshot.powerScale));
    else if (key == QStringLiteral("range") || key == QStringLiteral("attenuationrange"))
        snapshot.attenuationRange = static_cast<float>(value.toDouble(snapshot.attenuationRange));
    else if (key == QStringLiteral("enabled") || key == QStringLiteral("visible"))
        snapshot.enabled = value.toBool(snapshot.enabled);
    else if (key == QStringLiteral("castshadows") || key == QStringLiteral("shadows"))
        snapshot.castShadows = value.toBool(snapshot.castShadows);
    else if (key == QStringLiteral("cone") && snapshot.type == Ogre::Light::LT_SPOTLIGHT)
    {
        float innerDeg = snapshot.spotlightInnerAngleDeg;
        float outerDeg = snapshot.spotlightOuterAngleDeg;
        if (value.isArray())
        {
            const QJsonArray arr = value.toArray();
            if (arr.size() >= 1)
                innerDeg = static_cast<float>(arr.at(0).toDouble(innerDeg));
            if (arr.size() >= 2)
                outerDeg = static_cast<float>(arr.at(1).toDouble(outerDeg));
        }
        else if (value.isObject())
        {
            const QJsonObject obj = value.toObject();
            innerDeg = static_cast<float>(obj.value(QStringLiteral("inner")).toDouble(innerDeg));
            outerDeg = static_cast<float>(obj.value(QStringLiteral("outer")).toDouble(outerDeg));
        }
        snapshot.spotlightInnerAngleDeg = innerDeg;
        snapshot.spotlightOuterAngleDeg = outerDeg;
    }
    else
        return makeErrorResult(QStringLiteral("Error: Unsupported light property key '%1'.").arg(key));

    if (!lights->applyProperties(name, snapshot))
        return makeErrorResult(QStringLiteral("Error: Failed to set property on '%1'.").arg(name));

    SentryReporter::addBreadcrumb(QStringLiteral("scene.light.edit"),
                                  QStringLiteral("MCP set %1 on %2").arg(key, name));
    return makeSuccessResult(QStringLiteral("Updated '%1'.%2 = %3")
                                 .arg(name, key, QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("value"), value}}).toJson(QJsonDocument::Compact))));
}

QJsonObject MCPServer::toolApplyLightRig(const QJsonObject& args)
{
    QString rigId = args.value(QStringLiteral("name")).toString();
    if (rigId.isEmpty())
        rigId = args.value(QStringLiteral("rig_id")).toString();
    if (rigId.isEmpty())
        return makeErrorResult(QStringLiteral("Error: 'name' (rig id) is required."));

    const bool replaceExisting = args.value(QStringLiteral("replace_existing")).toBool(false);
    LightManager::getSingleton()->tryConnectToManager();

    const LightRigApplyResult result = LightRigLibrary::apply(rigId, replaceExisting);
    if (!result.ok)
        return makeErrorResult(result.error);

    SentryReporter::addBreadcrumb(QStringLiteral("scene.light.apply_rig"),
                                  QStringLiteral("MCP rig %1").arg(rigId));

    QJsonObject payload;
    payload.insert(QStringLiteral("rigId"), result.rigId);
    payload.insert(QStringLiteral("rigGroupNodeName"), result.rigGroupNodeName);
    payload.insert(QStringLiteral("addedLightCount"), result.addedLights.size());
    return makeSuccessResult(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Indented)));
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
// Vertex-anim B3 (#519) — Alembic import + vertex-clip playback.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Performance capture (epic #869, Slice D #873)
// ---------------------------------------------------------------------------

QJsonObject MCPServer::toolCaptureFaceFromVideo(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "capture_face_from_video");
#ifndef ENABLE_MOCAP
    Q_UNUSED(args);
    return makeErrorResult(
        "Error: this build has no performance-capture support. Rebuild with "
        "-DENABLE_MOCAP=ON -DENABLE_ONNX=ON.");
#else
    const QString videoPath = args.value("video_path").toString();
    if (videoPath.isEmpty())
        return makeErrorResult("Error: missing required 'video_path' argument");
    if (!QFileInfo::exists(videoPath))
        return makeErrorResult(QString("Error: video not found: %1").arg(videoPath));

    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr) return makeErrorResult("Error: Manager not available");

    const QString entityName = args.value("entity_name").toString();
    Ogre::Entity* entity = nullptr;
    if (entityName.isEmpty()) {
        // Schema says the target defaults to the selected entity — honour
        // SelectionSet, then fall back to the first entity in the scene.
        if (auto* sel = SelectionSet::getSingleton()) {
            const auto resolved = sel->getResolvedEntities();
            if (!resolved.isEmpty()) entity = resolved.first();
        }
    }
    if (!entity) {
        for (auto* ent : mgr->getEntities()) {
            if (!ent || ent->getMovableType() != "Entity") continue;
            if (entityName.isEmpty()
                || QString::fromStdString(ent->getName()) == entityName) { entity = ent; break; }
        }
    }
    if (!entity) {
        return makeErrorResult(entityName.isEmpty()
            ? QString("Error: No mesh entity found")
            : QString("Error: Entity '%1' not found").arg(entityName));
    }

    const QStringList targets =
        MorphAnimationManager::instance()->morphTargetsFor(entity);
    const bool head = args.value("head").toBool(true);
    if (targets.isEmpty() && !head)
        return makeErrorResult(
            "Error: the entity has no morph targets and head=false — nothing to record. "
            "Face capture drives ARKit-style blendshape targets (jawOpen, mouthSmileLeft, ...).");

    const FaceCapMapper::Mapping mapping =
        FaceCapMapper::build(targets, args.value("map_path").toString());
    if (!mapping.error.isEmpty())
        return makeErrorResult(QString("Error: %1").arg(mapping.error));

    if (!FaceCapPredictor::modelsPresent()
        && FaceCapPredictor::ensureModelsBlocking().isEmpty())
        return makeErrorResult(
            "Error: face capture models are not available (download failed, offline "
            "guard set, or not hosted yet). Set QTMESH_MOCAP_MODEL_BASE_URL or place "
            "the graphs in " + FaceCapPredictor::modelDir());
    auto predictor = std::make_shared<FaceCapPredictor>();
    if (!predictor->load())
        return makeErrorResult(QString("Error: %1").arg(predictor->lastError()));

    double fps = args.value("fps").toDouble(30.0);
    if (fps <= 0 || fps > 240) fps = 30.0;
    const bool smooth = args.value("smooth").toBool(true);

    auto source = std::make_shared<FileFrameSource>(videoPath, fps);
    QString openError;
    if (!source->open(&openError))
        return makeErrorResult(QString("Error: %1").arg(openError));

    auto samples = std::make_shared<std::vector<FaceSample>>();
    auto weightFilters = std::make_shared<std::array<OneEuroFilter, 52>>();
    auto headFilter = std::make_shared<OneEuroQuatFilter>();

    QEventLoop loop;
    bool finished = false;
    QString streamError;
    QObject::connect(source.get(), &VideoFrameSource::frameReady,
                     [&, predictor, samples, weightFilters, headFilter](const MocapFrame& frame) {
                         FaceSample s = predictor->predict(frame.image, frame.timeSec);
                         if (smooth && s.confidence > 0.f) {
                             for (int c = 0; c < 52; ++c)
                                 s.weights[c] = static_cast<float>(
                                     (*weightFilters)[c].filter(s.weights[c], s.timeSec));
                             s.headRotation = headFilter->filter(s.headRotation, s.timeSec);
                         }
                         samples->push_back(s);
                     });
    QObject::connect(source.get(), &VideoFrameSource::finished, [&] {
        finished = true; loop.quit();
    });
    QObject::connect(source.get(), &VideoFrameSource::errorOccurred,
                     [&](const QString& message) {
                         streamError = message; finished = true; loop.quit();
                     });
    source->start();
    if (!finished)
        loop.exec();
    source->stop();
    if (!streamError.isEmpty())
        return makeErrorResult(QString("Error: %1").arg(streamError));
    if (samples->empty())
        return makeErrorResult("Error: the video produced no frames");

    MocapRecorder::FaceRecordOptions options;
    options.clipName = args.value("clip_name").toString(QStringLiteral("FaceCap"));
    options.head = head;

    return runOgreOp([&]() -> QJsonObject {
        // ONE undoable step for the whole take
        auto* cmd = new RecordMocapClipCommand(entity->getName(), *samples,
                                               mapping, options);
        UndoManager::getSingleton()->push(cmd);
        const MocapRecorder::FaceRecordReport& report = cmd->report();
        if (!report.ok())
            return makeErrorResult(QString("Error: %1").arg(report.error));

        const QString outputPath = args.value("output_path").toString();
        if (!outputPath.isEmpty()) {
            Ogre::SceneNode* node = entity->getParentSceneNode();
            const QString fmt = CLIPipeline::formatForExtension(outputPath);
            if (!node || MeshImporterExporter::exporter(
                    node, QFileInfo(outputPath).absoluteFilePath(), fmt) != 0)
                return makeErrorResult(
                    QString("Error: recorded, but export to %1 failed").arg(outputPath));
        }

        GamificationManager::noteOperation(
            QStringLiteral("mocap_face"),
            {{QStringLiteral("frames"), static_cast<qint64>(report.framesProcessed)},
             {QStringLiteral("keyframes"),
              static_cast<qint64>(report.keyframesWritten + report.headKeyframesWritten)}},
            GamificationManager::Surface::Mcp);

        QJsonObject content;
        content["ok"] = true;
        content["clipName"] = report.clipName;
        content["framesProcessed"] = report.framesProcessed;
        content["framesNoFace"] = report.framesNoFace;
        content["keyframesWritten"] = report.keyframesWritten;
        content["headKeyframesWritten"] = report.headKeyframesWritten;
        content["headTarget"] = report.headTarget;
        content["clipLength"] = report.clipLength;
        content["matchedChannels"] = QJsonArray::fromStringList(report.matchedChannels);
        content["unmatchedCanonical"] = QJsonArray::fromStringList(report.unmatchedCanonical);
        content["unmatchedMesh"] = QJsonArray::fromStringList(report.unmatchedMesh);
        if (!outputPath.isEmpty())
            content["output"] = outputPath;
        return makeSuccessResult(
            QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
    });
#endif // ENABLE_MOCAP
}

QJsonObject MCPServer::toolListCaptureDevices(const QJsonObject &args)
{
    Q_UNUSED(args);
    SentryReporter::addBreadcrumb("ai.tool_call", "list_capture_devices");
#ifndef ENABLE_MOCAP
    return makeErrorResult(
        "Error: this build has no performance-capture support. Rebuild with "
        "-DENABLE_MOCAP=ON -DENABLE_ONNX=ON.");
#else
    QJsonArray devices;
    for (const auto& dev : CameraFrameSource::availableDevices()) {
        QJsonObject o;
        o["id"] = dev.id;
        o["description"] = dev.description;
        devices.append(o);
    }
    QJsonObject content;
    content["devices"] = devices;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
#endif
}

QJsonObject MCPServer::toolStartLiveCapture(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "start_live_capture");
#ifndef ENABLE_MOCAP
    Q_UNUSED(args);
    return makeErrorResult(
        "Error: this build has no performance-capture support. Rebuild with "
        "-DENABLE_MOCAP=ON -DENABLE_ONNX=ON.");
#else
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr || !mgr->getMainWindow())
        return makeErrorResult(
            "Error: live capture needs the GUI (run with --with-mcp, not --mcp).");
    auto* c = MocapController::instance();
    if (c->state() != MocapController::Idle)
        return makeErrorResult("Error: a live capture session is already running");

    // Optional channel toggles — set BEFORE starting so drivability is
    // evaluated with the requested channels (default: leave as-is).
    if (args.contains("face")) c->setFaceEnabled(args.value("face").toBool());
    if (args.contains("head")) c->setHeadEnabled(args.value("head").toBool());
    if (args.contains("body")) c->setBodyEnabled(args.value("body").toBool());

    // video_path drives from a file (the macOS-camera-blocked path); else the
    // webcam device_id (empty = default camera).
    const QString videoPath = args.value("video_path").toString();
    const bool started = videoPath.isEmpty()
        ? c->startPreview(args.value("device_id").toString())
        : c->startPreviewFromVideo(videoPath);
    if (!started)
        return makeErrorResult(QString("Error: %1").arg(c->statusMessage()));
    QJsonObject content;
    content["ok"] = true;
    content["state"] = "previewing";
    content["source"] = videoPath.isEmpty() ? "camera" : "video";
    content["matchedChannels"] = c->matchedChannelCount();
    content["headAvailable"] = c->headAvailable();
    content["bodyAvailable"] = c->bodyAvailable();
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
#endif
}

QJsonObject MCPServer::toolSetCaptureChannels(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "set_capture_channels");
#ifndef ENABLE_MOCAP
    Q_UNUSED(args);
    return makeErrorResult(
        "Error: this build has no performance-capture support. Rebuild with "
        "-DENABLE_MOCAP=ON -DENABLE_ONNX=ON.");
#else
    auto* c = MocapController::instance();
    if (args.contains("face")) c->setFaceEnabled(args.value("face").toBool());
    if (args.contains("head")) c->setHeadEnabled(args.value("head").toBool());
    if (args.contains("body")) c->setBodyEnabled(args.value("body").toBool());
    QJsonObject content;
    content["ok"] = true;
    content["face"] = c->faceEnabled();
    content["head"] = c->headEnabled();
    content["body"] = c->bodyEnabled();
    content["matchedChannels"] = c->matchedChannelCount();
    content["headAvailable"] = c->headAvailable();
    content["bodyAvailable"] = c->bodyAvailable();
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
#endif
}

QJsonObject MCPServer::toolStopLiveCapture(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "stop_live_capture");
#ifndef ENABLE_MOCAP
    Q_UNUSED(args);
    return makeErrorResult(
        "Error: this build has no performance-capture support. Rebuild with "
        "-DENABLE_MOCAP=ON -DENABLE_ONNX=ON.");
#else
    auto* c = MocapController::instance();
    if (c->state() == MocapController::Idle)
        return makeErrorResult("Error: no live capture session is running");
    if (args.value("record").toBool(false)
        && c->state() == MocapController::Recording)
        c->stopRecording();
    c->stopPreview();
    QJsonObject content;
    content["ok"] = true;
    content["status"] = c->statusMessage();
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
#endif
}

QJsonObject MCPServer::toolCaptureBodyFromVideo(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "capture_body_from_video");
#ifndef ENABLE_MOCAP
    Q_UNUSED(args);
    return makeErrorResult(
        "Error: this build has no performance-capture support. Rebuild with "
        "-DENABLE_MOCAP=ON -DENABLE_ONNX=ON.");
#else
    const QString videoPath = args.value("video_path").toString();
    if (videoPath.isEmpty())
        return makeErrorResult("Error: missing required 'video_path' argument");
    if (!QFileInfo::exists(videoPath))
        return makeErrorResult(QString("Error: video not found: %1").arg(videoPath));

    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr) return makeErrorResult("Error: Manager not available");

    const QString entityName = args.value("entity_name").toString();
    Ogre::Entity* entity = nullptr;
    if (entityName.isEmpty()) {
        // Schema says the target defaults to the selected entity — honour
        // SelectionSet, then fall back to the first entity in the scene.
        if (auto* sel = SelectionSet::getSingleton()) {
            const auto resolved = sel->getResolvedEntities();
            if (!resolved.isEmpty()) entity = resolved.first();
        }
    }
    if (!entity) {
        for (auto* ent : mgr->getEntities()) {
            if (!ent || ent->getMovableType() != "Entity") continue;
            if (entityName.isEmpty()
                || QString::fromStdString(ent->getName()) == entityName) { entity = ent; break; }
        }
    }
    if (!entity) {
        return makeErrorResult(entityName.isEmpty()
            ? QString("Error: No mesh entity found")
            : QString("Error: Entity '%1' not found").arg(entityName));
    }
    if (!entity->hasSkeleton())
        return makeErrorResult(
            "Error: the entity is not skinned — body capture retargets onto a "
            "humanoid skeleton (auto_rig with skin:true first).");

    if (!PoseCapPredictor::modelsPresent()
        && PoseCapPredictor::ensureModelsBlocking().isEmpty())
        return makeErrorResult(
            "Error: pose capture models are not available (download failed, offline "
            "guard set, or not hosted yet). Set QTMESH_MOCAP_MODEL_BASE_URL or place "
            "the graphs in " + PoseCapPredictor::modelDir());
    auto posePredictor = std::make_shared<PoseCapPredictor>();
    if (!posePredictor->load())
        return makeErrorResult(QString("Error: %1").arg(posePredictor->lastError()));

    double fps = args.value("fps").toDouble(30.0);
    if (fps <= 0 || fps > 240) fps = 30.0;
    const bool smooth = args.value("smooth").toBool(true);
    const QString algo = args.value("algo").toString(QStringLiteral("sam3dbody"));

    auto source = std::make_shared<FileFrameSource>(videoPath, fps);
    QString openError;
    if (!source->open(&openError))
        return makeErrorResult(QString("Error: %1").arg(openError));

    auto poseSamples = std::make_shared<std::vector<PoseSample>>();
    QEventLoop loop;
    bool finished = false;
    QString streamError;
    QObject::connect(source.get(), &VideoFrameSource::frameReady,
                     [&, posePredictor, poseSamples](const MocapFrame& frame) {
                         poseSamples->push_back(
                             posePredictor->predict(frame.image, frame.timeSec));
                     });
    QObject::connect(source.get(), &VideoFrameSource::finished, [&] {
        finished = true; loop.quit();
    });
    QObject::connect(source.get(), &VideoFrameSource::errorOccurred,
                     [&](const QString& message) {
                         streamError = message; finished = true; loop.quit();
                     });
    source->start();
    if (!finished)
        loop.exec();
    source->stop();
    if (!streamError.isEmpty())
        return makeErrorResult(QString("Error: %1").arg(streamError));

    std::vector<std::vector<std::array<float, 4>>> clipQuats;
    {
        std::array<OneEuroQuatFilter, PoseIK::kCanonicalRoles> roleFilters;
        PoseIK::Solver solver;
        for (const PoseSample& s : *poseSamples) {
            if (s.confidence <= 0.f)
                continue;
            PoseIK::FrameResult fr =
                solver.solveFrame(s.world.data(), s.visibility.data());
            if (smooth)
                for (int r = 0; r < PoseIK::kCanonicalRoles; ++r)
                    fr.quats[r] = roleFilters[r].filter(fr.quats[r], s.timeSec);
            clipQuats.push_back(std::vector<std::array<float, 4>>(
                fr.quats.begin(), fr.quats.end()));
        }
    }
    if (clipQuats.size() < 2)
        return makeErrorResult("Error: no person tracked in the video");

    MocapRecorder::BodyRecordOptions options;
    options.clipName = args.value("clip_name").toString(QStringLiteral("BodyCap"));
    options.algorithmUsed = QStringLiteral("pose-ik");
    options.fallbackReason = algo == QLatin1String("pose-ik")
        ? QString()
        : QStringLiteral("sam3dbody model not available (checkpoint access "
                         "pending — see THIRD_PARTY_AI_MODELS.md); used pose-ik");

    return runOgreOp([&]() -> QJsonObject {
        auto* cmd = new RecordBodyClipCommand(entity->getName(), clipQuats,
                                              static_cast<int>(fps), options);
        UndoManager::getSingleton()->push(cmd);
        MocapRecorder::BodyRecordReport report = cmd->report();
        report.framesProcessed = static_cast<int>(poseSamples->size());
        if (!report.ok())
            return makeErrorResult(QString("Error: %1").arg(report.error));

        const QString outputPath = args.value("output_path").toString();
        if (!outputPath.isEmpty()) {
            Ogre::SceneNode* node = entity->getParentSceneNode();
            const QString fmt = CLIPipeline::formatForExtension(outputPath);
            if (!node || MeshImporterExporter::exporter(
                    node, QFileInfo(outputPath).absoluteFilePath(), fmt) != 0)
                return makeErrorResult(
                    QString("Error: recorded, but export to %1 failed").arg(outputPath));
        }

        GamificationManager::noteOperation(
            QStringLiteral("mocap_body"),
            {{QStringLiteral("frames"), static_cast<qint64>(report.framesProcessed)},
             {QStringLiteral("tracks"), static_cast<qint64>(report.tracksWritten)}},
            GamificationManager::Surface::Mcp);

        QJsonObject content;
        content["ok"] = true;
        content["clipName"] = report.clipName;
        content["algorithmUsed"] = report.algorithmUsed;
        if (!report.fallbackReason.isEmpty())
            content["fallbackReason"] = report.fallbackReason;
        content["framesProcessed"] = report.framesProcessed;
        content["rolesResolved"] = report.rolesResolved;
        content["tracksWritten"] = report.tracksWritten;
        content["clipLength"] = report.clipLength;
        if (!outputPath.isEmpty())
            content["output"] = outputPath;
        return makeSuccessResult(
            QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
    });
#endif // ENABLE_MOCAP
}

QJsonObject MCPServer::toolImportAlembic(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "import_alembic");

    const QString filePath = args.value("file").toString();
    if (filePath.isEmpty())
        return makeErrorResult("Error: missing required 'file' argument");
    if (!QFileInfo::exists(filePath))
        return makeErrorResult(QString("Error: file not found: %1").arg(filePath));
    if (!AlembicImporter::available())
        return makeErrorResult(
            "Error: this build has no Alembic support. Rebuild with -DENABLE_ALEMBIC=ON.");

    SentryReporter::addBreadcrumb("file.import",
        QString("Importing Alembic cache %1").arg(filePath));

    // importToScene creates scene nodes / entities, which can throw
    // Ogre::Exception — run through runOgreOp so a failure returns a clean MCP
    // error instead of taking down the server (matches the other Ogre tools).
    return runOgreOp([&]() -> QJsonObject {
        QString err;
        Ogre::SceneNode* node = AlembicImporter::importToScene(filePath, &err);
        if (!node)
            return makeErrorResult(
                err.isEmpty() ? QString("Error: failed to import %1").arg(filePath) : err);

        // Report the node + the vertex clips the import produced so the agent
        // can immediately drive play_vertex_animation.
        QJsonObject content;
        content["ok"] = true;
        content["file"] = filePath;
        content["node"] = QString::fromStdString(node->getName());

        QStringList entities, clips;
        for (unsigned short i = 0; i < node->numAttachedObjects(); ++i) {
            Ogre::MovableObject* obj = node->getAttachedObject(i);
            if (!obj || obj->getMovableType() != "Entity") continue;
            auto* ent = static_cast<Ogre::Entity*>(obj);
            entities.append(QString::fromStdString(ent->getName()));
            if (auto* m = VertexAnimationManager::instance()) {
                for (const QString& c : m->vertexClipsFor(ent))
                    if (!clips.contains(c)) clips.append(c);
            }
        }
        QJsonArray entArr, clipArr;
        for (const QString& e : entities) entArr.append(e);
        for (const QString& c : clips) clipArr.append(c);
        content["entities"] = entArr;
        content["vertexClips"] = clipArr;
        return makeSuccessResult(
            QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
    });
}

QJsonObject MCPServer::toolPlayVertexAnimation(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "play_vertex_animation");
    // A vertex clip surfaces as an ordinary AnimationState, so playback is
    // identical to play_animation — delegate to keep one code path.
    return toolPlayAnimation(args);
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
// Node-anim: playback + editing parity (all-animation-controls-via-MCP).
// These complete the node-transform surface so an agent can do everything
// the Inspector's "Node Transform Animation" section can: play, delete,
// re-time / delete keyframes, and inspect a clip.
// ---------------------------------------------------------------------------

QJsonObject MCPServer::toolSetNodeAnimationPlaying(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "set_node_animation_playing");
    const QString clip = args.value("clip").toString();
    if (clip.isEmpty())
        return makeErrorResult("Error: missing required 'clip' argument");
    if (!args.value("enabled").isBool())
        return makeErrorResult("Error: 'enabled' must be a boolean");
    const bool enabled = args.value("enabled").toBool();
    auto* m = NodeAnimationManager::instance();
    if (!m || !m->setClipEnabled(clip, enabled))
        return makeErrorResult(QString("Error: clip '%1' not found").arg(clip));
    // Node clips are SceneManager-level states, advanced by
    // MainWindow::frameRenderingQueued ONLY while the global transport is
    // playing. Enabling the state alone leaves a node-only scene frozen — start
    // the transport, mirroring the skeletal play_animation handler. (#517)
    if (enabled) {
        if (MainWindow* mainWindow = qobject_cast<MainWindow*>(m_mainWindow))
            mainWindow->setPlaying(true);
    }
    QJsonObject content;
    content["ok"] = true;
    content["clip"] = clip;
    content["enabled"] = enabled;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolDeleteNodeAnimationClip(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "delete_node_animation_clip");
    const QString clip = args.value("clip").toString();
    if (clip.isEmpty())
        return makeErrorResult("Error: missing required 'clip' argument");
    auto* m = NodeAnimationManager::instance();
    if (!m->deleteClip(clip))
        return makeErrorResult(QString("Error: clip '%1' not found").arg(clip));
    QJsonObject content;
    content["ok"] = true;
    content["clip"] = clip;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolMoveNodeKeyframe(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "move_node_keyframe");
    const QString clip = args.value("clip").toString();
    const QString node = args.value("node").toString();
    if (clip.isEmpty() || node.isEmpty())
        return makeErrorResult("Error: 'clip' and 'node' are required");
    if (!args.value("old_time").isDouble() || !args.value("new_time").isDouble())
        return makeErrorResult("Error: 'old_time' and 'new_time' must be numbers");
    const double oldT = args.value("old_time").toDouble();
    const double newT = args.value("new_time").toDouble();
    if (!std::isfinite(oldT) || !std::isfinite(newT) || newT < 0.0)
        return makeErrorResult("Error: times must be finite and new_time >= 0");
    auto* m = NodeAnimationManager::instance();
    if (!m->moveNodeKeyframe(clip, node, oldT, newT))
        return makeErrorResult(
            "Error: move rejected (no key at old_time, a key already at new_time, "
            "clip/node missing, or new_time out of range)");
    QJsonObject content;
    content["ok"] = true;
    content["clip"] = clip;
    content["node"] = node;
    content["old_time"] = oldT;
    content["new_time"] = newT;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolDeleteNodeKeyframe(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "delete_node_keyframe");
    const QString clip = args.value("clip").toString();
    const QString node = args.value("node").toString();
    if (clip.isEmpty() || node.isEmpty())
        return makeErrorResult("Error: 'clip' and 'node' are required");
    if (!args.value("time").isDouble())
        return makeErrorResult("Error: 'time' must be a number");
    const double t = args.value("time").toDouble();
    if (!std::isfinite(t))
        return makeErrorResult("Error: 'time' must be finite");
    auto* m = NodeAnimationManager::instance();
    if (!m->deleteNodeKeyframe(clip, node, t))
        return makeErrorResult(
            QString("Error: no keyframe near %1s on clip '%2' node '%3'")
                .arg(t, 0, 'f', 3).arg(clip, node));
    QJsonObject content;
    content["ok"] = true;
    content["clip"] = clip;
    content["node"] = node;
    content["time"] = t;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolGetNodeAnimation(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "get_node_animation");
    const QString clip = args.value("clip").toString();
    if (clip.isEmpty())
        return makeErrorResult("Error: missing required 'clip' argument");
    auto* m = NodeAnimationManager::instance();
    const QStringList clips = m->listClips();
    if (!clips.contains(clip))
        return makeErrorResult(QString("Error: clip '%1' not found").arg(clip));

    QJsonObject content;
    content["clip"] = clip;
    content["length"] = m->clipLength(clip);
    content["enabled"] = m->isClipEnabled(clip);
    QJsonArray nodesArr;
    const QStringList nodes = m->animatedNodes(clip);
    for (const QString& n : nodes) {
        QJsonObject nodeObj;
        nodeObj["node"] = n;
        QJsonArray times;
        for (double t : m->keyTimesForNode(clip, n)) times.append(t);
        nodeObj["key_times"] = times;
        nodesArr.append(nodeObj);
    }
    content["nodes"] = nodesArr;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

// ---------------------------------------------------------------------------
// Global playback controls (AnimationControlController) — speed, loop
// region, selection, and a state read-back.
// ---------------------------------------------------------------------------

QJsonObject MCPServer::toolSetPlaybackSpeed(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "set_playback_speed");
    if (!args.value("speed").isDouble())
        return makeErrorResult("Error: 'speed' must be a number");
    const double speed = args.value("speed").toDouble();
    if (!std::isfinite(speed) || speed <= 0.0)
        return makeErrorResult("Error: 'speed' must be a positive finite number");
    AnimationControlController::instance()->setPlaybackSpeed(speed);
    QJsonObject content;
    content["ok"] = true;
    content["speed"] = speed;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolSetLoopRegion(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "set_loop_region");
    auto* c = AnimationControlController::instance();
    // Validate BEFORE applying so a bad value can't leave a half-set region.
    // A non-finite / negative / inverted region produces a loop that never
    // triggers while the response still reports ok — reject those outright
    // (matches the finiteness contract the node-keyframe handlers use).
    double newStart = c->loopStart();
    double newEnd = c->loopEnd();
    if (args.contains("start")) {
        if (!args.value("start").isDouble())
            return makeErrorResult("Error: 'start' must be a number");
        newStart = args.value("start").toDouble();
        if (!std::isfinite(newStart) || newStart < 0.0)
            return makeErrorResult("Error: 'start' must be a non-negative finite number");
    }
    if (args.contains("end")) {
        if (!args.value("end").isDouble())
            return makeErrorResult("Error: 'end' must be a number");
        newEnd = args.value("end").toDouble();
        if (!std::isfinite(newEnd) || newEnd < 0.0)
            return makeErrorResult("Error: 'end' must be a non-negative finite number");
    }
    if (newEnd < newStart)
        return makeErrorResult("Error: 'end' must be >= 'start'");
    c->setLoopStart(newStart);
    c->setLoopEnd(newEnd);
    if (args.contains("active")) {
        if (!args.value("active").isBool())
            return makeErrorResult("Error: 'active' must be a boolean");
        c->setLoopRegionActive(args.value("active").toBool());
    }
    QJsonObject content;
    content["ok"] = true;
    content["start"] = c->loopStart();
    content["end"] = c->loopEnd();
    content["active"] = c->loopRegionActive();
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolGetPlaybackState(const QJsonObject & /*args*/)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "get_playback_state");
    auto* c = AnimationControlController::instance();
    QJsonObject content;
    content["speed"] = c->playbackSpeed();
    content["loop_start"] = c->loopStart();
    content["loop_end"] = c->loopEnd();
    content["loop_active"] = c->loopRegionActive();
    content["time"] = c->sliderValue() / 1000.0;
    content["length"] = c->animationLength();
    content["has_animation"] = c->hasAnimation();
    content["selected_entity"] = c->selectedEntityName();
    content["selected_animation"] = c->selectedAnimation();
    content["selected_bone"] = c->selectedBone();
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolSelectAnimation(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "select_animation");
    const QString entity = args.value("entity").toString();
    const QString anim = args.value("animation").toString();
    if (entity.isEmpty() || anim.isEmpty())
        return makeErrorResult("Error: 'entity' and 'animation' are required");
    AnimationControlController::instance()->selectAnimation(entity, anim);
    auto* c = AnimationControlController::instance();
    if (c->selectedAnimation() != anim || c->selectedEntityName() != entity)
        return makeErrorResult(
            QString("Error: could not select '%1' on '%2' (not found?)").arg(anim, entity));
    QJsonObject content;
    content["ok"] = true;
    content["entity"] = entity;
    content["animation"] = anim;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolSelectBone(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "select_bone");
    const QString bone = args.value("bone").toString();
    if (bone.isEmpty())
        return makeErrorResult("Error: missing required 'bone' argument");
    AnimationControlController::instance()->selectBone(bone);
    QJsonObject content;
    content["ok"] = true;
    content["bone"] = AnimationControlController::instance()->selectedBone();
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

// ---------------------------------------------------------------------------
// Morph weight keyframing over time (MorphAnimationManager). The existing
// set_morph_weight sets an instantaneous weight; these author weight KEYS on
// the shared MorphAnim clip so a blend-shape animates over time.
// ---------------------------------------------------------------------------

QJsonObject MCPServer::toolSetMorphWeightKeyframe(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "set_morph_weight_keyframe");
    const QString target = args.value("target").toString();
    if (target.isEmpty())
        return makeErrorResult("Error: missing required 'target' argument");
    if (!args.value("time").isDouble() || !args.value("weight").isDouble())
        return makeErrorResult("Error: 'time' and 'weight' must be numbers");
    const double time = args.value("time").toDouble();
    const double weight = args.value("weight").toDouble();
    if (!std::isfinite(time) || time < 0.0 || !std::isfinite(weight))
        return makeErrorResult("Error: time must be >= 0 and finite; weight finite");
    auto* m = MorphAnimationManager::instance();
    if (!m->setMorphWeightKeyframe(target, time, weight))
        return makeErrorResult(
            QString("Error: failed to key morph '%1' (target missing or no selection?)")
                .arg(target));
    m->activateWeightClip();
    QJsonObject content;
    content["ok"] = true;
    content["target"] = target;
    content["time"] = time;
    content["weight"] = weight;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolClearMorphWeightKeyframe(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "clear_morph_weight_keyframe");
    const QString target = args.value("target").toString();
    if (target.isEmpty())
        return makeErrorResult("Error: missing required 'target' argument");
    if (!args.value("time").isDouble())
        return makeErrorResult("Error: 'time' must be a number");
    const double time = args.value("time").toDouble();
    if (!std::isfinite(time))
        return makeErrorResult("Error: 'time' must be finite");
    auto* m = MorphAnimationManager::instance();
    if (!m->clearMorphWeightKeyframe(target, time))
        return makeErrorResult(
            QString("Error: no morph weight key near %1s for '%2'")
                .arg(time, 0, 'f', 3).arg(target));
    QJsonObject content;
    content["ok"] = true;
    content["target"] = target;
    content["time"] = time;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

// ---------------------------------------------------------------------------
// Skeletal keyframe editing + navigation (AnimationControlController). These
// operate on the currently-selected entity+animation+bone (set via
// select_animation / select_bone), matching the dope-sheet / curve editor.
// ---------------------------------------------------------------------------

static bool isValidChannelId(const QString& ch)
{
    static const QSet<QString> ids = {
        "tx","ty","tz","rw","rx","ry","rz","sx","sy","sz"};
    return ids.contains(ch);
}

QJsonObject MCPServer::toolSetKeyframeValue(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "set_keyframe_value");
    const QString bone = args.value("bone").toString();
    const QString channel = args.value("channel").toString();
    if (bone.isEmpty())
        return makeErrorResult("Error: missing required 'bone' argument");
    if (!isValidChannelId(channel))
        return makeErrorResult("Error: 'channel' must be one of "
                               "tx,ty,tz,rw,rx,ry,rz,sx,sy,sz");
    if (!args.value("time").isDouble() || !args.value("value").isDouble())
        return makeErrorResult("Error: 'time' and 'value' must be numbers");
    const double time = args.value("time").toDouble();
    const double value = args.value("value").toDouble();
    if (!std::isfinite(time) || time < 0.0 || !std::isfinite(value))
        return makeErrorResult("Error: time must be >= 0 and finite; value finite");
    if (!AnimationControlController::instance()->setKeyframeValue(bone, channel, time, value))
        return makeErrorResult(
            QString("Error: no keyframe at %1s on bone '%2' (select the animation "
                    "first with select_animation)").arg(time, 0, 'f', 3).arg(bone));
    QJsonObject content;
    content["ok"] = true;
    content["bone"] = bone;
    content["channel"] = channel;
    content["time"] = time;
    content["value"] = value;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolMoveBoneKeyframe(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "move_bone_keyframe");
    const QString bone = args.value("bone").toString();
    if (bone.isEmpty())
        return makeErrorResult("Error: missing required 'bone' argument");
    if (!args.value("old_time").isDouble() || !args.value("new_time").isDouble())
        return makeErrorResult("Error: 'old_time' and 'new_time' must be numbers");
    const double oldT = args.value("old_time").toDouble();
    const double newT = args.value("new_time").toDouble();
    if (!std::isfinite(oldT) || !std::isfinite(newT) || newT < 0.0)
        return makeErrorResult("Error: times must be finite and new_time >= 0");
    if (!AnimationControlController::instance()->moveKeyframe(bone, oldT, newT))
        return makeErrorResult(
            "Error: move rejected (no key at old_time, collision at new_time, "
            "or no animation/bone selected)");
    QJsonObject content;
    content["ok"] = true;
    content["bone"] = bone;
    content["old_time"] = oldT;
    content["new_time"] = newT;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolStepKeyframe(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "step_keyframe");
    const QString dir = args.value("direction").toString();
    auto* c = AnimationControlController::instance();
    if (dir == "next") c->nextKeyframe();
    else if (dir == "prev") c->prevKeyframe();
    else return makeErrorResult("Error: 'direction' must be 'next' or 'prev'");
    QJsonObject content;
    content["ok"] = true;
    content["direction"] = dir;
    content["time"] = c->sliderValue() / 1000.0;
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolGetChannelValues(const QJsonObject &args)
{
    SentryReporter::addBreadcrumb("ai.tool_call", "get_channel_values");
    const QString bone = args.value("bone").toString();
    const QString channel = args.value("channel").toString();
    if (bone.isEmpty())
        return makeErrorResult("Error: missing required 'bone' argument");
    if (!isValidChannelId(channel))
        return makeErrorResult("Error: 'channel' must be one of "
                               "tx,ty,tz,rw,rx,ry,rz,sx,sy,sz");
    const QVariantList vals =
        AnimationControlController::instance()->channelValuesAt(bone, channel);
    QJsonArray arr;
    for (const QVariant& v : vals) arr.append(v.toDouble());
    QJsonObject content;
    content["bone"] = bone;
    content["channel"] = channel;
    content["values"] = arr;
    content["count"] = arr.size();
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
    CloudSession session = CloudCredentialStore::loadSession();
    QtMeshCloudClient::UploadLimitsResult limits;
    if (connected) {
        const auto me = QtMeshCloudClient::fetchCurrentUser(session.token);
        if (me.ok && !me.user.value(QStringLiteral("email")).toString().isEmpty()) {
            session.email = me.user.value(QStringLiteral("email")).toString();
            CloudCredentialStore::saveSession(session);
        }
        limits = QtMeshCloudClient::fetchUploadLimits(session.token);
    }
    const qint64 lastUploadAt = CloudCredentialStore::lastUploadAt();

    QJsonObject content;
    content[QStringLiteral("connected")] = connected;
    if (connected) {
        if (!session.email.isEmpty())
            content[QStringLiteral("email")] = session.email;
        if (lastUploadAt > 0)
            content[QStringLiteral("lastUploadAt")] = lastUploadAt;
        if (limits.ok) {
            QJsonObject limitsObj;
            limitsObj.insert(QStringLiteral("maxFileSizeBytes"), limits.maxFileSizeBytes);
            limitsObj.insert(QStringLiteral("maxProjectSizeBytes"), limits.maxProjectSizeBytes);
            limitsObj.insert(QStringLiteral("maxReportSizeBytes"), limits.maxReportSizeBytes);
            content[QStringLiteral("limits")] = limitsObj;
        }
    }
    return makeSuccessResult(
        QString::fromUtf8(QJsonDocument(content).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolCloudLimits(const QJsonObject & /*args*/)
{
    SentryReporter::addBreadcrumb(QStringLiteral("ai.tool_call"), QStringLiteral("cloud_limits"));
    CloudCredentialStore::migrateLegacySettingsIfNeeded();
    const QString token = CloudCredentialStore::loadSession().token;
    if (token.isEmpty())
        return makeErrorResult("Error: not signed in. Use cloud_login first.");

    const auto limits = QtMeshCloudClient::fetchUploadLimits(token);
    if (!limits.ok)
        return makeErrorResult(limits.errorString);

    QJsonObject content;
    content.insert(QStringLiteral("maxFileSizeBytes"), limits.maxFileSizeBytes);
    content.insert(QStringLiteral("maxProjectSizeBytes"), limits.maxProjectSizeBytes);
    content.insert(QStringLiteral("maxReportSizeBytes"), limits.maxReportSizeBytes);
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

    QStringList includeGlobs;
    if (args.contains(QStringLiteral("include"))) {
        for (const QJsonValue& value : args.value(QStringLiteral("include")).toArray())
            includeGlobs.append(value.toString().trimmed());
    }
    QStringList excludeGlobs;
    if (args.contains(QStringLiteral("exclude"))) {
        for (const QJsonValue& value : args.value(QStringLiteral("exclude")).toArray())
            excludeGlobs.append(value.toString().trimmed());
    }

    const QStringList selectedPaths =
        CloudUploadPlanner::selectedPathsForUpload(filePath, includeGlobs, excludeGlobs);

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

// ---------------------------------------------------------------------------
// PS1 runtime ripper MCP surface (#412) — headless drive-and-verify.
// ---------------------------------------------------------------------------
#ifdef ENABLE_PS1_RIP

namespace {

void ps1PumpEventLoop(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
}

QJsonObject ps1StatusObject(PS1RipManager *mgr)
{
    QJsonObject o;
    o[QStringLiteral("sessionActive")] = mgr->isSessionActive();
    o[QStringLiteral("startPending")] = mgr->isStartPending();
    o[QStringLiteral("paused")] = mgr->isPaused();
    o[QStringLiteral("captureArmed")] = mgr->isCaptureArmed();
    o[QStringLiteral("hasBios")] = mgr->hasBios();
    o[QStringLiteral("hasIso")] = mgr->hasIso();
    o[QStringLiteral("coreId")] = mgr->activeCoreId();
    o[QStringLiteral("biosPath")] = mgr->biosPath();
    o[QStringLiteral("isoPath")] = mgr->isoPath();
    return o;
}

} // namespace

QJsonObject MCPServer::toolPs1RipStart(const QJsonObject &args)
{
    PS1RipManager *mgr = PS1RipManager::getSingleton();
    if (!mgr)
        return makeErrorResult("PS1 ripper unavailable");

    const QString bios = args.value(QStringLiteral("bios_path")).toString();
    const QString iso = args.value(QStringLiteral("iso_path")).toString();
    if (!bios.isEmpty() && !mgr->loadBios(bios))
        return makeErrorResult(QStringLiteral("Failed to load BIOS: %1").arg(bios));
    if (!iso.isEmpty() && !mgr->loadIso(iso))
        return makeErrorResult(QStringLiteral("Failed to load ISO: %1").arg(iso));
    if (!mgr->hasBios() || !mgr->hasIso())
        return makeErrorResult("BIOS and ISO must both be loaded (pass bios_path/iso_path)");

    if (!mgr->isSessionActive() && !mgr->start())
        return makeErrorResult("Failed to start emulation");

    const int bootTimeoutMs = args.value(QStringLiteral("boot_timeout_ms")).toInt(30000);
    QElapsedTimer t;
    t.start();
    while (!mgr->isSessionActive() && t.elapsed() < bootTimeoutMs)
        ps1PumpEventLoop(50);
    if (!mgr->isSessionActive())
        return makeErrorResult("Emulator did not reach an active session before timeout");

    QJsonObject o = ps1StatusObject(mgr);
    return makeSuccessResult(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolPs1RipStop(const QJsonObject &)
{
    PS1RipManager *mgr = PS1RipManager::getSingletonPtr();
    if (!mgr)
        return makeErrorResult("PS1 ripper not running");
    mgr->stop();
    ps1PumpEventLoop(300);
    return makeSuccessResult("stopped");
}

QJsonObject MCPServer::toolPs1RipStatus(const QJsonObject &)
{
    PS1RipManager *mgr = PS1RipManager::getSingletonPtr();
    if (!mgr)
        return makeErrorResult("PS1 ripper not initialized");
    QJsonObject o = ps1StatusObject(mgr);
    return makeSuccessResult(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolPs1RipRunFrames(const QJsonObject &args)
{
    PS1RipManager *mgr = PS1RipManager::getSingletonPtr();
    if (!mgr || !mgr->isSessionActive())
        return makeErrorResult("No active PS1 session — call ps1rip_start first");

    const int frames = std::clamp(args.value(QStringLiteral("frames")).toInt(600), 1, 20000);
    const bool autoInput = args.value(QStringLiteral("auto_input")).toBool();

    const int approxMs = std::clamp(frames * 1000 / 60, 100, 120000);
    QElapsedTimer t;
    t.start();
    int phase = 0;
    while (t.elapsed() < approxMs) {
        if (autoInput) {
            const bool press = (phase / 15) % 2 == 0;
            const unsigned button = ((phase / 30) % 2 == 0) ? 3u : 8u;
            mgr->setJoypadPressed(0, 3, false);
            mgr->setJoypadPressed(0, 8, false);
            mgr->setJoypadPressed(0, button, press);
            ++phase;
        }
        ps1PumpEventLoop(50);
    }
    if (autoInput)
        mgr->resetJoypad(0);

    QJsonObject o = ps1StatusObject(mgr);
    o[QStringLiteral("ranApproxFrames")] = frames;
    return makeSuccessResult(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolPs1RipCapture(const QJsonObject &args)
{
    PS1RipManager *mgr = PS1RipManager::getSingletonPtr();
    if (!mgr || !mgr->isSessionActive())
        return makeErrorResult("No active PS1 session — call ps1rip_start first");

    const bool trackedOnly = args.value(QStringLiteral("tracked_only")).toBool();
    const bool smooth = args.value(QStringLiteral("smooth")).toBool();
    const bool removeZeroArea = args.value(QStringLiteral("remove_zero_area")).toBool();
    const bool mergeObjects = args.value(QStringLiteral("merge_objects")).toBool(true);
    const bool rigidAnimation = args.value(QStringLiteral("rigid_animation")).toBool();
    Ps1NormalizerSettings ns = mgr->normalizerSettings();
    ns.trackedGeometryOnly = trackedOnly;
    ns.cleanupWeldNormals = smooth;
    ns.cleanupRemoveZeroArea = removeZeroArea;
    ns.mergeSameObjectParts = mergeObjects;
    ns.captureRigidAnimation = rigidAnimation;
    mgr->setNormalizerSettings(ns);

    QJsonObject built;
    bool done = false;
    QMetaObject::Connection conn = QObject::connect(
        mgr, &PS1RipManager::meshBuilt, mgr,
        [&](const QString &captureId, int capturedParts, int uniqueMeshes, int instanceCount,
            int vertexCount, int triangleCount, int matrixCount, uint32_t, bool,
            int gteInversePercent, int gteTrackedPercent, int depthOnlyPercent, bool slabLike,
            int primsWithMatrixId, int primsTotal, PsxVramMirrorMode, Gp0CaptureStats) {
            built[QStringLiteral("captureId")] = captureId;
            built[QStringLiteral("capturedParts")] = capturedParts;
            built[QStringLiteral("uniqueMeshes")] = uniqueMeshes;
            built[QStringLiteral("instances")] = instanceCount;
            built[QStringLiteral("vertices")] = vertexCount;
            built[QStringLiteral("triangles")] = triangleCount;
            built[QStringLiteral("matrices")] = matrixCount;
            built[QStringLiteral("trackedPercent")] = gteTrackedPercent;
            built[QStringLiteral("depthPercent")] = depthOnlyPercent;
            built[QStringLiteral("inversePercent")] = gteInversePercent;
            built[QStringLiteral("slabLike")] = slabLike;
            built[QStringLiteral("primsWithMatrix")] = primsWithMatrixId;
            built[QStringLiteral("primsTotal")] = primsTotal;
            done = true;
        });

    const int seconds = args.value(QStringLiteral("scene_seconds")).toInt();
    if (seconds > 0)
        mgr->captureScene(seconds);
    else {
        mgr->armCapture(true);
        ps1PumpEventLoop(200);
        mgr->captureFrame();
    }

    const int timeoutMs = args.value(QStringLiteral("timeout_ms")).toInt(30000)
                          + (seconds > 0 ? seconds * 1000 : 0);
    QElapsedTimer t;
    t.start();
    while (!done && t.elapsed() < timeoutMs)
        ps1PumpEventLoop(50);
    QObject::disconnect(conn);

    if (!done)
        return makeErrorResult("Capture produced no reconstructable geometry before timeout "
                               "(let the game play into a 3D scene first via ps1rip_run_frames)");

    built[QStringLiteral("trackedOnlyFilter")] = trackedOnly;
    return makeSuccessResult(QString::fromUtf8(QJsonDocument(built).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolPs1RipStats(const QJsonObject &)
{
    Manager *mgr = Manager::getSingletonPtr();
    if (!mgr)
        return makeErrorResult("Scene manager unavailable");
    int nodes = 0, entities = 0;
    qint64 tris = 0;
    for (Ogre::SceneNode *node : mgr->getSceneNodes()) {
        const QString name = QString::fromStdString(node->getName());
        if (!name.startsWith(QStringLiteral("PS1Capture_"))
            && !name.startsWith(QStringLiteral("PS1Imported_")))
            continue;
        ++nodes;
        for (Ogre::MovableObject *obj : node->getAttachedObjects()) {
            if (!obj || obj->getMovableType() != "Entity")
                continue;
            ++entities;
            Ogre::Entity *e = static_cast<Ogre::Entity *>(obj);
            const Ogre::MeshPtr &m = e->getMesh();
            for (unsigned s = 0; s < m->getNumSubMeshes(); ++s) {
                Ogre::SubMesh *sm = m->getSubMesh(s);
                if (sm->indexData)
                    tris += sm->indexData->indexCount / 3;
            }
        }
    }
    QJsonObject o;
    o[QStringLiteral("captureNodes")] = nodes;
    o[QStringLiteral("entities")] = entities;
    o[QStringLiteral("triangles")] = static_cast<double>(tris);
    return makeSuccessResult(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented)));
}

QJsonObject MCPServer::toolPs1RipClear(const QJsonObject &)
{
    Manager *mgr = Manager::getSingletonPtr();
    if (!mgr)
        return makeErrorResult("Scene manager unavailable");
    QStringList toRemove;
    for (Ogre::SceneNode *node : mgr->getSceneNodes()) {
        const QString name = QString::fromStdString(node->getName());
        if (name.startsWith(QStringLiteral("PS1Capture_"))
            || name.startsWith(QStringLiteral("PS1Imported_")))
            toRemove.append(name);
    }
    for (const QString &name : toRemove)
        mgr->destroySceneNode(name);
    if (PS1CapturedAssets *store = PS1CapturedAssets::getSingletonPtr())
        store->clear();
    QJsonObject o;
    o[QStringLiteral("removed")] = toRemove.size();
    return makeSuccessResult(QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Indented)));
}

#else // !ENABLE_PS1_RIP

QJsonObject MCPServer::toolPs1RipStart(const QJsonObject &)
{ return makeErrorResult("PS1 ripper not compiled in (build with -DENABLE_PS1_RIP=ON)"); }
QJsonObject MCPServer::toolPs1RipStop(const QJsonObject &)
{ return makeErrorResult("PS1 ripper not compiled in (build with -DENABLE_PS1_RIP=ON)"); }
QJsonObject MCPServer::toolPs1RipStatus(const QJsonObject &)
{ return makeErrorResult("PS1 ripper not compiled in (build with -DENABLE_PS1_RIP=ON)"); }
QJsonObject MCPServer::toolPs1RipRunFrames(const QJsonObject &)
{ return makeErrorResult("PS1 ripper not compiled in (build with -DENABLE_PS1_RIP=ON)"); }
QJsonObject MCPServer::toolPs1RipCapture(const QJsonObject &)
{ return makeErrorResult("PS1 ripper not compiled in (build with -DENABLE_PS1_RIP=ON)"); }
QJsonObject MCPServer::toolPs1RipStats(const QJsonObject &)
{ return makeErrorResult("PS1 ripper not compiled in (build with -DENABLE_PS1_RIP=ON)"); }
QJsonObject MCPServer::toolPs1RipClear(const QJsonObject &)
{ return makeErrorResult("PS1 ripper not compiled in (build with -DENABLE_PS1_RIP=ON)"); }

#endif // ENABLE_PS1_RIP


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
        "List the built-in material presets (Plastic / Metal / Wood / Glass / Unlit / Wireframe + PBR templates: Metallic-Roughness, Specular-Glossiness, Unlit PBR + HDR Environment presets: Polished Metal (HDR), Glass (HDR), Skin (HDR-friendly), etc.). Pass any returned name to apply_material_preset.",
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

    // create_light
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        QJsonObject properties;
        properties["type"] = QJsonObject{{"type", "string"}, {"description", "directional | point | spot"}};
        properties["position"] = QJsonObject{{"type", "array"}, {"description", "World position [x, y, z]"}};
        properties["direction"] = QJsonObject{{"type", "array"}, {"description", "Optional aim direction [x, y, z] for directional/spot lights"}};
        properties["colour"] = QJsonObject{{"type", "string"}, {"description", "Optional diffuse colour (#rrggbb or [r,g,b])"}};
        properties["intensity"] = QJsonObject{{"type", "number"}, {"description", "Optional powerScale (QtMeshEditor intensity units)"}};
        properties["range"] = QJsonObject{{"type", "number"}, {"description", "Optional attenuation range for point/spot lights"}};
        properties["cone"] = QJsonObject{{"type", "array"}, {"description", "Optional spot cone [innerDeg, outerDeg]"}};
        inputSchema["properties"] = properties;
        inputSchema["required"] = QJsonArray{"type", "position"};
        tools.append(buildToolDefinition(
            "create_light",
            "Create a user scene light in the live editor scene. Intensity uses QtMeshEditor powerScale units.",
            inputSchema));
    }

    // delete_light
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        QJsonObject properties;
        properties["name"] = QJsonObject{{"type", "string"}, {"description", "Light scene-node name"}};
        inputSchema["properties"] = properties;
        inputSchema["required"] = QJsonArray{"name"};
        tools.append(buildToolDefinition(
            "delete_light",
            "Delete a user scene light by name from the live editor scene.",
            inputSchema));
    }

    // list_lights
    appendTool(
        "list_lights",
        "List every user light in the live scene (name, type, colour, intensity, rig group, etc.) as JSON.",
        QJsonObject());

    // set_light_property
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        QJsonObject properties;
        properties["name"] = QJsonObject{{"type", "string"}, {"description", "Light scene-node name"}};
        properties["key"] = QJsonObject{{"type", "string"}, {"description", "position | direction | colour | intensity | range | enabled | castShadows | cone"}};
        properties["value"] = QJsonObject{{"description", "New value (type depends on key)"}};
        inputSchema["properties"] = properties;
        inputSchema["required"] = QJsonArray{"name", "key", "value"};
        tools.append(buildToolDefinition(
            "set_light_property",
            "Set one property on an existing user light in the live scene.",
            inputSchema));
    }

    // apply_light_rig
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        QJsonObject properties;
        properties["name"] = QJsonObject{{"type", "string"}, {"description", "Rig preset id (e.g. three_point_studio). Use list_rigs via qtmesh light --list-rigs."}};
        properties["replace_existing"] = QJsonObject{{"type", "boolean"}, {"description", "When true, remove existing rig groups before applying."}};
        inputSchema["properties"] = properties;
        inputSchema["required"] = QJsonArray{"name"};
        tools.append(buildToolDefinition(
            "apply_light_rig",
            "Apply a built-in light rig preset to the live scene (same presets as the Lighting panel).",
            inputSchema));
    }

    // set_hdr_environment
  {
        QJsonObject properties;
        properties["path_or_name"] = QJsonObject{
            {"type", "string"},
            {"description", "Absolute path or bundled HDRI name (e.g. studio_neutral or studio_neutral.hdr)"}};
        appendTool(
            "set_hdr_environment",
            "Load a global HDR environment for image-based lighting (IBL). Mirrors the Inspector Environment picker.",
            properties,
            QJsonArray{"path_or_name"});
    }

    // get_hdr_environment
    appendTool(
        "get_hdr_environment",
        "Return the active HDR environment path, IBL readiness, tonemap settings, and skybox defaults as JSON.",
        QJsonObject());

    // set_tonemap
    {
        QJsonObject properties;
        properties["operator"] = QJsonObject{
            {"type", "string"},
            {"description", "Tonemap operator: aces (default), reinhard, or agx"}};
        properties["exposure"] = QJsonObject{
            {"type", "number"},
            {"description", "Exposure in EV stops (alias: exposure_ev)"}};
        properties["white_point"] = QJsonObject{
            {"type", "number"},
            {"description", "Reinhard white point (ignored for ACES/AgX)"}};
        appendTool(
            "set_tonemap",
            "Set global viewport tonemap operator, exposure, and optional Reinhard white point.",
            properties);
    }

    // set_env_intensity
    {
        QJsonObject properties;
        properties["material"] = QJsonObject{
            {"type", "string"},
            {"description", "Ogre material name (use list_materials)"}};
        properties["value"] = QJsonObject{
            {"type", "number"},
            {"description", "IBL environment intensity multiplier (0..4, alias: intensity)"}};
        appendTool(
            "set_env_intensity",
            "Set per-material IBL environment intensity (pbr_environment_intensity).",
            properties,
            QJsonArray{"material", "value"});
    }

    // set_env_tint
    {
        QJsonObject properties;
        properties["material"] = QJsonObject{
            {"type", "string"},
            {"description", "Ogre material name (use list_materials)"}};
        properties["hex"] = QJsonObject{
            {"type", "string"},
            {"description", "Environment tint as #rrggbb (e.g. #fff5e6)"}};
        appendTool(
            "set_env_tint",
            "Set per-material IBL environment tint (pbr_environment_tint).",
            properties,
            QJsonArray{"material", "hex"});
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

    // motion_in_between
    {
        QJsonObject props;
        props["entity_name"] = QJsonObject{{"type", "string"}, {"description", "Name of the entity. If omitted, uses the first entity with a skeleton."}};
        props["animation_name"] = QJsonObject{{"type", "string"}, {"description", "Name of the animation to fill. If omitted, uses the first animation on the skeleton."}};
        props["gap_frames"] = QJsonObject{{"type", "integer"}, {"description", "Number of intermediate keyframes to insert between the bracketing keys (N >= 1)."}};
        props["start_time"] = QJsonObject{{"type", "number"}, {"description", "Clip-time (seconds) of the window start. Defaults to the clip start (0)."}};
        props["end_time"] = QJsonObject{{"type", "number"}, {"description", "Clip-time (seconds) of the window end. Defaults to the clip length."}};
        props["no_model"] = QJsonObject{{"type", "boolean"}, {"description", "Force the deterministic spline fallback instead of the RMIB ML model. Default false."}};
        appendTool(
            "motion_in_between",
            "AI animation in-betweening (#409): fill the gap between two keyframes with smooth, plausible "
            "intermediate poses predicted by Robust Motion In-betweening (RMIB, ONNX). For each bone track "
            "whose keyframes bracket [start_time, end_time], inserts gap_frames interpolated keys. Falls back "
            "automatically to a cubic-Hermite + slerp spline (visibly smoother than linear) when the model is "
            "unavailable, the build lacks ONNX, or the skeleton is incompatible with the model — the result "
            "reports which path ran. Works best on humanoid skeletons close to the model's training distribution.",
            props,
            QJsonArray{"gap_frames"}
        );
    }

    // generate_motion
    {
        QJsonObject props;
        props["prompt"] = QJsonObject{{"type", "string"}, {"description", "Text describing the motion, e.g. \"walking confidently\", \"jump\", \"wave hello\". Matched by action keyword to the bundled clip library (walk/run/jump/dance/march/kick/punch/wave/climb/idle; synonyms like jog→run)."}};
        props["entity_name"] = QJsonObject{{"type", "string"}, {"description", "Name of the rigged entity to apply the motion to. If omitted, uses the first skinned entity."}};
        props["duration"] = QJsonObject{{"type", "number"}, {"description", "Optional clip length in seconds (retimes the template). Default: the clip's native length."}};
        props["output_path"] = QJsonObject{{"type", "string"}, {"description", "Optional path to re-export the mesh with the new animation (e.g. /tmp/out.glb). If omitted, the animation is applied in-session only."}};
        props["model"] = QJsonObject{{"type", "boolean"}, {"description", "EXPERIMENTAL: use the trained from-scratch text-to-motion ONNX model instead of the template clip. Falls back to the template library automatically if the model is unavailable or the action isn't in its vocabulary. Default false (template). Quality is action-dependent (locomotion better than gestures)."}};
        props["arm_space"] = QJsonObject{{"type", "number"}, {"description", "Optional Mixamo-style arm-space post-process in degrees (#854): positive widens the arms away from the body, negative tucks them in. Default 0. Rescues arm-into-torso clipping on rigs whose proportions differ from the clip."}};
        props["foot_pin"] = QJsonObject{{"type", "boolean"}, {"description", "Foot-contact cleanup (#856): detect ground-contact spans and IK-pin the feet so they plant instead of skating. Default true; set false to keep the raw retarget."}};
        props["smooth_bake"] = QJsonObject{{"type", "boolean"}, {"description", "Temporal low-pass post-pass: bake the clip sparse then back to its native rate, removing retarget trembling. Default true."}};
        props["smooth_fps"] = QJsonObject{{"type", "number"}, {"description", "Sparse keyframe rate for the smooth-bake pass. Lower = smoother but softer motion. Default 12."}};
        props["vertical_descent"] = QJsonObject{{"type", "boolean"}, {"description", "Lower the body to the ground on non-locomotion crouch/pickup/sit/crawl/death clips (#838, descent-only). Default true; set false to keep the root at standing height when the descent over-sinks on a given clip. No effect on locomotion actions."}};
        props["variant_index"] = QJsonObject{{"type", "integer"}, {"description", "Select an EXACT clip from the library by index (parity with CLI --variant / the GUI picker) instead of keyword-matching a prompt. Forces the template path. When given, 'prompt' is optional. Out-of-range indices error."}};
        appendTool(
            "generate_motion",
            "AI text-to-motion (#411, experimental): generate a skeletal animation from a text prompt and "
            "retarget it onto a rigged mesh. MVP approach — matches the prompt to a curated, permissively-"
            "licensed motion clip (CMU MoCap) and retargets it onto the skeleton via the canonical-joint "
            "mapping (same as #409). Pass 'variant_index' to pick an exact clip deterministically. The clip "
            "library downloads on first use. Requires a humanoid rig; reports which action matched and how "
            "many bones/joints were retargeted. (Not generative diffusion — see "
            "docs/TEXT_TO_MOTION_SPIKE_411.md for why the template approach ships first.)",
            props,
            QJsonArray{}   // neither prompt nor variant_index is unconditionally required; handler validates
        );
    }

    // adjust_arm_space
    {
        QJsonObject props;
        props["animation_name"] = QJsonObject{{"type", "string"}, {"description", "Name of the animation to adjust, e.g. \"generated_walk\"."}};
        props["arm_space"] = QJsonObject{{"type", "number"}, {"description", "Absolute arm-space angle in degrees: positive widens the arms away from the body, negative tucks them in, 0 restores the original. Idempotent — re-applying reverts the previous value first."}};
        props["entity_name"] = QJsonObject{{"type", "string"}, {"description", "Name of the rigged entity. If omitted, uses the first skinned entity."}};
        props["output_path"] = QJsonObject{{"type", "string"}, {"description", "Optional path to re-export the mesh with the adjusted animation. If omitted, applied in-session only."}};
        appendTool(
            "adjust_arm_space",
            "Mixamo-style arm-space post-process (#854): swing the arm chains of an existing animation outward "
            "(widen) or inward (tuck) to rescue arm-into-torso clipping on rigs whose proportions differ from "
            "the source clip. Only the shoulders/collars are rewritten; elbows/hands follow through the "
            "hierarchy, and legs/spine are untouched. The angle is ABSOLUTE and idempotent (re-applying reverts "
            "the prior value), so it maps directly to a UI slider.",
            props,
            QJsonArray{"animation_name", "arm_space"}
        );
    }

    // pin_feet
    {
        QJsonObject props;
        props["animation_name"] = QJsonObject{{"type", "string"}, {"description", "Name of the animation to clean up, e.g. \"generated_walk\"."}};
        props["entity_name"] = QJsonObject{{"type", "string"}, {"description", "Name of the rigged entity. If omitted, uses the first skinned entity."}};
        props["output_path"] = QJsonObject{{"type", "string"}, {"description", "Optional path to re-export the mesh with the cleaned animation. If omitted, applied in-session only."}};
        appendTool(
            "pin_feet",
            "Foot-contact cleanup (#856): detect frames where each foot is near the clip's ground level and "
            "nearly stationary (contact spans) and lock the foot's world position to its span-start position "
            "with an analytic two-bone hip-knee-foot IK, blending in/out at span edges so knees don't pop. "
            "Fixes foot skating/floating on retargeted clips whose rig proportions differ from the source. "
            "Only the thigh/shin/foot keyframes are rewritten; effectively idempotent.",
            props,
            QJsonArray{"animation_name"}
        );
    }

    // segment_mesh
    {
        QJsonObject props;
        props["entity_name"] = QJsonObject{{"type", "string"}, {"description", "Name of the entity to segment. If omitted, uses the first mesh entity."}};
        props["no_model"] = QJsonObject{{"type", "boolean"}, {"description", "Force the deterministic geometric fallback instead of the PointNet++ ML model. Default false."}};
        props["up_axis"] = QJsonObject{{"type", "string"}, {"enum", QJsonArray{"x", "y", "z"}}, {"description", "Mesh up axis. Affects BOTH the ML model (the point cloud is remapped to the model's +Y-up training frame before inference) and the geometric fallback's head-vs-leg heuristic. Set this for X/Z-up meshes or labels will be wrong. Default 'y' (+Y up)."}};
        props["category"] = QJsonObject{{"type", "string"}, {"enum", QJsonArray{"auto", "body", "vegetation", "vehicle", "building"}}, {"description", "Mesh category (#818): selects the specialised label set + model. 'auto' (default) runs the tiny point-cloud category classifier first (downloads on first use; falls back to 'body' when unavailable). body = head/torso/arms/legs; vegetation = trunk/branch/foliage/root/flower; vehicle = vehicle_body/wheel/window/wing/rotor; building = wall/roof/window/door/chimney/foundation."}};
        props["no_cleanup"] = QJsonObject{{"type", "boolean"}, {"description", "Return RAW model labels: skip the split-cleanup pass (#863) that de-fringes ragged part seams and reabsorbs small disconnected junction islands. Default false (cleanup ON)."}};
        appendTool(
            "segment_mesh",
            "AI mesh part segmentation (#410/#818): predict a semantic part label "
            "per vertex via a category-specialised PointNet++ ONNX model — body "
            "(head/torso/arms/legs), vegetation, vehicle, or building label sets, "
            "auto-dispatched by a point-cloud category classifier. Returns JSON with "
            "the full label map: the resolved category, per-part vertex+face counts "
            "and the per-face label array (index into `parts`) so callers can drive "
            "selection / per-part material assignment directly. Falls back "
            "automatically to a deterministic geometric segmenter (connected "
            "components + spatial heuristic, refined by rig bone proximity) when "
            "the model is unavailable or the build lacks ONNX — the result reports "
            "which path ran.",
            props
        );
    }

    // split_mesh_by_segments (#859/#861): PartOps split into per-part submeshes.
    {
        QJsonObject props;
        props["entity_name"] = QJsonObject{{"type", "string"}, {"description", "Entity to split. Empty → the first mesh entity in the scene."}};
        props["no_model"] = QJsonObject{{"type", "boolean"}, {"description", "Force the offline geometric/rig-prior segmentation (skip the ONNX model). Default false."}};
        props["up_axis"] = QJsonObject{{"type", "string"}, {"enum", QJsonArray{"x", "y", "z"}}, {"description", "Mesh up axis for segmentation. Default 'y'."}};
        props["category"] = QJsonObject{{"type", "string"}, {"enum", QJsonArray{"auto", "body", "vegetation", "vehicle", "building"}}, {"description", "Segmentation category (default 'auto')."}};
        props["solidify"] = QJsonObject{{"type", "boolean"}, {"description", "Give each part real WALL VOLUME before capping (default false). For thin-shell game assets (single-sided surfaces) an exploded part otherwise exposes its hollow interior at the cut; solidify offsets an inner shell so the cut shows a solid wall. Adds geometry — only meaningful for thin shells."}};
        appendTool(
            "split_mesh_by_segments",
            "PartOps split (#859/#861): segment the selected/named mesh and REPLACE "
            "it with one named submesh per detected part (head/torso/left_arm/…). "
            "Boundary vertices are duplicated so parts are independent; normals, "
            "UVs, colours, tangents, the skeleton and bone weights are preserved. "
            "Undoable (same command as the GUI 'Split into Parts' button). Returns "
            "the created submesh count + part names. FBX export keeps the submesh "
            "boundaries; glTF coalesces same-material parts.",
            props
        );
    }

    // explode_mesh_parts (#862/#864) — split a multi-part mesh into separate nodes.
    {
        QJsonObject props;
        props["entity_name"] = QJsonObject{{"type", "string"}, {"description", "Entity to explode (must already have >=2 part submeshes — split it first). Empty → the first mesh entity."}};
        props["distance"] = QJsonObject{{"type", "number"}, {"description", "Outward explode offset multiplier (× the assembly diagonal). Default 0.15; 0 = parts coincident."}};
        appendTool(
            "explode_mesh_parts",
            "PartOps explode (#862/#864): split every submesh of an already-multi-part "
            "entity into its own scene node, offset outward from the assembly centre. "
            "Preserves materials, and (for a skinned source) the skeleton + bone "
            "weights. Undoable (same command as the GUI 'Explode Parts' button). "
            "Returns the exploded part count.",
            props
        );
    }

    // join_mesh_parts (#862/#864) — merge separate part entities into one mesh.
    {
        QJsonObject props;
        props["entity_names"] = QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "string"}}}, {"description", "Names of the 2+ part entities to join (world transforms baked in). Omit → join ALL mesh entities in the scene."}};
        appendTool(
            "join_mesh_parts",
            "PartOps join (#862/#864): merge 2+ part entities into ONE fused mesh, "
            "baking each part's world transform into its geometry. Same-material "
            "submeshes coalesce. Yields STATIC geometry (skeletons are NOT reconciled "
            "— a documented join limitation). Undoable (same command as the GUI 'Join "
            "Parts' button). Returns the joined part count + created submesh count.",
            props
        );
    }

    // generate_mesh_from_image (#764) — always advertised: the TRELLIS.2
    // backend runs through an external runtime and needs no ONNX build (the
    // handler gates the local TripoSR/TripoSG paths on ENABLE_ONNX itself).
    {
        QJsonObject props;
        props["image_path"] = QJsonObject{{"type", "string"}, {"description", "Absolute path to the source image (a single object, ideally background-removed). Required."}};
        props["output"] = QJsonObject{{"type", "string"}, {"description", "Optional path to save the generated mesh (e.g. /tmp/out.glb). If omitted, the mesh is loaded into the current scene instead."}};
        props["resolution"] = QJsonObject{{"type", "integer"}, {"description", "Marching-cubes grid resolution 16..1024 (default 256; 128 is a fast/preview tier). Higher = more detail + slower. Cost is res^3 floats in RAM: 512~=0.5 GB, 768~=1.7 GB, 1024~=4.3 GB. (TripoSR's encoder input is fixed at 512^2, so its detail gains taper off above 512; TripoSG uses a 224^2 DINOv2 encoder and this is purely the extraction grid.)"}};
        props["vertex_color"] = QJsonObject{{"type", "boolean"}, {"description", "TripoSR only: bake its predicted per-vertex color (default true). Ignored by triposg (geometry-only — colour comes from the AI texture pass)."}};
        props["remove_bg"] = QJsonObject{{"type", "boolean"}, {"description", "Run U²-Net background removal on the image first (default false). Recommended for photos with a background; the model needs an isolated subject. Falls back to the raw image if the model is unavailable."}};
        props["quality"] = QJsonObject{{"type", "string"}, {"enum", QJsonArray{"fp32", "int8"}}, {"description", "Precision/size tier, downloaded on demand. TripoSR: fp32 = best (~1.7GB), int8 = smallest with slight quality loss (~430MB). TripoSG: fp32 only (int8 degrades geometry) — int8 is silently upgraded to fp32."}};
        props["smooth"] = QJsonObject{{"type", "boolean"}, {"description", "Taubin-smooth the extracted mesh to remove marching-cubes stair-stepping (default true; volume-preserving)."}};
        props["refine"] = QJsonObject{{"type", "boolean"}, {"description", "After smoothing, Newton-project each vertex back onto the network's true iso-surface via extra decoder queries (default true; recovers grid-quantized detail)."}};
        props["bake_texture"] = QJsonObject{{"type", "boolean"}, {"description", "TripoSR only: bake a real diffuse texture (xatlas unwrap + per-texel decoder color) instead of per-vertex colors (default true; falls back to vertex colors if the bake fails). Ignored by triposg (its colour comes from the GUI AI-texture pass; the CLI/MCP triposg mesh is geometry-only)."}};
        props["texture_size"] = QJsonObject{{"type", "integer"}, {"description", "Baked-texture resolution 64..8192 (default 1024)."}};
        props["upscale_texture"] = QJsonObject{{"type", "boolean"}, {"description", "Run Real-ESRGAN 2x on the baked diffuse before saving (default false; best-effort — keeps the un-upscaled texture if the upscale model is unavailable)."}};
        props["generate_pbr"] = QJsonObject{{"type", "boolean"}, {"description", "Synthesize normal + roughness maps from the baked diffuse (#404 PBRify) and bind them into the material — the polished-surface look (default true; requires bake_texture; fails soft to diffuse-only if the models are unavailable)."}};
        props["backend"] = QJsonObject{{"type", "string"}, {"enum", QJsonArray{"trellis2", "triposr", "triposg"}}, {"description", "Generation backend. DEFAULT: trellis2 when its runtime is installed on this machine, else triposr. trellis2 = Microsoft TRELLIS.2-4B (MIT) via the Python sidecar (Linux + NVIDIA GPU) — highest quality, real PBR (base color/metallic/roughness) baked natively by QtMeshEditor WITHOUT NVIDIA nvdiffrast/nvdiffrec; triposr = fast local single-pass LRM with color; triposg = 1.5B rectified-flow model — higher-fidelity GEOMETRY, slower, geometry-only."}};
        props["flow_steps"] = QJsonObject{{"type", "integer"}, {"description", "TripoSG rectified-flow Euler steps 1..200 (default 25; 50 = reference quality, 10 = fast preview). Ignored by the other backends."}};
        props["guidance"] = QJsonObject{{"type", "number"}, {"description", "TripoSG classifier-free-guidance scale 0..30 (default 7; 0 disables CFG and halves DiT cost). Ignored by the other backends."}};
        props["seed"] = QJsonObject{{"type", "integer"}, {"description", "trellis2 only: deterministic generation seed (default 42)."}};
        props["preset"] = QJsonObject{{"type", "string"}, {"enum", QJsonArray{"fast", "balanced", "high"}}, {"description", "trellis2 only: quality preset (default balanced). fast = 512 pipeline, balanced = 1024 cascade, high = 1536 cascade (more VRAM/time)."}};
        props["target_tris"] = QJsonObject{{"type", "integer"}, {"description", "ALL backends: game-ready target triangle count — weld + debris-cull + simplify, re-baking lost detail as diffuse + tangent-space normal maps (0 = keep the original density; suggested presets: 10000 low / 25000 medium / 50000 high). For trellis2 the full-res source is preserved as a .qtm3d sidecar when 'output' is given."}};
        appendTool(
            "generate_mesh_from_image",
            "AI image-to-3D mesh generation (epic #764 + TRELLIS.2): reconstruct a "
            "3D mesh from a single image. Backends: trellis2 (Microsoft TRELLIS.2-4B "
            "sidecar — the default when installed; PBR-textured, game-ready "
            "processing + native texture bake), triposr (local ONNX, fast, "
            "color), triposg (local ONNX, best local geometry). Returns "
            "vertexCount/triangleCount/backend and, when 'output' is given, the "
            "saved meshPath (+ sourcePath for the preserved trellis2 full-res "
            "generation); otherwise the mesh is loaded into the scene. Models "
            "download on first use; a missing runtime/model returns a clear error "
            "(no crash).",
            props,
            QJsonArray{"image_path"}
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

    // uv_info (#465)
    {
        QJsonObject props;
        props["channel"] = QJsonObject{{"type", "integer"},
            {"description", "UV channel to analyze. Default 0."}};
        appendTool(
            "uv_info",
            "Report UV channel coverage, island count, and an AABB overlap upper bound for "
            "the currently selected mesh. Response includes a structured 'uv' object.",
            props
        );
    }

    // uv_project (#465)
    {
        QJsonObject props;
        props["mode"] = QJsonObject{{"type", "string"},
            {"description", "Projection mode: box, cylinder, sphere, or reset. Default box."}};
        props["channel"] = QJsonObject{{"type", "integer"},
            {"description", "UV channel to write. Default 0."}};
        props["axis"] = QJsonObject{{"type", "integer"},
            {"description", "Primary axis for box/cylinder/sphere (0=X, 1=Y, 2=Z). Default 1."}};
        props["scale"] = QJsonObject{{"type", "number"},
            {"description", "Box projection scale multiplier. Default 1.0."}};
        appendTool(
            "uv_project",
            "Apply a geometric UV projection (box / cylinder / sphere / reset) to the "
            "currently selected mesh in-session. View projection is GUI-only.",
            props
        );
    }

    // uv_set_seams (#465)
    {
        QJsonObject props;
        props["edges"] = QJsonObject{{"type", "string"},
            {"description",
             "Comma-separated seam edges as submesh:vertA-vertB (e.g. \"0:1-2,0:2-3\"). "
             "Submesh prefix may be omitted when 0."}};
        appendTool(
            "uv_set_seams",
            "Mark mesh edges as UV seams on the currently selected entity. "
            "Edges persist in qtme.seams bindings for unwrap / the UV editor.",
            props,
            QJsonArray{"edges"}
        );
    }

    // uv_unwrap_selection (#465)
    {
        QJsonObject props;
        props["submesh"] = QJsonObject{{"type", "integer"},
            {"description", "Submesh index containing the triangles. Default 0."}};
        props["triangles"] = QJsonObject{
            {"type", "array"},
            {"items", QJsonObject{{"type", "integer"}}},
            {"description", "Local triangle indices to re-unwrap via xatlas."}};
        props["resolution"] = QJsonObject{{"type", "integer"},
            {"description", "Atlas resolution hint. Default 1024."}};
        props["padding"] = QJsonObject{{"type", "integer"},
            {"description", "Chart padding in texels. Default 4."}};
        props["channel"] = QJsonObject{{"type", "integer"},
            {"description", "UV channel to write. Default 0."}};
        props["preserve_original"] = QJsonObject{{"type", "boolean"},
            {"description", "Preserve the overwritten channel on UV{channel+1}. Default true."}};
        appendTool(
            "uv_unwrap_selection",
            "xatlas re-unwrap of selected triangle indices on one submesh of the "
            "currently selected entity. Respects existing seam bindings.",
            props,
            QJsonArray{"triangles"}
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
        props["algo"] = QJsonObject{{"type", "string"},
            {"enum", QJsonArray{"skintokens", "geodesic-voxel",
                                "inverse-distance", "unirig"}},
            {"description",
             "Weighting algorithm: 'skintokens' (default — SkinTokens/TokenRig ML "
             "skinner, geodesically localised; downloads ~2.3 GB models on first use "
             "and falls back to geodesic-voxel when models/ONNX are unavailable), "
             "'geodesic-voxel' (Maya-style volume-aware bind), 'inverse-distance' "
             "(legacy straight-line heuristic). 'unirig' is a deprecated alias of "
             "'skintokens'."}};
        props["voxel_resolution"] = QJsonObject{{"type", "integer"},
            {"description",
             "Geodesic-voxel grid resolution along the longest axis. Higher resolves "
             "thinner parts (fingers). Range [8, 256]. Default 64."}};
        props["smooth_iterations"] = QJsonObject{{"type", "integer"},
            {"description",
             "Laplacian weight-smoothing iterations applied after any algorithm "
             "(0 disables). Range [0, 50]. Default 3."}};
        appendTool(
            "compute_skin_weights",
            "Compute and apply skin weights for the currently selected mesh against "
            "its attached skeleton. Default algorithm is the SkinTokens ML skinner "
            "(issue #819) with geodesic localisation; it falls back to geodesic-voxel "
            "binding when the models or ONNX are unavailable. Weights are "
            "Laplacian-smoothed and pruned. The mesh must have a skeleton attached.",
            props
        );
    }

    // set_skinning_display (#819 Slice D)
    {
        QJsonObject props;
        props["mode"] = QJsonObject{{"type", "string"},
            {"enum", QJsonArray{"linear", "dual-quaternion"}},
            {"description",
             "'linear' (default LBS path) or 'dual-quaternion' (RTSS hardware DQS — "
             "preserves volume on twists, no candy-wrapper collapse)."}};
        appendTool(
            "set_skinning_display",
            "Set the skinning display mode of the currently selected skinned entity. "
            "Dual-quaternion is a runtime shading choice only: exported weights are "
            "unchanged (engines re-skin with their own blend). Issue #819 Slice D.",
            props,
            QJsonArray{"mode"}
        );
    }

    // auto_rig (#407)
    {
        QJsonObject props;
        props["template"] = QJsonObject{{"type", "string"},
            {"description",
             "Skeleton template: 'humanoid' (19-bone, default), 'biped', "
             "'quadruped', or 'generic' (3-joint spine fallback). Used by the "
             "'pinocchio' algorithm (and as the 'unirig' fallback)."}};
        props["algo"] = QJsonObject{{"type", "string"},
            {"description",
             "Skeleton-prediction backend: 'pinocchio' (native template embedding, "
             "offline, default) or 'unirig' (UniRig ML model via ONNX — better on "
             "arbitrary/non-humanoid topology; needs an ONNX build + first-run model "
             "download, and falls back to pinocchio when unavailable)."}};
        props["skin"] = QJsonObject{{"type", "boolean"},
            {"description",
             "When true, also compute + apply skin weights so the mesh deforms "
             "immediately (chains compute_skin_weights). Default false."}};
        props["up_axis"] = QJsonObject{{"type", "string"},
            {"description", "Mesh up axis: 'x', 'y' (default), or 'z'."}};
        props["output_path"] = QJsonObject{{"type", "string"},
            {"description",
             "Optional path to re-export the rigged mesh. When omitted, the rig is "
             "applied to the in-session scene only."}};
        appendTool(
            "auto_rig",
            "Auto-rig the currently selected STATIC (unrigged) mesh by embedding a "
            "skeleton template into it (issue #407). Native heuristic (no external "
            "deps): maps a proportional joint graph into the mesh AABB and recentres "
            "joints toward the mesh's medial mass. Best on roughly upright, manifold, "
            "T/A-pose meshes with +Y up. Already-skinned meshes are rejected. Pair "
            "skin:true for a one-click rig+skin.",
            props
        );
    }

    // remove_skeleton
    {
        QJsonObject props;
        appendTool(
            "remove_skeleton",
            "Remove the ENTIRE skeleton from the currently selected mesh: clears "
            "all bone weights and unbinds the rig, turning it back into a plain "
            "static mesh so auto_rig can regenerate a skeleton from scratch. "
            "Skeletal animations are removed with the skeleton (morph/pose clips "
            "survive). Undoable in the GUI session.",
            props
        );
    }

    // add_arkit_blendshapes (#889)
    {
        QJsonObject props;
        props["max_shapes"] = QJsonObject{{"type", "integer"},
            {"description",
             "Cap the number of ARKit shapes generated (0 = all 52 in the "
             "template, default)."}};
        props["max_residual_pct"] = QJsonObject{{"type", "number"},
            {"description",
             "Reject the rig when the non-rigid fit is worse than this percent of "
             "the mesh diagonal (default 8). A human template only fits a roughly "
             "human face; a non-face mesh blows past this and is refused."}};
        props["output_path"] = QJsonObject{{"type", "string"},
            {"description",
             "Optional path to re-export the mesh with the attached blendshapes. "
             "When omitted, the shapes are added to the in-session scene only."}};
        appendTool(
            "add_arkit_blendshapes",
            "Fit the ARKit blendshape template onto the currently selected FACE "
            "mesh and attach the 52 ARKit morph targets (#889). Native pipeline (no "
            "external deps): non-rigid ICP fits the template to the user face, then "
            "Sumner-Popovic deformation transfer realizes each expression on the "
            "user's identity; the shapes are named per the mocap-52 vocabulary so "
            "face capture drives them. Humanoid faces only — a poor fit is rejected. "
            "The bundled template downloads on first use.",
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
        appendTool(
            "cloud_limits",
            "Return server-reported QtMesh Cloud upload size limits for the signed-in account.",
            QJsonObject{},
            QJsonArray{});
        QJsonObject loginProps;
        loginProps["api_key"] = QJsonObject{
            {"type", "string"},
            {"description", "Bearer token / API key to store in per-user QSettings. Device flow is CLI-only."}};
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
        uploadProps["include"] = QJsonObject{
            {"type", "array"},
            {"items", QJsonObject{{"type", "string"}}},
            {"description", "Optional glob patterns limiting which dependency files are packaged."}};
        uploadProps["exclude"] = QJsonObject{
            {"type", "array"},
            {"items", QJsonObject{{"type", "string"}}},
            {"description", "Optional glob patterns excluding dependency files from the package."}};
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

    // capture_face_from_video (epic #869, Slice D #873)
    {
        QJsonObject props;
        props["video_path"] = QJsonObject{{"type", "string"}, {"description", "Path to a video file of a face performance."}};
        props["entity_name"] = QJsonObject{{"type", "string"}, {"description", "Target entity (default: first/selected entity)."}};
        props["output_path"] = QJsonObject{{"type", "string"}, {"description", "Optional export path (e.g. out.glb) written after recording."}};
        props["clip_name"] = QJsonObject{{"type", "string"}, {"description", "Morph weight clip name (default FaceCap)."}};
        props["fps"] = QJsonObject{{"type", "number"}, {"description", "Capture rate; frames are decimated to this (default 30)."}};
        props["smooth"] = QJsonObject{{"type", "boolean"}, {"description", "One-Euro smoothing (default true)."}};
        props["map_path"] = QJsonObject{{"type", "string"}, {"description", "Optional JSON mapping-override sidecar."}};
        props["head"] = QJsonObject{{"type", "boolean"}, {"description", "Also record head pose (default true)."}};
        QJsonArray required;
        required.append("video_path");
        appendTool(
            "capture_face_from_video",
            "Performance capture: run the face-capture models over a video and record the "
            "expressions as morph-target weight keyframes (plus head rotation) on the entity, "
            "as ONE undoable clip. The entity needs ARKit-style blendshape morph targets "
            "(jawOpen, mouthSmileLeft, ...). Unmatched channels are reported, never dropped. "
            "Requires a build with ENABLE_MOCAP=ON; models download on first use.",
            props,
            required
        );
    }

    // capture_body_from_video (epic #869, Slice E #874)
    {
        QJsonObject props;
        props["video_path"] = QJsonObject{{"type", "string"}, {"description", "Path to a video of a full-body performance."}};
        props["entity_name"] = QJsonObject{{"type", "string"}, {"description", "Target skinned entity (default: first/selected)."}};
        props["output_path"] = QJsonObject{{"type", "string"}, {"description", "Optional export path written after recording."}};
        props["clip_name"] = QJsonObject{{"type", "string"}, {"description", "Skeletal clip name (default BodyCap)."}};
        props["fps"] = QJsonObject{{"type", "number"}, {"description", "Capture rate (default 30)."}};
        props["smooth"] = QJsonObject{{"type", "boolean"}, {"description", "One-Euro smoothing (default true)."}};
        props["algo"] = QJsonObject{{"type", "string"}, {"description", "sam3dbody (quality path; falls back while its checkpoints are gated) or pose-ik."}};
        QJsonArray required;
        required.append("video_path");
        appendTool(
            "capture_body_from_video",
            "Performance capture: track a person in a video and record the full-body pose as a "
            "skeletal animation on the entity's humanoid rig (rotation-only, root locked, "
            "ONE undoable clip). Needs a skinned mesh resolving at least half of the 22 "
            "canonical roles. Requires a build with ENABLE_MOCAP=ON; models download on first use.",
            props,
            required
        );
    }

    // live capture (epic #869, Slice F #875) — GUI-attached sessions only
    {
        QJsonObject props;
        appendTool(
            "list_capture_devices",
            "List the available camera devices for live performance capture "
            "(id + description). Requires a build with ENABLE_MOCAP=ON.",
            props, QJsonArray());
    }
    {
        QJsonObject props;
        props["device_id"] = QJsonObject{{"type", "string"}, {"description", "Camera id from list_capture_devices (default camera when omitted)."}};
        props["video_path"] = QJsonObject{{"type", "string"}, {"description", "Drive from a VIDEO FILE instead of the camera (the macOS-camera-blocked path). Local path, not a URL."}};
        props["face"] = QJsonObject{{"type", "boolean"}, {"description", "Enable Face (morph-target) drive. Default: leave current toggle."}};
        props["head"] = QJsonObject{{"type", "boolean"}, {"description", "Enable Head-bone drive."}};
        props["body"] = QJsonObject{{"type", "boolean"}, {"description", "Enable full-body drive (humanoid rig)."}};
        QJsonArray required;
        appendTool(
            "start_live_capture",
            "Start a live performance-capture preview driving the SELECTED entity's morph "
            "targets + Head bone + (humanoid) body — the GUI Performance Capture panel's "
            "session. Source is the webcam (device_id) or a video file (video_path). The "
            "face/head/body flags toggle channels before starting. Only works in --with-mcp "
            "(GUI) mode. Follow with stop_live_capture.",
            props, required);
    }
    {
        QJsonObject props;
        props["face"] = QJsonObject{{"type", "boolean"}, {"description", "Enable/disable the Face (morph-target) channel."}};
        props["head"] = QJsonObject{{"type", "boolean"}, {"description", "Enable/disable the Head-bone channel."}};
        props["body"] = QJsonObject{{"type", "boolean"}, {"description", "Enable/disable the full-body channel."}};
        appendTool(
            "set_capture_channels",
            "Set the Face/Head/Body capture channel toggles for the SELECTED entity (the "
            "Performance Capture checkboxes). Returns each flag plus what the selection can "
            "actually drive (matchedChannels, headAvailable, bodyAvailable). Set BEFORE "
            "start_live_capture, or between sessions.",
            props, QJsonArray());
    }
    {
        QJsonObject props;
        props["record"] = QJsonObject{{"type", "boolean"}, {"description", "true: stop and commit the recording in progress (if any) before stopping the preview."}};
        appendTool(
            "stop_live_capture",
            "Stop the live performance-capture preview started by start_live_capture, "
            "restoring the entity's prior state exactly.",
            props, QJsonArray());
    }

    // import_alembic
    {
        QJsonObject props;
        props["file"] = QJsonObject{{"type", "string"}, {"description", "Path to an Alembic (.abc) vertex cache."}};
        QJsonArray required;
        required.append("file");
        appendTool(
            "import_alembic",
            "Import an Alembic (.abc) vertex cache into the live scene. Decodes the first "
            "animated polymesh into a fixed-topology frame set and builds a VAT_POSE-animated "
            "Ogre entity (cloth/sim/fluid bakes from Houdini/Blender). Returns the created node, "
            "entities, and vertex-animation clip names — drive them with play_vertex_animation. "
            "Requires a build with ENABLE_ALEMBIC=ON.",
            props,
            required
        );
    }

    // play_vertex_animation
    {
        QJsonObject props;
        props["entity"]    = QJsonObject{{"type", "string"}, {"description", "Entity name (from import_alembic)."}};
        props["animation"] = QJsonObject{{"type", "string"}, {"description", "Vertex-animation clip name."}};
        props["play"]      = QJsonObject{{"type", "boolean"}, {"description", "true to play (default), false to stop."}};
        props["loop"]      = QJsonObject{{"type", "boolean"}, {"description", "Loop the clip (default true)."}};
        QJsonArray required;
        required.append("entity");
        required.append("animation");
        appendTool(
            "play_vertex_animation",
            "Play / stop a vertex-animation clip on a live entity. The clip surfaces as an "
            "ordinary Ogre::AnimationState, so this behaves like play_animation for skeletal "
            "clips. Returns an error if the entity or clip is not found.",
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

    // set_node_animation_playing
    {
        QJsonObject props;
        props["clip"]    = QJsonObject{{"type", "string"}, {"description", "Clip name."}};
        props["enabled"] = QJsonObject{{"type", "boolean"}, {"description", "true to play the clip, false to pause. A paused clip leaves its nodes editable."}};
        QJsonArray required; required.append("clip"); required.append("enabled");
        appendTool("set_node_animation_playing",
            "Play or pause a node-transform clip. Node clips play from their own "
            "toggle, independent of the global skeletal Play button.",
            props, required);
    }

    // delete_node_animation_clip
    {
        QJsonObject props;
        props["clip"] = QJsonObject{{"type", "string"}, {"description", "Clip name to delete."}};
        QJsonArray required; required.append("clip");
        appendTool("delete_node_animation_clip",
            "Delete a node-transform clip and its driving AnimationState.",
            props, required);
    }

    // move_node_keyframe
    {
        QJsonObject props;
        props["clip"]     = QJsonObject{{"type", "string"}, {"description", "Clip name."}};
        props["node"]     = QJsonObject{{"type", "string"}, {"description", "Animated SceneNode name."}};
        props["old_time"] = QJsonObject{{"type", "number"}, {"description", "Existing keyframe time (seconds)."}};
        props["new_time"] = QJsonObject{{"type", "number"}, {"description", "New time (seconds, 0..length, no existing key there)."}};
        QJsonArray required; required.append("clip"); required.append("node"); required.append("old_time"); required.append("new_time");
        appendTool("move_node_keyframe",
            "Re-time a node keyframe, preserving its TRS. Rejected if there is no key "
            "at old_time or a key already sits at new_time.",
            props, required);
    }

    // delete_node_keyframe
    {
        QJsonObject props;
        props["clip"] = QJsonObject{{"type", "string"}, {"description", "Clip name."}};
        props["node"] = QJsonObject{{"type", "string"}, {"description", "Animated SceneNode name."}};
        props["time"] = QJsonObject{{"type", "number"}, {"description", "Time (seconds) of the keyframe to delete (nearest within 1ms)."}};
        QJsonArray required; required.append("clip"); required.append("node"); required.append("time");
        appendTool("delete_node_keyframe",
            "Delete the node keyframe nearest `time`.", props, required);
    }

    // get_node_animation
    {
        QJsonObject props;
        props["clip"] = QJsonObject{{"type", "string"}, {"description", "Clip name to inspect."}};
        QJsonArray required; required.append("clip");
        appendTool("get_node_animation",
            "Inspect a node clip: its length, whether it's playing, and each animated "
            "node with its keyframe times. Complements list_node_animations.",
            props, required);
    }

    // set_playback_speed
    {
        QJsonObject props;
        props["speed"] = QJsonObject{{"type", "number"}, {"description", "Playback speed multiplier (>0). 1.0 = real time, 2.0 = double speed."}};
        QJsonArray required; required.append("speed");
        appendTool("set_playback_speed",
            "Set the global animation playback speed multiplier (applies to all playing clips).",
            props, required);
    }

    // set_loop_region
    {
        QJsonObject props;
        props["start"]  = QJsonObject{{"type", "number"}, {"description", "Loop start time (seconds). Optional."}};
        props["end"]    = QJsonObject{{"type", "number"}, {"description", "Loop end time (seconds). Optional."}};
        props["active"] = QJsonObject{{"type", "boolean"}, {"description", "Enable/disable looping over [start,end]. Optional."}};
        appendTool("set_loop_region",
            "Set the playback loop region for the selected clip (start/end seconds) and "
            "toggle it on/off. All fields optional; omitted fields keep their value.",
            props);
    }

    // get_playback_state
    {
        QJsonObject props;
        appendTool("get_playback_state",
            "Read the current playback state: speed, loop region, current time, clip "
            "length, and the selected entity/animation/bone. No args.",
            props);
    }

    // select_animation
    {
        QJsonObject props;
        props["entity"]    = QJsonObject{{"type", "string"}, {"description", "Entity name owning the animation."}};
        props["animation"] = QJsonObject{{"type", "string"}, {"description", "Animation (clip) name on that entity."}};
        QJsonArray required; required.append("entity"); required.append("animation");
        appendTool("select_animation",
            "Select the active entity+animation for keyframe editing and playback. "
            "Use list_skeletal_animations / get_animation_info to enumerate.",
            props, required);
    }

    // select_bone
    {
        QJsonObject props;
        props["bone"] = QJsonObject{{"type", "string"}, {"description", "Bone name to make active for keyframe editing."}};
        QJsonArray required; required.append("bone");
        appendTool("select_bone",
            "Select the active bone for per-bone keyframe editing (add_keyframe / "
            "remove_keyframe operate on the selected bone).",
            props, required);
    }

    // set_morph_weight_keyframe
    {
        QJsonObject props;
        props["target"] = QJsonObject{{"type", "string"}, {"description", "Morph target (blend shape) name."}};
        props["time"]   = QJsonObject{{"type", "number"}, {"description", "Keyframe time in seconds (>= 0)."}};
        props["weight"] = QJsonObject{{"type", "number"}, {"description", "Weight at this time (typically 0..1)."}};
        QJsonArray required; required.append("target"); required.append("time"); required.append("weight");
        appendTool("set_morph_weight_keyframe",
            "Key a morph target's weight at a time on the shared MorphAnim clip so the "
            "blend shape animates over time (set_morph_weight is instantaneous only).",
            props, required);
    }

    // clear_morph_weight_keyframe
    {
        QJsonObject props;
        props["target"] = QJsonObject{{"type", "string"}, {"description", "Morph target name."}};
        props["time"]   = QJsonObject{{"type", "number"}, {"description", "Time (seconds) of the weight key to remove."}};
        QJsonArray required; required.append("target"); required.append("time");
        appendTool("clear_morph_weight_keyframe",
            "Remove a morph weight keyframe at the given time.", props, required);
    }

    // set_keyframe_value
    {
        QJsonObject props;
        props["bone"]    = QJsonObject{{"type", "string"}, {"description", "Bone whose keyframe to edit (select_animation first)."}};
        props["channel"] = QJsonObject{{"type", "string"}, {"description", "One of tx,ty,tz (translation), rw,rx,ry,rz (rotation quat), sx,sy,sz (scale)."}};
        props["time"]    = QJsonObject{{"type", "number"}, {"description", "Keyframe time in seconds (must already exist)."}};
        props["value"]   = QJsonObject{{"type", "number"}, {"description", "New value for that single channel."}};
        QJsonArray required; required.append("bone"); required.append("channel"); required.append("time"); required.append("value");
        appendTool("set_keyframe_value",
            "Write one channel of a bone keyframe (leaving the other 9 untouched), "
            "undoable. The keyframe must already exist at `time` — use add_keyframe first.",
            props, required);
    }

    // move_bone_keyframe
    {
        QJsonObject props;
        props["bone"]     = QJsonObject{{"type", "string"}, {"description", "Bone whose keyframe to move."}};
        props["old_time"] = QJsonObject{{"type", "number"}, {"description", "Existing keyframe time (seconds)."}};
        props["new_time"] = QJsonObject{{"type", "number"}, {"description", "New time (seconds), no existing key there."}};
        QJsonArray required; required.append("bone"); required.append("old_time"); required.append("new_time");
        appendTool("move_bone_keyframe",
            "Re-time a skeletal keyframe on the selected animation. Rejected on "
            "collision or when no key exists at old_time.",
            props, required);
    }

    // step_keyframe
    {
        QJsonObject props;
        props["direction"] = QJsonObject{{"type", "string"}, {"description", "'next' or 'prev' — moves the playhead to the adjacent keyframe."}};
        QJsonArray required; required.append("direction");
        appendTool("step_keyframe",
            "Move the playhead to the next or previous keyframe of the selected "
            "animation. Returns the resulting time.",
            props, required);
    }

    // get_channel_values
    {
        QJsonObject props;
        props["bone"]    = QJsonObject{{"type", "string"}, {"description", "Bone to read."}};
        props["channel"] = QJsonObject{{"type", "string"}, {"description", "Channel id (tx..sz)."}};
        QJsonArray required; required.append("bone"); required.append("channel");
        appendTool("get_channel_values",
            "Read a bone channel's value at every keyframe of the selected animation, "
            "in time order. Use for inspecting or plotting a curve.",
            props, required);
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

#ifdef ENABLE_PS1_RIP
    // PS1 runtime ripper (#412): drive-and-verify a capture headlessly.
    {
        QJsonObject schema;
        schema["type"] = "object";
        QJsonObject props;
        props["bios_path"] = QJsonObject{{"type", "string"},
            {"description", "Path to the PS1 BIOS (e.g. scph5501.bin)."}};
        props["iso_path"] = QJsonObject{{"type", "string"},
            {"description", "Path to the disc image (.cue/.bin/.chd/.iso). .cue/.chd boot most reliably."}};
        props["boot_timeout_ms"] = QJsonObject{{"type", "integer"},
            {"description", "Boot timeout ms (default 30000)."}};
        schema["properties"] = props;
        tools.append(buildToolDefinition(
            "ps1rip_start",
            "Start the PS1 runtime ripper on a BIOS + disc image and wait for an active session. "
            "Loads bios_path/iso_path if given. Returns session status incl. whether the "
            "in-core rip fork registered. Requires the beetle_psx_qtmesh_libretro core in PS1Cores/.",
            schema));
    }
    {
        QJsonObject schema; schema["type"] = "object"; schema["properties"] = QJsonObject();
        tools.append(buildToolDefinition("ps1rip_stop", "Stop the running PS1 emulation session.",
                                         schema));
    }
    {
        QJsonObject schema; schema["type"] = "object"; schema["properties"] = QJsonObject();
        tools.append(buildToolDefinition("ps1rip_status",
            "Report PS1 ripper session status (active, armed, core id, in-core hooks).", schema));
    }
    {
        QJsonObject schema; schema["type"] = "object";
        QJsonObject props;
        props["frames"] = QJsonObject{{"type", "integer"},
            {"description", "Approx frames of gameplay to advance (default 600 ~10s). The emulator "
                            "runs uncapped, so this is wall-clock-approximate, not exact."}};
        props["auto_input"] = QJsonObject{{"type", "boolean"},
            {"description", "Mash START/X through intros & menus to reach a 3D scene (default false)."}};
        schema["properties"] = props;
        tools.append(buildToolDefinition(
            "ps1rip_run_frames",
            "Advance the running game (let it play) so it reaches an in-game 3D scene before capture. "
            "Optionally auto-presses START/X to get past logos and menus.",
            schema));
    }
    {
        QJsonObject schema; schema["type"] = "object";
        QJsonObject props;
        props["tracked_only"] = QJsonObject{{"type", "boolean"},
            {"description", "Clean-up filter: keep only in-core tracked+depth 3D geometry, drop "
                            "HUD/sprite/2D screen-space prims (default false)."}};
        props["smooth"] = QJsonObject{{"type", "boolean"},
            {"description", "Weld duplicate vertices + recompute smoothed normals (default false)."}};
        props["remove_zero_area"] = QJsonObject{{"type", "boolean"},
            {"description", "Clean-up: drop zero-area (collinear / duplicate-vertex) sliver "
                            "triangles common in raw PS1 captures (default false)."}};
        props["merge_objects"] = QJsonObject{{"type", "boolean"},
            {"description", "Merge the per-frame sparse parts of the same object (scene "
                            "captures) into one mesh: clusters tracked groups by object-space "
                            "vertex overlap, unions triangles across frames, drops the "
                            "once-per-frame repeats (vertex-animated objects collapse to their "
                            "best single frame). Needs in-core tracked capture. Default TRUE — "
                            "set false to keep the raw per-frame parts."}};
        props["rigid_animation"] = QJsonObject{{"type", "boolean"},
            {"description", "Scene capture only: extract per-object rigid animation from the "
                            "per-frame GTE matrices and author node tracks that play in the "
                            "editor (in-editor preview; node tracks don't export yet). "
                            "Default false. Use with scene_seconds > 0."}};
        props["scene_seconds"] = QJsonObject{{"type", "integer"},
            {"description", "If > 0, accumulate a multi-second scene capture instead of one frame."}};
        props["timeout_ms"] = QJsonObject{{"type", "integer"},
            {"description", "Build timeout ms (default 30000)."}};
        schema["properties"] = props;
        tools.append(buildToolDefinition(
            "ps1rip_capture",
            "Capture the current frame (or a scene), reconstruct meshes, and return the tier "
            "breakdown (tracked%/depth%, unique meshes, triangles, slabLike). Arms capture "
            "automatically. Blocks until the mesh is built.",
            schema));
    }
    {
        QJsonObject schema; schema["type"] = "object"; schema["properties"] = QJsonObject();
        tools.append(buildToolDefinition("ps1rip_stats",
            "Aggregate the captured PS1 meshes currently in the scene (nodes, entities, "
            "triangles) straight from Ogre.", schema));
    }
    {
        QJsonObject schema; schema["type"] = "object"; schema["properties"] = QJsonObject();
        tools.append(buildToolDefinition("ps1rip_clear",
            "Remove all captured PS1 meshes (live preview + promoted) from the scene and clear "
            "the inspector list.", schema));
    }
#endif

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
