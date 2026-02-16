#include "MCPServer.h"
#include "mainwindow.h"
#include "Manager.h"
#include "MaterialEditorQML.h"
#include "PrimitiveObject.h"
#include "SelectionSet.h"
#include "TransformOperator.h"
#include "MeshImporterExporter.h"
#include "OgreWidget.h"
#include <QDebug>
#include <QFile>
#include <QDir>
#include <QTemporaryFile>
#include <QImage>
#include <QBuffer>
#include <QTimer>
#include <QDateTime>
#include <QMetaObject>
#include <QPixmap>
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

#ifdef Q_OS_WIN
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#endif

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

void MCPServer::setMainWindow(MainWindow *mainWindow)
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
    if (!m_running) return;

    if (m_stdinNotifier) {
        m_stdinNotifier->setEnabled(false);
        delete m_stdinNotifier;
        m_stdinNotifier = nullptr;
    }

    m_running = false;
    qDebug() << "MCP Server stopped";
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

QJsonObject MCPServer::callTool(const QString &name, const QJsonObject &args)
{
    qDebug() << "MCP Tool Call:" << name << args;

    // Lazily initialize Ogre/Manager on first tool call
    if (!ensureOgreInitialized()) {
        return makeErrorResult("Error: Ogre 3D engine could not be initialized (no OpenGL available)");
    }

    QJsonObject toolResult;

    // Dispatch to appropriate tool handler
    if (name == "create_material") {
        toolResult = toolCreateMaterial(args);
    } else if (name == "modify_material") {
        toolResult = toolModifyMaterial(args);
    } else if (name == "get_material") {
        toolResult = toolGetMaterial(args);
    } else if (name == "list_materials") {
        toolResult = toolListMaterials(args);
    } else if (name == "apply_material") {
        toolResult = toolApplyMaterial(args);
    } else if (name == "load_mesh") {
        toolResult = toolLoadMesh(args);
    } else if (name == "get_mesh_info") {
        toolResult = toolGetMeshInfo(args);
    } else if (name == "transform_mesh") {
        toolResult = toolTransformMesh(args);
    } else if (name == "list_textures") {
        toolResult = toolListTextures(args);
    } else if (name == "set_texture") {
        toolResult = toolSetTexture(args);
    } else if (name == "export_mesh") {
        toolResult = toolExportMesh(args);
    } else if (name == "get_scene_info") {
        toolResult = toolGetSceneInfo(args);
    } else if (name == "take_screenshot") {
        toolResult = toolTakeScreenshot(args);
    } else if (name == "create_primitive") {
        toolResult = toolCreatePrimitive(args);
    } else if (name == "animate") {
        toolResult = toolAnimate(args);
    } else {
        return makeErrorResult(QString("Unknown tool: %1").arg(name));
    }

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

        // Set properties from colors
        QJsonObject colors = args["colors"].toObject();
        Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);

        if (colors.contains("ambient")) {
            QJsonArray a = colors["ambient"].toArray();
            pass->setAmbient(a[0].toDouble(0.2), a[1].toDouble(0.2), a[2].toDouble(0.2));
        } else {
            pass->setAmbient(0.2, 0.2, 0.2);
        }

        if (colors.contains("diffuse")) {
            QJsonArray d = colors["diffuse"].toArray();
            pass->setDiffuse(d[0].toDouble(1.0), d[1].toDouble(1.0), d[2].toDouble(1.0), 1.0);
        }

        if (colors.contains("specular")) {
            QJsonArray s = colors["specular"].toArray();
            double shininess = colors.value("shininess").toDouble(32.0);
            pass->setSpecular(s[0].toDouble(0.5), s[1].toDouble(0.5), s[2].toDouble(0.5), 1.0);
            pass->setShininess(shininess);
        } else {
            pass->setSpecular(0.5, 0.5, 0.5, 1.0);
            pass->setShininess(32.0);
        }

        if (colors.contains("emissive")) {
            QJsonArray e = colors["emissive"].toArray();
            pass->setSelfIllumination(e[0].toDouble(), e[1].toDouble(), e[2].toDouble());
        }

        mat->load();

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
    QString materialName = args["material"].toString();
    QString meshName = args["mesh"].toString();

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
            // Apply to specific entity by name
            QList<Ogre::Entity*>& entities = mgr->getEntities();
            bool found = false;
            for (Ogre::Entity* entity : entities) {
                if (entity && QString::fromStdString(entity->getName()) == meshName) {
                    entity->setMaterialName(materialName.toStdString());
                    appliedTo << QString::fromStdString(entity->getName());
                    found = true;
                    break;
                }
            }
            if (!found) {
                return makeErrorResult(QString("Error: Entity '%1' not found").arg(meshName));
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

QJsonObject MCPServer::toolLoadMesh(const QJsonObject &args)
{
    QString path = args["path"].toString();

    if (path.isEmpty()) {
        return makeErrorResult("Error: File path is required");
    }

    if (!m_mainWindow) {
        return makeErrorResult("Error: MainWindow not available. Run with --with-mcp flag for full functionality.");
    }

    if (!QFile::exists(path)) {
        return makeErrorResult(QString("Error: File not found: %1").arg(path));
    }

    try {
        m_mainWindow->importMeshs(QStringList{path});
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
        Manager* mgr = Manager::getSingletonPtr();
        if (!mgr) {
            return makeErrorResult("Error: Manager not available");
        }

        // If a name is provided, find and select that node first
        QString name = args["name"].toString();
        Ogre::SceneNode* targetNode = nullptr;
        if (!name.isEmpty()) {
            QList<Ogre::SceneNode*> nodes = mgr->getSceneNodes();
            for (Ogre::SceneNode* node : nodes) {
                if (node && QString::fromStdString(node->getName()) == name) {
                    targetNode = node;
                    break;
                }
            }
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
    QString format = args["format"].toString("mesh");

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

    return makeSuccessResult(QString("Created %1 primitive '%2'").arg(type).arg(name));
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
        Manager* mgr = Manager::getSingletonPtr();
        if (!mgr) {
            return makeErrorResult("Error: Manager not available");
        }

        Ogre::SceneNode* targetNode = nullptr;
        QList<Ogre::SceneNode*> nodes = mgr->getSceneNodes();
        for (Ogre::SceneNode* node : nodes) {
            if (node && QString::fromStdString(node->getName()) == name) {
                targetNode = node;
                break;
            }
        }

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

// Helper methods

QJsonArray MCPServer::buildToolsList()
{
    QJsonArray tools;

    // create_material
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        QJsonObject properties;
        properties["name"] = QJsonObject{{"type", "string"}, {"description", "Name of the material to create"}};
        properties["script"] = QJsonObject{{"type", "string"}, {"description", "Optional: Full Ogre3D material script"}};
        QJsonObject colors;
        colors["type"] = "object";
        colors["description"] = "Optional: Color values if not providing full script";
        properties["colors"] = colors;
        inputSchema["properties"] = properties;
        inputSchema["required"] = QJsonArray{"name"};

        tools.append(buildToolDefinition(
            "create_material",
            "Create a new Ogre3D material with specified properties",
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
            "Modify an existing material's properties (colors, texture, etc.)",
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
            "Get the full material script for a specific material",
            inputSchema
        ));
    }

    // list_materials
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        inputSchema["properties"] = QJsonObject();

        tools.append(buildToolDefinition(
            "list_materials",
            "List all available materials in the scene",
            inputSchema
        ));
    }

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
            "Apply a material to a mesh in the scene",
            inputSchema
        ));
    }

    // load_mesh
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        QJsonObject properties;
        properties["path"] = QJsonObject{{"type", "string"}, {"description", "Path to the mesh file to load"}};
        inputSchema["properties"] = properties;
        inputSchema["required"] = QJsonArray{"path"};

        tools.append(buildToolDefinition(
            "load_mesh",
            "Load a 3D mesh file into the editor",
            inputSchema
        ));
    }

    // get_mesh_info
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        inputSchema["properties"] = QJsonObject();

        tools.append(buildToolDefinition(
            "get_mesh_info",
            "Get information about the currently loaded mesh",
            inputSchema
        ));
    }

    // transform_mesh
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        QJsonObject properties;
        properties["position"] = QJsonObject{{"type", "array"}, {"description", "Position [X, Y, Z]"}};
        properties["rotation"] = QJsonObject{{"type", "array"}, {"description", "Rotation in degrees [X, Y, Z]"}};
        properties["scale"] = QJsonObject{{"type", "array"}, {"description", "Scale [X, Y, Z]"}};
        inputSchema["properties"] = properties;

        tools.append(buildToolDefinition(
            "transform_mesh",
            "Transform the mesh (position, rotation, scale)",
            inputSchema
        ));
    }

    // list_textures
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        inputSchema["properties"] = QJsonObject();

        tools.append(buildToolDefinition(
            "list_textures",
            "List all available textures",
            inputSchema
        ));
    }

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
            "Set a texture on a material",
            inputSchema
        ));
    }

    // export_mesh
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        QJsonObject properties;
        properties["path"] = QJsonObject{{"type", "string"}, {"description", "Output file path"}};
        properties["format"] = QJsonObject{{"type", "string"}, {"description", "Export format (mesh, obj, fbx, etc.)"}};
        inputSchema["properties"] = properties;
        inputSchema["required"] = QJsonArray{"path"};

        tools.append(buildToolDefinition(
            "export_mesh",
            "Export the current mesh to a file",
            inputSchema
        ));
    }

    // get_scene_info
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        inputSchema["properties"] = QJsonObject();

        tools.append(buildToolDefinition(
            "get_scene_info",
            "Get information about the current scene",
            inputSchema
        ));
    }

    // take_screenshot
    {
        QJsonObject inputSchema;
        inputSchema["type"] = "object";
        QJsonObject properties;
        properties["path"] = QJsonObject{{"type", "string"}, {"description", "Output file path (optional, uses temp if not specified)"}};
        inputSchema["properties"] = properties;

        tools.append(buildToolDefinition(
            "take_screenshot",
            "Take a screenshot of the current viewport",
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
            "Create a 3D primitive object in the scene",
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

void MCPServer::startHttp(int port)
{
    m_httpPort = port;
    m_httpServer = new QTcpServer(this);
    connect(m_httpServer, &QTcpServer::newConnection, this, &MCPServer::onHttpConnection);

    if (m_httpServer->listen(QHostAddress::Any, m_httpPort)) {
        qDebug() << "HTTP REST API listening on port" << m_httpPort;
    } else {
        qWarning() << "Failed to start HTTP server on port" << m_httpPort
                    << ":" << m_httpServer->errorString();
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
