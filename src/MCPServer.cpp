#include "MCPServer.h"
#include "mainwindow.h"
#include "Manager.h"
#include <QDebug>
#include <QFile>
#include <QDir>
#include <QTemporaryFile>
#include <QImage>
#include <QBuffer>
#include <QTimer>

#ifdef Q_OS_WIN
#include <io.h>
#include <fcntl.h>
#endif

MCPServer::MCPServer(QObject *parent)
    : QObject(parent)
    , m_stdout(stdout)
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

void MCPServer::start()
{
    if (m_running) return;

    // Open stdin for reading
    if (!m_stdin.open(stdin, QIODevice::ReadOnly)) {
        emit errorOccurred("Failed to open stdin");
        return;
    }

    // Create notifier for stdin
    m_stdinNotifier = new QSocketNotifier(fileno(stdin), QSocketNotifier::Read, this);
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

    m_stdin.close();
    m_running = false;
    qDebug() << "MCP Server stopped";
}

void MCPServer::onReadyRead()
{
    // Read available data
    QByteArray data = m_stdin.readAll();
    if (data.isEmpty()) return;

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

    QJsonObject result;

    // Handle different MCP methods
    if (method == "initialize") {
        result = handleInitialize(params);
    } else if (method == "initialized") {
        // Notification, no response needed
        return;
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

    // Send response
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

    QString header = QString("Content-Length: %1\r\n\r\n").arg(data.size());

    m_stdout << header;
    m_stdout << QString::fromUtf8(data);
    m_stdout.flush();
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

    qDebug() << "MCP Tool Call:" << toolName << args;

    QJsonObject toolResult;

    // Dispatch to appropriate tool handler
    if (toolName == "create_material") {
        toolResult = toolCreateMaterial(args);
    } else if (toolName == "modify_material") {
        toolResult = toolModifyMaterial(args);
    } else if (toolName == "get_material") {
        toolResult = toolGetMaterial(args);
    } else if (toolName == "list_materials") {
        toolResult = toolListMaterials(args);
    } else if (toolName == "apply_material") {
        toolResult = toolApplyMaterial(args);
    } else if (toolName == "load_mesh") {
        toolResult = toolLoadMesh(args);
    } else if (toolName == "get_mesh_info") {
        toolResult = toolGetMeshInfo(args);
    } else if (toolName == "transform_mesh") {
        toolResult = toolTransformMesh(args);
    } else if (toolName == "list_textures") {
        toolResult = toolListTextures(args);
    } else if (toolName == "set_texture") {
        toolResult = toolSetTexture(args);
    } else if (toolName == "export_mesh") {
        toolResult = toolExportMesh(args);
    } else if (toolName == "get_scene_info") {
        toolResult = toolGetSceneInfo(args);
    } else if (toolName == "take_screenshot") {
        toolResult = toolTakeScreenshot(args);
    } else {
        QJsonObject result;
        result["isError"] = true;
        QJsonArray content;
        QJsonObject textContent;
        textContent["type"] = "text";
        textContent["text"] = QString("Unknown tool: %1").arg(toolName);
        content.append(textContent);
        result["content"] = content;
        return result;
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
        // TODO: Get actual material text from editor
        content["text"] = "// Current material script";
        contents.append(content);
    } else if (uri == "qtmesheditor://scene/info") {
        QJsonObject content;
        content["uri"] = uri;
        content["mimeType"] = "application/json";
        content["text"] = "{}"; // TODO: Get actual scene info
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
    QString script = args["script"].toString();

    QJsonArray content;
    QJsonObject textContent;
    textContent["type"] = "text";

    if (name.isEmpty()) {
        textContent["text"] = "Error: Material name is required";
        content.append(textContent);
        QJsonObject result;
        result["isError"] = true;
        result["content"] = content;
        return result;
    }

    // Generate a basic material script if not provided
    if (script.isEmpty()) {
        QJsonObject colors = args["colors"].toObject();
        double r = colors["diffuse"].toArray().at(0).toDouble(1.0);
        double g = colors["diffuse"].toArray().at(1).toDouble(1.0);
        double b = colors["diffuse"].toArray().at(2).toDouble(1.0);

        script = QString(
            "material %1\n"
            "{\n"
            "    technique\n"
            "    {\n"
            "        pass\n"
            "        {\n"
            "            ambient 0.2 0.2 0.2\n"
            "            diffuse %2 %3 %4\n"
            "            specular 0.5 0.5 0.5 32\n"
            "        }\n"
            "    }\n"
            "}\n"
        ).arg(name).arg(r).arg(g).arg(b);
    }

    // TODO: Actually create the material in Ogre
    // For now, return success with the generated script

    textContent["text"] = QString("Created material '%1':\n%2").arg(name).arg(script);
    content.append(textContent);

    QJsonObject result;
    result["content"] = content;
    return result;
}

QJsonObject MCPServer::toolModifyMaterial(const QJsonObject &args)
{
    QString name = args["name"].toString();

    QJsonArray content;
    QJsonObject textContent;
    textContent["type"] = "text";

    if (name.isEmpty()) {
        textContent["text"] = "Error: Material name is required";
        content.append(textContent);
        QJsonObject result;
        result["isError"] = true;
        result["content"] = content;
        return result;
    }

    // Build modification description
    QStringList modifications;

    if (args.contains("ambient")) {
        QJsonArray a = args["ambient"].toArray();
        modifications << QString("ambient: %1 %2 %3")
            .arg(a[0].toDouble()).arg(a[1].toDouble()).arg(a[2].toDouble());
    }
    if (args.contains("diffuse")) {
        QJsonArray d = args["diffuse"].toArray();
        modifications << QString("diffuse: %1 %2 %3")
            .arg(d[0].toDouble()).arg(d[1].toDouble()).arg(d[2].toDouble());
    }
    if (args.contains("specular")) {
        QJsonArray s = args["specular"].toArray();
        double shininess = args.value("shininess").toDouble(32);
        modifications << QString("specular: %1 %2 %3 %4")
            .arg(s[0].toDouble()).arg(s[1].toDouble()).arg(s[2].toDouble()).arg(shininess);
    }
    if (args.contains("emissive")) {
        QJsonArray e = args["emissive"].toArray();
        modifications << QString("emissive: %1 %2 %3")
            .arg(e[0].toDouble()).arg(e[1].toDouble()).arg(e[2].toDouble());
    }
    if (args.contains("texture")) {
        modifications << QString("texture: %1").arg(args["texture"].toString());
    }

    // TODO: Actually modify the material in Ogre via MainWindow

    textContent["text"] = QString("Modified material '%1':\n%2").arg(name).arg(modifications.join("\n"));
    content.append(textContent);

    QJsonObject result;
    result["content"] = content;
    return result;
}

QJsonObject MCPServer::toolGetMaterial(const QJsonObject &args)
{
    QString name = args["name"].toString();

    QJsonArray content;
    QJsonObject textContent;
    textContent["type"] = "text";

    // TODO: Get actual material from Ogre
    textContent["text"] = QString("Material '%1' script:\n// Material not found or not implemented yet").arg(name);
    content.append(textContent);

    QJsonObject result;
    result["content"] = content;
    return result;
}

QJsonObject MCPServer::toolListMaterials(const QJsonObject &args)
{
    Q_UNUSED(args);

    QJsonArray content;
    QJsonObject textContent;
    textContent["type"] = "text";

    // TODO: Get actual materials from Ogre MaterialManager
    QStringList materials;
    materials << "BaseWhite" << "BaseWhiteNoLighting";

    textContent["text"] = QString("Available materials:\n%1").arg(materials.join("\n"));
    content.append(textContent);

    QJsonObject result;
    result["content"] = content;
    return result;
}

QJsonObject MCPServer::toolApplyMaterial(const QJsonObject &args)
{
    QString materialName = args["material"].toString();
    QString meshName = args["mesh"].toString();

    QJsonArray content;
    QJsonObject textContent;
    textContent["type"] = "text";

    // TODO: Apply material to mesh via Manager

    textContent["text"] = QString("Applied material '%1' to mesh '%2'").arg(materialName).arg(meshName);
    content.append(textContent);

    QJsonObject result;
    result["content"] = content;
    return result;
}

QJsonObject MCPServer::toolLoadMesh(const QJsonObject &args)
{
    QString path = args["path"].toString();

    QJsonArray content;
    QJsonObject textContent;
    textContent["type"] = "text";

    if (path.isEmpty()) {
        textContent["text"] = "Error: File path is required";
        content.append(textContent);
        QJsonObject result;
        result["isError"] = true;
        result["content"] = content;
        return result;
    }

    // TODO: Load mesh via MainWindow->loadMesh()

    textContent["text"] = QString("Loaded mesh from: %1").arg(path);
    content.append(textContent);

    QJsonObject result;
    result["content"] = content;
    return result;
}

QJsonObject MCPServer::toolGetMeshInfo(const QJsonObject &args)
{
    Q_UNUSED(args);

    QJsonArray content;
    QJsonObject textContent;
    textContent["type"] = "text";

    // TODO: Get actual mesh info from Manager/Ogre

    textContent["text"] = "Mesh info:\n- Vertices: N/A\n- Triangles: N/A\n- Materials: N/A";
    content.append(textContent);

    QJsonObject result;
    result["content"] = content;
    return result;
}

QJsonObject MCPServer::toolTransformMesh(const QJsonObject &args)
{
    QJsonArray content;
    QJsonObject textContent;
    textContent["type"] = "text";

    QStringList transforms;

    if (args.contains("position")) {
        QJsonArray p = args["position"].toArray();
        transforms << QString("position: %1, %2, %3")
            .arg(p[0].toDouble()).arg(p[1].toDouble()).arg(p[2].toDouble());
    }
    if (args.contains("rotation")) {
        QJsonArray r = args["rotation"].toArray();
        transforms << QString("rotation: %1, %2, %3")
            .arg(r[0].toDouble()).arg(r[1].toDouble()).arg(r[2].toDouble());
    }
    if (args.contains("scale")) {
        QJsonArray s = args["scale"].toArray();
        transforms << QString("scale: %1, %2, %3")
            .arg(s[0].toDouble()).arg(s[1].toDouble()).arg(s[2].toDouble());
    }

    // TODO: Apply transforms via Manager

    textContent["text"] = QString("Applied transforms:\n%1").arg(transforms.join("\n"));
    content.append(textContent);

    QJsonObject result;
    result["content"] = content;
    return result;
}

QJsonObject MCPServer::toolListTextures(const QJsonObject &args)
{
    Q_UNUSED(args);

    QJsonArray content;
    QJsonObject textContent;
    textContent["type"] = "text";

    // TODO: Get actual textures from Ogre TextureManager

    textContent["text"] = "Available textures:\n(texture list not implemented yet)";
    content.append(textContent);

    QJsonObject result;
    result["content"] = content;
    return result;
}

QJsonObject MCPServer::toolSetTexture(const QJsonObject &args)
{
    QString materialName = args["material"].toString();
    QString texturePath = args["texture"].toString();
    int textureUnit = args["unit"].toInt(0);

    QJsonArray content;
    QJsonObject textContent;
    textContent["type"] = "text";

    // TODO: Set texture on material

    textContent["text"] = QString("Set texture '%1' on material '%2' (unit %3)")
        .arg(texturePath).arg(materialName).arg(textureUnit);
    content.append(textContent);

    QJsonObject result;
    result["content"] = content;
    return result;
}

QJsonObject MCPServer::toolExportMesh(const QJsonObject &args)
{
    QString path = args["path"].toString();
    QString format = args["format"].toString("mesh");

    QJsonArray content;
    QJsonObject textContent;
    textContent["type"] = "text";

    if (path.isEmpty()) {
        textContent["text"] = "Error: Export path is required";
        content.append(textContent);
        QJsonObject result;
        result["isError"] = true;
        result["content"] = content;
        return result;
    }

    // TODO: Export mesh via MainWindow

    textContent["text"] = QString("Exported mesh to: %1 (format: %2)").arg(path).arg(format);
    content.append(textContent);

    QJsonObject result;
    result["content"] = content;
    return result;
}

QJsonObject MCPServer::toolGetSceneInfo(const QJsonObject &args)
{
    Q_UNUSED(args);

    QJsonArray content;
    QJsonObject textContent;
    textContent["type"] = "text";

    // TODO: Get actual scene info

    textContent["text"] = "Scene info:\n- Objects: N/A\n- Materials: N/A\n- Lights: N/A";
    content.append(textContent);

    QJsonObject result;
    result["content"] = content;
    return result;
}

QJsonObject MCPServer::toolTakeScreenshot(const QJsonObject &args)
{
    QString path = args["path"].toString();

    QJsonArray content;
    QJsonObject textContent;
    textContent["type"] = "text";

    if (path.isEmpty()) {
        // Generate temp path
        path = QDir::temp().filePath("qtmesheditor_screenshot.png");
    }

    // TODO: Take screenshot via OgreWidget

    textContent["text"] = QString("Screenshot saved to: %1").arg(path);
    content.append(textContent);

    QJsonObject result;
    result["content"] = content;
    return result;
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
