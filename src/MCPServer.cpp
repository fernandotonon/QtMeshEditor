#include "MCPServer.h"
#include "mainwindow.h"
#include "Manager.h"
#include "MaterialEditorQML.h"
#include "MaterialPresetLibrary.h"
#include "TextureChannelPacker.h"
#include "PrimitiveObject.h"
#include "SelectionSet.h"
#include "TransformOperator.h"
#include "MeshImporterExporter.h"
#include "OgreWidget.h"
#include "SpaceCamera.h"
#include "AnimationWidget.h"
#include "NormalVisualizer.h"
#include "MeshInfoOverlay.h"
#include "MeshValidator.h"
#include "MeshLodController.h"
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
        {QStringLiteral("load_mesh"), &MCPServer::toolLoadMesh},
        {QStringLiteral("get_mesh_info"), &MCPServer::toolGetMeshInfo},
        {QStringLiteral("transform_mesh"), &MCPServer::toolTransformMesh},
        {QStringLiteral("transform_submesh"), &MCPServer::toolTransformSubMesh},
        {QStringLiteral("list_textures"), &MCPServer::toolListTextures},
        {QStringLiteral("set_texture"), &MCPServer::toolSetTexture},
        {QStringLiteral("export_mesh"), &MCPServer::toolExportMesh},
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
        {QStringLiteral("pack_textures"), &MCPServer::toolPackTextures}
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
        QStringLiteral("open_scene")
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

    // Lazily initialize Ogre/Manager on first tool call
    if (!ensureOgreInitialized()) {
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

QJsonObject MCPServer::toolCreateMaterial(const QJsonObject &args)
{
    QString name = args["name"].toString();

    if (name.isEmpty()) {
        return makeErrorResult("Error: Material name is required");
    }

    try {
        // Check if material already exists
        Ogre::MaterialPtr existing = Ogre::MaterialManager::getSingleton().getByName(name.toStdString());
        if (existing) {
            return makeErrorResult(QString("Error: Material '%1' already exists").arg(name));
        }

        // Create the material programmatically
        Ogre::MaterialPtr mat = Ogre::MaterialManager::getSingleton().create(
            name.toStdString(), Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

        // Accept colors as either top-level params (diffuse, ambient …) or
        // nested under a "colors" object — both formats are valid.
        auto resolveColor = [&](const QString& key) -> QJsonArray {
            if (args.contains(key) && args[key].isArray())
                return args[key].toArray();
            QJsonObject nested = args["colors"].toObject();
            if (nested.contains(key) && nested[key].isArray())
                return nested[key].toArray();
            return {};
        };
        auto resolveNumber = [&](const QString& key, double def) -> double {
            if (args.contains(key)) return args[key].toDouble(def);
            return args["colors"].toObject().value(key).toDouble(def);
        };

        Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);

        QJsonArray amb = resolveColor("ambient");
        if (!amb.isEmpty())
            pass->setAmbient(amb[0].toDouble(0.2), amb[1].toDouble(0.2), amb[2].toDouble(0.2));
        else
            pass->setAmbient(0.2, 0.2, 0.2);

        QJsonArray diff = resolveColor("diffuse");
        if (!diff.isEmpty())
            pass->setDiffuse(diff[0].toDouble(1.0), diff[1].toDouble(1.0), diff[2].toDouble(1.0), 1.0);

        QJsonArray spec = resolveColor("specular");
        if (!spec.isEmpty()) {
            pass->setSpecular(spec[0].toDouble(0.5), spec[1].toDouble(0.5), spec[2].toDouble(0.5), 1.0);
            pass->setShininess(resolveNumber("shininess", 32.0));
        } else {
            pass->setSpecular(0.5, 0.5, 0.5, 1.0);
            pass->setShininess(32.0);
        }

        QJsonArray emis = resolveColor("emissive");
        if (!emis.isEmpty())
            pass->setSelfIllumination(emis[0].toDouble(), emis[1].toDouble(), emis[2].toDouble());

        try { mat->load(); } catch (...) { /* headless — no GPU context */ }


        // Serialize the created material for display
        Ogre::MaterialSerializer serializer;
        serializer.queueForExport(mat);
        QString materialScript = QString::fromStdString(serializer.getQueuedAsString());

        return makeSuccessResult(QString("Created material '%1':\n%2").arg(name).arg(materialScript));

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Ogre error creating material: %1").arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolModifyMaterial(const QJsonObject &args)
{
    QString name = args["name"].toString();

    if (name.isEmpty()) {
        return makeErrorResult("Error: Material name is required");
    }

    // Try to get the material from Ogre
    try {
        Ogre::MaterialPtr material = Ogre::MaterialManager::getSingleton().getByName(name.toStdString());
        if (!material) {
            return makeErrorResult(QString("Error: Material '%1' not found").arg(name));
        }

        // Get the first technique and pass
        if (material->getNumTechniques() == 0) {
            return makeErrorResult(QString("Error: Material '%1' has no techniques").arg(name));
        }

        Ogre::Technique* technique = material->getTechnique(0);
        if (technique->getNumPasses() == 0) {
            return makeErrorResult(QString("Error: Material '%1' technique has no passes").arg(name));
        }

        Ogre::Pass* pass = technique->getPass(0);

        QStringList modifications;

        // Apply modifications
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

        return makeSuccessResult(QString("Modified material '%1':\n%2").arg(name).arg(modifications.join("\n")));

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolGetMaterial(const QJsonObject &args)
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

        // Serialize the material to script text
        Ogre::MaterialSerializer serializer;
        serializer.queueForExport(material);
        QString script = QString::fromStdString(serializer.getQueuedAsString());

        return makeSuccessResult(QString("Material '%1' script:\n%2").arg(name).arg(script));

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
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
        return makeSuccessResult(QString("Available materials (%1):\n%2").arg(materials.size()).arg(materials.join("\n")));
    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
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
        // Verify material exists
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
            bool found = false;

            // Primary: search by entity name via getEntities()
            QList<Ogre::Entity*>& entities = mgr->getEntities();
            for (Ogre::Entity* entity : entities) {
                if (entity && QString::fromStdString(entity->getName()) == meshName) {
                    entity->setMaterialName(materialName.toStdString());
                    appliedTo << QString::fromStdString(entity->getName());
                    found = true;
                    break;
                }
            }

            // Fallback: look up scene node by name and apply to its attached entity.
            // Handles cases where the entity name differs from the node name, or
            // getEntities() returns an incomplete / mis-cast list.
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
            // Apply to selected entities
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

        return makeSuccessResult(QString("Applied material '%1' to: %2").arg(materialName).arg(appliedTo.join(", ")));

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
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

QJsonObject MCPServer::toolLoadMesh(const QJsonObject &args)
{
    QString path = args["path"].toString();

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

    } catch (std::exception& e) {
        return makeErrorResult(QString("Error loading mesh: %1").arg(e.what()));
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

            // Count vertices and indices
            unsigned int totalVertices = 0;
            unsigned int totalIndices = 0;
            unsigned int numSubMeshes = mesh->getNumSubMeshes();

            for (unsigned int i = 0; i < numSubMeshes; ++i) {
                Ogre::SubMesh* subMesh = mesh->getSubMesh(i);
                if (subMesh->vertexData)
                    totalVertices += subMesh->vertexData->vertexCount;
                if (subMesh->indexData)
                    totalIndices += subMesh->indexData->indexCount;
            }
            // Shared vertex data
            if (mesh->sharedVertexData)
                totalVertices += mesh->sharedVertexData->vertexCount;

            // Get materials for sub-entities
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

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
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

        // If a name is provided, find and select that node first
        QString name = args["name"].toString();
        Ogre::SceneNode* targetNode = nullptr;
        if (!name.isEmpty()) {
            targetNode = findSceneNodeByName(name);
            if (!targetNode) {
                return makeErrorResult(QString("Error: Node '%1' not found").arg(name));
            }
        } else {
            // No name given - require something selected
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
            .arg(QString::fromStdString(targetNode->getName()))
            .arg(transforms.join("\n")));

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolTransformSubMesh(const QJsonObject &args)
{
    try {
        if (!Manager::getSingletonPtr())
            return makeErrorResult("Error: Manager not available");

        QString entityName = args["entity_name"].toString();
        if (entityName.isEmpty())
            return makeErrorResult("Error: entity_name is required");

        int subIdx = args["submesh_index"].toInt(-1);
        if (subIdx < 0)
            return makeErrorResult("Error: submesh_index must be a non-negative integer");

        Ogre::Entity* entity = findEntityByName(entityName);
        if (!entity)
            return makeErrorResult(QString("Error: Entity '%1' not found").arg(entityName));

        unsigned int uSubIdx = static_cast<unsigned int>(subIdx);
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

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
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

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolSetTexture(const QJsonObject &args)
{
    QString materialName = args["material"].toString();
    QString texturePath = args["texture"].toString();
    int textureUnit = args["unit"].toInt(0);

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
            .arg(texturePath).arg(materialName).arg(textureUnit));

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolExportMesh(const QJsonObject &args)
{
    QString path = args["path"].toString();
    QString format = args["format"].toString("Ogre Mesh (*.mesh)");

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

        int exportResult = MeshImporterExporter::exporter(node, path, format);
        if (exportResult == 0) {
            return makeSuccessResult(QString("Exported mesh to: %1 (format: %2)").arg(path).arg(format));
        } else {
            return makeSuccessResult(QString("Export completed to: %1 (format: %2), result code: %3").arg(path).arg(format).arg(exportResult));
        }

    } catch (std::exception& e) {
        return makeErrorResult(QString("Error exporting mesh: %1").arg(e.what()));
    }
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
        QList<Ogre::SceneNode*> nodes = mgr->getSceneNodes();

        // Count materials
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

        QString sceneInfo = QString(
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
    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
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

        float yaw = static_cast<float>(args["yaw"].toDouble(0));
        float pitch = static_cast<float>(args["pitch"].toDouble(0));
        float roll = static_cast<float>(args["roll"].toDouble(0));

        if (yaw == 0 && pitch == 0 && roll == 0) {
            yaw = 45; // default: 45 degrees/sec around Y
        }

        NodeAnimation anim;
        anim.node = targetNode;
        anim.yawSpeed = yaw;
        anim.pitchSpeed = pitch;
        anim.rollSpeed = roll;
        m_animations[name] = anim;

        // Create and start the animation timer if needed
        if (!m_animationTimer) {
            m_animationTimer = new QTimer(this);
            connect(m_animationTimer, &QTimer::timeout, this, &MCPServer::onAnimationTick);
        }
        if (!m_animationTimer->isActive()) {
            m_animationTimer->start(16); // ~60fps
        }

        return makeSuccessResult(QString("Started animation on '%1' (yaw: %2, pitch: %3, roll: %4 deg/sec)")
            .arg(name).arg(yaw).arg(pitch).arg(roll));

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
    }
}

QJsonObject MCPServer::toolListSkeletalAnimations(const QJsonObject &args)
{
    Q_UNUSED(args);

    try {
        Manager* mgr = Manager::getSingletonPtr();
        if (!mgr) return makeErrorResult("Error: Manager not available");

        QStringList infoLines;
        QList<Ogre::SceneNode*> nodes = mgr->getSceneNodes();
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

    } catch (Ogre::Exception& e) {
        return makeErrorResult(QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
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
    return sel && sel->hasEntities();
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

    QString errorMsg = captureLodControllerError([&]() {
        MeshLodController::instance()->generateLods(count, reductions);
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
        props["preset"] = QJsonObject{{"type", "string"}, {"description", "Tolerance preset: 'conservative' (~0.1mm/0.05°), 'balanced' (~1mm/0.5°, default), or 'aggressive' (~1cm/1°). Higher tolerance removes more keys."}};
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
        props["preset"] = QJsonObject{{"type", "string"}, {"description", "Tolerance preset: 'conservative', 'balanced' (default), or 'aggressive'."}};
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
        appendTool(
            "generate_lods",
            "Generate LOD (Level of Detail) levels for the selected mesh, reducing polygon count at distance. "
            "Specify count (1–4) and optional per-level reduction ratios. "
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
