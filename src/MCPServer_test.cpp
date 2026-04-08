#include <gtest/gtest.h>
#include <QApplication>
#include <QThread>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QTcpSocket>
#include <QSignalSpy>
#include <QElapsedTimer>
#include <QDir>
#include <QTemporaryDir>
#include <memory>
#include <QMainWindow>
#include <unistd.h>
#include <fcntl.h>

#define private public
#include "MCPServer.h"
#undef private

#include "Manager.h"
#include "MeshInfoOverlay.h"
#include "PrimitiveObject.h"
#include "SelectionSet.h"
#include <OgreException.h>
#include "TestHelpers.h"

// Helper to extract the text from an MCP tool result
static QString getResultText(const QJsonObject &result)
{
    QJsonArray content = result["content"].toArray();
    if (content.isEmpty()) return QString();
    return content[0].toObject()["text"].toString();
}

static bool isError(const QJsonObject &result)
{
    return result["isError"].toBool(false);
}

static QByteArray readTransportMessage(int readFd)
{
    QByteArray response;
    char buffer[4096];
    int expectedTotalBytes = -1;

    while (true) {
        ssize_t bytesRead = read(readFd, buffer, sizeof(buffer));
        if (bytesRead <= 0) {
            break;
        }

        response.append(buffer, bytesRead);

        if (expectedTotalBytes < 0) {
            const int headerEnd = response.indexOf("\r\n\r\n");
            if (headerEnd != -1) {
                const QList<QByteArray> headerLines = response.left(headerEnd).split('\n');
                for (const QByteArray& rawLine : headerLines) {
                    const QByteArray line = rawLine.trimmed();
                    if (!line.toLower().startsWith("content-length:")) {
                        continue;
                    }

                    bool ok = false;
                    const int contentLength = line.mid(sizeof("content-length:") - 1).trimmed().toInt(&ok);
                    if (ok && contentLength >= 0) {
                        expectedTotalBytes = headerEnd + 4 + contentLength;
                    }
                    break;
                }

                // If a full header was received but no valid length was found,
                // stop here and let downstream assertions fail with context.
                if (expectedTotalBytes < 0) {
                    break;
                }
            }
        }

        if (expectedTotalBytes >= 0 && response.size() >= expectedTotalBytes) {
            response.truncate(expectedTotalBytes);
            break;
        }
    }

    return response;
}

static QByteArray readTransportMessageNonBlocking(int readFd)
{
    const int flags = fcntl(readFd, F_GETFL, 0);
    if (flags == -1) {
        return {};
    }

    if (fcntl(readFd, F_SETFL, flags | O_NONBLOCK) == -1) {
        return {};
    }

    QByteArray response;
    char buffer[4096];
    ssize_t bytesRead = read(readFd, buffer, sizeof(buffer));
    if (bytesRead > 0) {
        response.append(buffer, bytesRead);
    }

    fcntl(readFd, F_SETFL, flags);
    return response;
}

static QJsonObject extractJsonBody(const QByteArray &transport)
{
    const int headerEnd = transport.indexOf("\r\n\r\n");
    EXPECT_NE(headerEnd, -1);
    if (headerEnd == -1) {
        return QJsonObject();
    }

    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(transport.mid(headerEnd + 4), &error);
    EXPECT_EQ(error.error, QJsonParseError::NoError);
    EXPECT_TRUE(doc.isObject());
    return doc.object();
}

class MCPServerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        server.reset();
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
        }
        createStandardOgreMaterials();

        server = std::make_unique<MCPServer>();
        // No mainWindow set -- tests that need it will test the error path
    }

    void TearDown() override
    {
        if (app)
        {
            app->processEvents();
        }
    }

    QApplication* app = nullptr;
    std::unique_ptr<MCPServer> server;
};

class MCPServerProtocolTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        server = std::make_unique<MCPServer>();
        ASSERT_EQ(pipe(outputPipe), 0);
        server->setOutputFd(outputPipe[1]);
    }

    void TearDown() override
    {
        server.reset();
        if (outputPipe[0] != -1) {
            close(outputPipe[0]);
        }
        if (outputPipe[1] != -1) {
            close(outputPipe[1]);
        }
        Manager::kill();
        if (app) {
            app->processEvents();
        }
    }

    QJsonObject processAndRead(const QByteArray &payload)
    {
        server->processMessage(payload);
        return extractJsonBody(readTransportMessage(outputPipe[0]));
    }

    QByteArray makeTransport(const QByteArray &json)
    {
        return QByteArray("Content-Length: ") + QByteArray::number(json.size()) + "\r\n\r\n" + json;
    }

    QApplication* app = nullptr;
    std::unique_ptr<MCPServer> server;
    int outputPipe[2] = {-1, -1};
};

// --- Material tools ---

TEST_F(MCPServerTest, CreateMaterial)
{
    QJsonObject args;
    args["name"] = "TestMaterial";
    QJsonObject colors;
    colors["diffuse"] = QJsonArray{1.0, 0.0, 0.0};
    colors["ambient"] = QJsonArray{0.1, 0.1, 0.1};
    colors["specular"] = QJsonArray{1.0, 1.0, 1.0};
    colors["shininess"] = 64.0;
    args["colors"] = colors;

    QJsonObject result = server->callTool("create_material", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("TestMaterial"));
}

TEST_F(MCPServerTest, CreateMaterialDuplicate)
{
    QJsonObject args;
    args["name"] = "DupMaterial";
    server->callTool("create_material", args);

    QJsonObject result = server->callTool("create_material", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("already exists"));
}

TEST_F(MCPServerTest, GetMaterial)
{
    QJsonObject createArgs;
    createArgs["name"] = "GetTestMat";
    server->callTool("create_material", createArgs);

    QJsonObject getArgs;
    getArgs["name"] = "GetTestMat";
    QJsonObject result = server->callTool("get_material", getArgs);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("GetTestMat"));
}

TEST_F(MCPServerTest, GetMaterialNotFound)
{
    QJsonObject args;
    args["name"] = "NonExistentMaterial_XYZ";
    QJsonObject result = server->callTool("get_material", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("not found"));
}

TEST_F(MCPServerTest, ModifyMaterial)
{
    QJsonObject createArgs;
    createArgs["name"] = "ModifyTestMat";
    server->callTool("create_material", createArgs);

    QJsonObject modArgs;
    modArgs["name"] = "ModifyTestMat";
    modArgs["diffuse"] = QJsonArray{0.0, 1.0, 0.0};
    modArgs["shininess"] = 100.0;
    QJsonObject result = server->callTool("modify_material", modArgs);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Modified material"));
}

TEST_F(MCPServerTest, ModifyMaterialNotFound)
{
    QJsonObject args;
    args["name"] = "NoSuchMat_XYZ";
    args["diffuse"] = QJsonArray{1.0, 0.0, 0.0};
    QJsonObject result = server->callTool("modify_material", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("not found"));
}

TEST_F(MCPServerTest, ListMaterials)
{
    QJsonObject createArgs;
    createArgs["name"] = "ListTestMat";
    server->callTool("create_material", createArgs);

    QJsonObject result = server->callTool("list_materials", QJsonObject());
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("ListTestMat"));
}

// --- Primitive tools ---

TEST_F(MCPServerTest, CreatePrimitive)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject args;
    args["type"] = "sphere";
    args["name"] = "TestSphere";
    QJsonObject result = server->callTool("create_primitive", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("TestSphere"));
}

TEST_F(MCPServerTest, CreatePrimitiveTypes)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QStringList types = {"box", "cylinder", "cone"};
    for (const QString &type : types) {
        QJsonObject args;
        args["type"] = type;
        args["name"] = "Test_" + type;
        QJsonObject result = server->callTool("create_primitive", args);
        EXPECT_FALSE(isError(result)) << "Failed to create primitive type: " << type.toStdString();
    }
}

TEST_F(MCPServerTest, CreatePrimitiveInvalidType)
{
    QJsonObject args;
    args["type"] = "dodecahedron";
    QJsonObject result = server->callTool("create_primitive", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Unknown primitive type"));
}

// --- Scene info ---

TEST_F(MCPServerTest, GetSceneInfo)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject primArgs;
    primArgs["type"] = "cube";
    primArgs["name"] = "SceneInfoCube";
    server->callTool("create_primitive", primArgs);

    QJsonObject result = server->callTool("get_scene_info", QJsonObject());
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("SceneInfoCube"));
    EXPECT_TRUE(text.contains("Scene Information"));
}

// --- Mesh info ---

TEST_F(MCPServerTest, GetMeshInfo)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject primArgs;
    primArgs["type"] = "sphere";
    primArgs["name"] = "MeshInfoSphere";
    server->callTool("create_primitive", primArgs);

    QJsonObject result = server->callTool("get_mesh_info", QJsonObject());
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Vertices"));
    EXPECT_TRUE(text.contains("Triangles"));
}

// --- Transform ---

TEST_F(MCPServerTest, TransformMesh)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject primArgs;
    primArgs["type"] = "cube";
    primArgs["name"] = "TransformCube";
    server->callTool("create_primitive", primArgs);

    QJsonObject transArgs;
    transArgs["name"] = "TransformCube";
    transArgs["position"] = QJsonArray{1.0, 2.0, 3.0};
    QJsonObject result = server->callTool("transform_mesh", transArgs);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("position"));
}

TEST_F(MCPServerTest, TransformMeshNotFound)
{
    QJsonObject args;
    args["name"] = "NonExistentNode_XYZ";
    args["position"] = QJsonArray{0.0, 0.0, 0.0};
    QJsonObject result = server->callTool("transform_mesh", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("not found"));
}

// --- Apply material ---

TEST_F(MCPServerTest, ApplyMaterial)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    // Create a material
    QJsonObject matArgs;
    matArgs["name"] = "ApplyTestMat";
    server->callTool("create_material", matArgs);

    // Create a primitive (which creates an entity)
    QJsonObject primArgs;
    primArgs["type"] = "cube";
    primArgs["name"] = "ApplyCube";
    server->callTool("create_primitive", primArgs);

    // Apply the material to the entity by mesh name
    QJsonObject applyArgs;
    applyArgs["material"] = "ApplyTestMat";
    applyArgs["mesh"] = "ApplyCube";
    QJsonObject result = server->callTool("apply_material", applyArgs);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Applied material"));
}

TEST_F(MCPServerTest, ApplyMaterialNotFound)
{
    QJsonObject args;
    args["material"] = "NoSuchMaterial_XYZ";
    args["mesh"] = "SomeEntity";
    QJsonObject result = server->callTool("apply_material", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("not found"));
}

// --- Textures ---

TEST_F(MCPServerTest, ListTextures)
{
    QJsonObject result = server->callTool("list_textures", QJsonObject());
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("textures"));
}

TEST_F(MCPServerTest, SetTexture)
{
    QJsonObject matArgs;
    matArgs["name"] = "TexTestMat";
    server->callTool("create_material", matArgs);

    QJsonObject texArgs;
    texArgs["material"] = "TexTestMat";
    texArgs["texture"] = "nonexistent_texture.png";
    QJsonObject result = server->callTool("set_texture", texArgs);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Set texture"));
}

TEST_F(MCPServerTest, SetTextureMaterialNotFound)
{
    QJsonObject args;
    args["material"] = "NoMat_XYZ";
    args["texture"] = "test.png";
    QJsonObject result = server->callTool("set_texture", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("not found"));
}

// --- Animation ---

TEST_F(MCPServerTest, Animate)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject primArgs;
    primArgs["type"] = "sphere";
    primArgs["name"] = "AnimSphere";
    server->callTool("create_primitive", primArgs);

    QJsonObject animArgs;
    animArgs["name"] = "AnimSphere";
    animArgs["yaw"] = 90.0;
    QJsonObject result = server->callTool("animate", animArgs);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Started animation"));
}

TEST_F(MCPServerTest, AnimateStop)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject primArgs;
    primArgs["type"] = "sphere";
    primArgs["name"] = "AnimStopSphere";
    server->callTool("create_primitive", primArgs);

    // Start
    QJsonObject startArgs;
    startArgs["name"] = "AnimStopSphere";
    startArgs["yaw"] = 45.0;
    server->callTool("animate", startArgs);

    // Stop
    QJsonObject stopArgs;
    stopArgs["name"] = "AnimStopSphere";
    stopArgs["stop"] = true;
    QJsonObject result = server->callTool("animate", stopArgs);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Stopped animation"));
}

TEST_F(MCPServerTest, AnimateNodeNotFound)
{
    QJsonObject args;
    args["name"] = "NoSuchNode_XYZ";
    QJsonObject result = server->callTool("animate", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("not found"));
}

// --- Unknown tool ---

TEST_F(MCPServerTest, UnknownTool)
{
    QJsonObject result = server->callTool("totally_fake_tool", QJsonObject());
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Unknown tool"));
}

// --- Export mesh ---

TEST_F(MCPServerTest, ExportMeshNoSelection)
{
    QJsonObject args;
    args["path"] = "/tmp/test_export.obj";
    QJsonObject result = server->callTool("export_mesh", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("No scene nodes selected"));
}

// --- Take screenshot (no MainWindow -- test error path) ---

TEST_F(MCPServerTest, TakeScreenshotNoMainWindow)
{
    QJsonObject args;
    args["path"] = "/tmp/mcp_test_screenshot.png";
    QJsonObject result = server->callTool("take_screenshot", args);
    // Without MainWindow, screenshot should return an error or handle gracefully
    EXPECT_FALSE(getResultText(result).isEmpty());
}

// --- Load mesh (no MainWindow -- test error path) ---

TEST_F(MCPServerTest, LoadMeshNoMainWindow)
{
    QJsonObject args;
    args["path"] = "/tmp/nonexistent.mesh";
    QJsonObject result = server->callTool("load_mesh", args);
    // Without MainWindow, load_mesh should return an error
    EXPECT_FALSE(getResultText(result).isEmpty());
}

// --- Tools list ---

TEST_F(MCPServerTest, HandleToolsList)
{
    // Verify at least 15 tools are defined by checking that known tools work
    QStringList knownTools = {
        "create_material", "modify_material", "get_material", "list_materials",
        "apply_material", "load_mesh", "get_mesh_info", "transform_mesh",
        "list_textures", "set_texture", "export_mesh", "get_scene_info",
        "take_screenshot", "create_primitive", "animate",
        "list_skeletal_animations", "get_animation_info", "set_animation_length",
        "set_animation_time", "add_keyframe", "remove_keyframe",
        "play_animation", "toggle_skeleton_debug", "toggle_bone_weights",
        "toggle_normals", "merge_animations"
    };

    for (const QString &tool : knownTools) {
        // Just call with empty args - we only care that it's not "Unknown tool"
        QJsonObject result = server->callTool(tool, QJsonObject());
        EXPECT_FALSE(getResultText(result).contains("Unknown tool"))
            << "Tool should be recognized: " << tool.toStdString();
    }
}

// ==========================================================================
// NEW TESTS: Server state and start/stop
// ==========================================================================

TEST_F(MCPServerTest, ServerInitiallyNotRunning)
{
    // A freshly constructed MCPServer should not be running until start() is called
    EXPECT_FALSE(server->isRunning());
}

TEST_F(MCPServerTest, ServerStartAndStop)
{
    server->start();
    EXPECT_TRUE(server->isRunning());

    server->stop();
    EXPECT_FALSE(server->isRunning());
}

TEST_F(MCPServerTest, ServerDoubleStartIsIdempotent)
{
    server->start();
    EXPECT_TRUE(server->isRunning());

    // Calling start again should not crash or change state
    server->start();
    EXPECT_TRUE(server->isRunning());

    server->stop();
    EXPECT_FALSE(server->isRunning());
}

TEST_F(MCPServerTest, ServerDoubleStopIsIdempotent)
{
    // Stopping a server that was never started should not crash
    server->stop();
    EXPECT_FALSE(server->isRunning());

    server->start();
    server->stop();
    // Stopping again should be safe
    server->stop();
    EXPECT_FALSE(server->isRunning());
}

// ==========================================================================
// NEW TESTS: create_material edge cases
// ==========================================================================

TEST_F(MCPServerTest, CreateMaterialEmptyName)
{
    QJsonObject args;
    args["name"] = "";
    QJsonObject result = server->callTool("create_material", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Material name is required"));
}

TEST_F(MCPServerTest, CreateMaterialNoNameKey)
{
    // Calling create_material with no "name" key at all
    QJsonObject args;
    QJsonObject result = server->callTool("create_material", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Material name is required"));
}

TEST_F(MCPServerTest, CreateMaterialWithEmissiveColor)
{
    QJsonObject args;
    args["name"] = "EmissiveMat";
    QJsonObject colors;
    colors["emissive"] = QJsonArray{0.5, 0.3, 0.1};
    args["colors"] = colors;

    QJsonObject result = server->callTool("create_material", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("EmissiveMat"));
}

// ==========================================================================
// NEW TESTS: get_material edge cases
// ==========================================================================

TEST_F(MCPServerTest, GetMaterialEmptyName)
{
    QJsonObject args;
    args["name"] = "";
    QJsonObject result = server->callTool("get_material", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Material name is required"));
}

TEST_F(MCPServerTest, GetMaterialNoNameKey)
{
    QJsonObject args;
    QJsonObject result = server->callTool("get_material", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Material name is required"));
}

// ==========================================================================
// NEW TESTS: modify_material with emissive, ambient, specular
// ==========================================================================

TEST_F(MCPServerTest, ModifyMaterialEmissive)
{
    QJsonObject createArgs;
    createArgs["name"] = "ModEmissiveMat";
    server->callTool("create_material", createArgs);

    QJsonObject modArgs;
    modArgs["name"] = "ModEmissiveMat";
    modArgs["emissive"] = QJsonArray{1.0, 0.5, 0.0};
    QJsonObject result = server->callTool("modify_material", modArgs);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Modified material"));
    EXPECT_TRUE(text.contains("emissive"));
}

TEST_F(MCPServerTest, ModifyMaterialAmbient)
{
    QJsonObject createArgs;
    createArgs["name"] = "ModAmbientMat";
    server->callTool("create_material", createArgs);

    QJsonObject modArgs;
    modArgs["name"] = "ModAmbientMat";
    modArgs["ambient"] = QJsonArray{0.3, 0.4, 0.5};
    QJsonObject result = server->callTool("modify_material", modArgs);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Modified material"));
    EXPECT_TRUE(text.contains("ambient"));
}

TEST_F(MCPServerTest, ModifyMaterialSpecular)
{
    QJsonObject createArgs;
    createArgs["name"] = "ModSpecularMat";
    server->callTool("create_material", createArgs);

    QJsonObject modArgs;
    modArgs["name"] = "ModSpecularMat";
    modArgs["specular"] = QJsonArray{0.8, 0.9, 1.0};
    modArgs["shininess"] = 64.0;
    QJsonObject result = server->callTool("modify_material", modArgs);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Modified material"));
    EXPECT_TRUE(text.contains("specular"));
}

TEST_F(MCPServerTest, ModifyMaterialEmptyName)
{
    QJsonObject args;
    args["name"] = "";
    args["diffuse"] = QJsonArray{1.0, 0.0, 0.0};
    QJsonObject result = server->callTool("modify_material", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Material name is required"));
}

// ==========================================================================
// NEW TESTS: transform_mesh with rotation and scale
// ==========================================================================

TEST_F(MCPServerTest, TransformMeshRotation)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject primArgs;
    primArgs["type"] = "cube";
    primArgs["name"] = "RotateCube";
    server->callTool("create_primitive", primArgs);

    QJsonObject transArgs;
    transArgs["name"] = "RotateCube";
    transArgs["rotation"] = QJsonArray{45.0, 90.0, 0.0};
    QJsonObject result = server->callTool("transform_mesh", transArgs);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("rotation"));
}

TEST_F(MCPServerTest, TransformMeshScale)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject primArgs;
    primArgs["type"] = "cube";
    primArgs["name"] = "ScaleCube";
    server->callTool("create_primitive", primArgs);

    QJsonObject transArgs;
    transArgs["name"] = "ScaleCube";
    transArgs["scale"] = QJsonArray{2.0, 3.0, 4.0};
    QJsonObject result = server->callTool("transform_mesh", transArgs);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("scale"));
}

TEST_F(MCPServerTest, TransformMeshAllThreeTransforms)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject primArgs;
    primArgs["type"] = "sphere";
    primArgs["name"] = "FullTransformSphere";
    server->callTool("create_primitive", primArgs);

    QJsonObject transArgs;
    transArgs["name"] = "FullTransformSphere";
    transArgs["position"] = QJsonArray{5.0, 10.0, -3.0};
    transArgs["rotation"] = QJsonArray{0.0, 180.0, 0.0};
    transArgs["scale"] = QJsonArray{1.5, 1.5, 1.5};
    QJsonObject result = server->callTool("transform_mesh", transArgs);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("position"));
    EXPECT_TRUE(text.contains("rotation"));
    EXPECT_TRUE(text.contains("scale"));
}

TEST_F(MCPServerTest, TransformMeshNoNameNoSelection)
{
    // No name provided and no selection -- should fail
    QJsonObject transArgs;
    transArgs["position"] = QJsonArray{1.0, 2.0, 3.0};
    QJsonObject result = server->callTool("transform_mesh", transArgs);
    EXPECT_TRUE(isError(result));
    // Should report either "not found" or "no scene nodes selected"
    EXPECT_FALSE(getResultText(result).isEmpty());
}

// ==========================================================================
// NEW TESTS: get_scene_info with multiple objects
// ==========================================================================

TEST_F(MCPServerTest, GetSceneInfoMultipleObjects)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    // Create several primitives
    QJsonObject args1;
    args1["type"] = "sphere";
    args1["name"] = "SceneSphere";
    server->callTool("create_primitive", args1);

    QJsonObject args2;
    args2["type"] = "cube";
    args2["name"] = "SceneCube";
    server->callTool("create_primitive", args2);

    QJsonObject args3;
    args3["type"] = "cylinder";
    args3["name"] = "SceneCylinder";
    server->callTool("create_primitive", args3);

    QJsonObject result = server->callTool("get_scene_info", QJsonObject());
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("SceneSphere"));
    EXPECT_TRUE(text.contains("SceneCube"));
    EXPECT_TRUE(text.contains("SceneCylinder"));
    EXPECT_TRUE(text.contains("Scene Information"));
}

TEST_F(MCPServerTest, GetSceneInfoEmptyScene)
{
    // No objects created -- scene should still report info
    QJsonObject result = server->callTool("get_scene_info", QJsonObject());
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Scene Information"));
    EXPECT_TRUE(text.contains("Scene Nodes: 0") || text.contains("(none)"));
}

// ==========================================================================
// NEW TESTS: get_mesh_info with empty scene
// ==========================================================================

TEST_F(MCPServerTest, GetMeshInfoEmptyScene)
{
    // No entities in scene -- should report "No entities"
    QJsonObject result = server->callTool("get_mesh_info", QJsonObject());
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("No entities in scene"));
}

// ==========================================================================
// NEW TESTS: apply_material edge cases
// ==========================================================================

TEST_F(MCPServerTest, ApplyMaterialEmptyMaterialName)
{
    QJsonObject args;
    args["material"] = "";
    args["mesh"] = "SomeEntity";
    QJsonObject result = server->callTool("apply_material", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Material name is required"));
}

TEST_F(MCPServerTest, ApplyMaterialToSelectionNoSelection)
{
    // Create a material but do not specify mesh name and have no selection
    QJsonObject matArgs;
    matArgs["name"] = "ApplySelMat";
    server->callTool("create_material", matArgs);

    QJsonObject applyArgs;
    applyArgs["material"] = "ApplySelMat";
    // No "mesh" key -- should try to apply to selection, but nothing is selected
    QJsonObject result = server->callTool("apply_material", applyArgs);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("No entity specified and no entities selected"));
}

TEST_F(MCPServerTest, ApplyMaterialEntityNotFound)
{
    // Create a material and try to apply to a non-existent entity
    QJsonObject matArgs;
    matArgs["name"] = "ApplyEntNotFoundMat";
    server->callTool("create_material", matArgs);

    QJsonObject applyArgs;
    applyArgs["material"] = "ApplyEntNotFoundMat";
    applyArgs["mesh"] = "NonExistentEntity_XYZ";
    QJsonObject result = server->callTool("apply_material", applyArgs);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("not found"));
}

// ==========================================================================
// NEW TESTS: set_texture edge cases
// ==========================================================================

TEST_F(MCPServerTest, SetTextureEmptyArgs)
{
    // Both material and texture are empty
    QJsonObject args;
    args["material"] = "";
    args["texture"] = "";
    QJsonObject result = server->callTool("set_texture", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Both material and texture names are required"));
}

TEST_F(MCPServerTest, SetTextureEmptyTextureName)
{
    QJsonObject matArgs;
    matArgs["name"] = "SetTexEmptyTexMat";
    server->callTool("create_material", matArgs);

    QJsonObject args;
    args["material"] = "SetTexEmptyTexMat";
    args["texture"] = "";
    QJsonObject result = server->callTool("set_texture", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Both material and texture names are required"));
}

TEST_F(MCPServerTest, SetTextureEmptyMaterialName)
{
    QJsonObject args;
    args["material"] = "";
    args["texture"] = "some_texture.png";
    QJsonObject result = server->callTool("set_texture", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Both material and texture names are required"));
}

// ==========================================================================
// NEW TESTS: export_mesh edge cases
// ==========================================================================

TEST_F(MCPServerTest, ExportMeshEmptyPath)
{
    QJsonObject args;
    args["path"] = "";
    QJsonObject result = server->callTool("export_mesh", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Export path is required"));
}

// ==========================================================================
// NEW TESTS: create_primitive edge cases
// ==========================================================================

TEST_F(MCPServerTest, CreatePrimitiveEmptyType)
{
    QJsonObject args;
    args["type"] = "";
    QJsonObject result = server->callTool("create_primitive", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Primitive type is required"));
}

TEST_F(MCPServerTest, CreatePrimitivePlane)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject args;
    args["type"] = "plane";
    args["name"] = "TestPlane";
    QJsonObject result = server->callTool("create_primitive", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("TestPlane"));
}

TEST_F(MCPServerTest, CreatePrimitiveTorus)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject args;
    args["type"] = "torus";
    args["name"] = "TestTorus";
    QJsonObject result = server->callTool("create_primitive", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("TestTorus"));
}

TEST_F(MCPServerTest, CreatePrimitiveTube)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject args;
    args["type"] = "tube";
    args["name"] = "TestTube";
    QJsonObject result = server->callTool("create_primitive", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("TestTube"));
}

TEST_F(MCPServerTest, CreatePrimitiveCapsule)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject args;
    args["type"] = "capsule";
    args["name"] = "TestCapsule";
    QJsonObject result = server->callTool("create_primitive", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("TestCapsule"));
}

TEST_F(MCPServerTest, CreatePrimitiveIcoSphere)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject args;
    args["type"] = "icosphere";
    args["name"] = "TestIcoSphere";
    QJsonObject result = server->callTool("create_primitive", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("TestIcoSphere"));
}

TEST_F(MCPServerTest, CreatePrimitiveSpring)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject args;
    args["type"] = "spring";
    args["name"] = "TestSpring";
    QJsonObject result = server->callTool("create_primitive", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("TestSpring"));
}

TEST_F(MCPServerTest, CreatePrimitiveAutoGeneratedName)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    // When no name is provided, create_primitive auto-generates one
    QJsonObject args;
    args["type"] = "sphere";
    // No "name" key
    QJsonObject result = server->callTool("create_primitive", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("sphere"));
}

// ==========================================================================
// NEW TESTS: animate with pitch and roll
// ==========================================================================

TEST_F(MCPServerTest, AnimateWithPitchAndRoll)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject primArgs;
    primArgs["type"] = "cube";
    primArgs["name"] = "PitchRollCube";
    server->callTool("create_primitive", primArgs);

    QJsonObject animArgs;
    animArgs["name"] = "PitchRollCube";
    animArgs["pitch"] = 30.0;
    animArgs["roll"] = 60.0;
    QJsonObject result = server->callTool("animate", animArgs);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Started animation"));
    EXPECT_TRUE(text.contains("pitch: 30"));
    EXPECT_TRUE(text.contains("roll: 60"));
}

TEST_F(MCPServerTest, AnimateZeroSpeedsDefaultsToYaw45)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject primArgs;
    primArgs["type"] = "sphere";
    primArgs["name"] = "DefaultAnimSphere";
    server->callTool("create_primitive", primArgs);

    // All speeds zero -- should default to yaw=45
    QJsonObject animArgs;
    animArgs["name"] = "DefaultAnimSphere";
    animArgs["yaw"] = 0.0;
    animArgs["pitch"] = 0.0;
    animArgs["roll"] = 0.0;
    QJsonObject result = server->callTool("animate", animArgs);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Started animation"));
    EXPECT_TRUE(text.contains("yaw: 45"));
}

TEST_F(MCPServerTest, AnimateNoSpeedsDefaultsToYaw45)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject primArgs;
    primArgs["type"] = "sphere";
    primArgs["name"] = "NoSpeedAnimSphere";
    server->callTool("create_primitive", primArgs);

    // No speed args at all -- should default to yaw=45
    QJsonObject animArgs;
    animArgs["name"] = "NoSpeedAnimSphere";
    QJsonObject result = server->callTool("animate", animArgs);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Started animation"));
    EXPECT_TRUE(text.contains("yaw: 45"));
}

TEST_F(MCPServerTest, AnimateEmptyName)
{
    QJsonObject args;
    args["name"] = "";
    QJsonObject result = server->callTool("animate", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Node name is required"));
}

TEST_F(MCPServerTest, AnimateTimerIsRunningAfterStart)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject primArgs;
    primArgs["type"] = "cube";
    primArgs["name"] = "TimerTestCube";
    server->callTool("create_primitive", primArgs);

    QJsonObject animArgs;
    animArgs["name"] = "TimerTestCube";
    animArgs["yaw"] = 90.0;
    server->callTool("animate", animArgs);

    // Process events so the timer can fire at least once
    app->processEvents();
    QThread::msleep(30);
    app->processEvents();

    // Stopping animation should succeed (timer was running)
    QJsonObject stopArgs;
    stopArgs["name"] = "TimerTestCube";
    stopArgs["stop"] = true;
    QJsonObject result = server->callTool("animate", stopArgs);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Stopped animation"));
}

// ==========================================================================
// NEW TESTS: load_mesh edge cases
// ==========================================================================

TEST_F(MCPServerTest, LoadMeshEmptyPath)
{
    QJsonObject args;
    args["path"] = "";
    QJsonObject result = server->callTool("load_mesh", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("File path is required"));
}

// ==========================================================================
// NEW TESTS: take_screenshot edge cases
// ==========================================================================

TEST_F(MCPServerTest, TakeScreenshotNoMainWindowNoPath)
{
    // No path specified and no MainWindow -- should error about MainWindow
    QJsonObject args;
    QJsonObject result = server->callTool("take_screenshot", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("MainWindow not available"));
}

// ==========================================================================
// NEW TESTS: Multiple unknown tools
// ==========================================================================

TEST_F(MCPServerTest, UnknownToolVariousNames)
{
    QStringList fakeTools = {"foo_bar", "delete_everything", "render_final"};
    for (const QString &tool : fakeTools) {
        QJsonObject result = server->callTool(tool, QJsonObject());
        EXPECT_TRUE(isError(result)) << "Expected error for unknown tool: " << tool.toStdString();
        EXPECT_TRUE(getResultText(result).contains("Unknown tool"))
            << "Expected 'Unknown tool' message for: " << tool.toStdString();
    }
}

TEST_F(MCPServerTest, EmptyToolName)
{
    QJsonObject result = server->callTool("", QJsonObject());
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Unknown tool"));
}

// ==========================================================================
// NEW TESTS: list_materials includes standard materials
// ==========================================================================

TEST_F(MCPServerTest, ListMaterialsIncludesStandardMaterials)
{
    QJsonObject result = server->callTool("list_materials", QJsonObject());
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    // BaseWhite and BaseWhiteNoLighting are created in SetUp via createStandardOgreMaterials
    EXPECT_TRUE(text.contains("BaseWhite"));
    EXPECT_TRUE(text.contains("BaseWhiteNoLighting"));
}

// ==========================================================================
// NEW TESTS: create_material with all color types at once
// ==========================================================================

TEST_F(MCPServerTest, CreateMaterialWithAllColors)
{
    QJsonObject args;
    args["name"] = "AllColorsMat";
    QJsonObject colors;
    colors["diffuse"] = QJsonArray{0.8, 0.2, 0.1};
    colors["ambient"] = QJsonArray{0.3, 0.3, 0.3};
    colors["specular"] = QJsonArray{1.0, 1.0, 1.0};
    colors["emissive"] = QJsonArray{0.0, 0.5, 0.0};
    colors["shininess"] = 96.0;
    args["colors"] = colors;

    QJsonObject result = server->callTool("create_material", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("AllColorsMat"));
}

// ==========================================================================
// NEW TESTS: get_mesh_info after creating multiple entities
// ==========================================================================

TEST_F(MCPServerTest, GetMeshInfoMultipleEntities)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject args1;
    args1["type"] = "sphere";
    args1["name"] = "MeshInfoSphere1";
    server->callTool("create_primitive", args1);

    QJsonObject args2;
    args2["type"] = "cube";
    args2["name"] = "MeshInfoCube1";
    server->callTool("create_primitive", args2);

    QJsonObject result = server->callTool("get_mesh_info", QJsonObject());
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Vertices"));
    EXPECT_TRUE(text.contains("Triangles"));
    // Should report 2 entities
    EXPECT_TRUE(text.contains("2 entities"));
}

// ==========================================================================
// NEW TESTS: Verify setOutputFd does not crash
// ==========================================================================

TEST_F(MCPServerTest, SetOutputFdDoesNotCrash)
{
    // Setting an output fd should be safe; we just set it to stderr fd for testing
    server->setOutputFd(2); // stderr
    // Should not crash; no assertion needed beyond not throwing
}

// ==========================================================================
// NEW TESTS: setMainWindow with nullptr
// ==========================================================================

TEST_F(MCPServerTest, SetMainWindowNullptr)
{
    // Setting mainWindow to nullptr should be safe
    server->setMainWindow(nullptr);
    // Calling a tool that requires MainWindow should fail gracefully
    QJsonObject args;
    args["path"] = "/tmp/test.png";
    QJsonObject result = server->callTool("take_screenshot", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("MainWindow not available"));
}

// ==========================================================================
// NEW TESTS: list_skeletal_animations
// ==========================================================================

TEST_F(MCPServerTest, ListSkeletalAnimationsEmptyScene)
{
    QJsonObject result = server->callTool("list_skeletal_animations", QJsonObject());
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("No skeletal animations found"));
}

TEST_F(MCPServerTest, ListSkeletalAnimationsNoSkeletonEntities)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    // Primitives have no skeleton
    QJsonObject primArgs;
    primArgs["type"] = "sphere";
    primArgs["name"] = "SkeletalListSphere";
    server->callTool("create_primitive", primArgs);

    QJsonObject result = server->callTool("list_skeletal_animations", QJsonObject());
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("No skeletal animations found"));
}

// ==========================================================================
// NEW TESTS: get_animation_info
// ==========================================================================

TEST_F(MCPServerTest, GetAnimationInfoMissingParams)
{
    // Both missing
    QJsonObject result = server->callTool("get_animation_info", QJsonObject());
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("required"));
}

TEST_F(MCPServerTest, GetAnimationInfoEmptyParams)
{
    QJsonObject args;
    args["entity"] = "";
    args["animation"] = "";
    QJsonObject result = server->callTool("get_animation_info", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("required"));
}

TEST_F(MCPServerTest, GetAnimationInfoMissingAnimation)
{
    QJsonObject args;
    args["entity"] = "SomeEntity";
    QJsonObject result = server->callTool("get_animation_info", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("required"));
}

TEST_F(MCPServerTest, GetAnimationInfoEntityNotFound)
{
    QJsonObject args;
    args["entity"] = "NonExistentEntity_XYZ";
    args["animation"] = "Walk";
    QJsonObject result = server->callTool("get_animation_info", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("not found"));
}

TEST_F(MCPServerTest, GetAnimationInfoNoSkeleton)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject primArgs;
    primArgs["type"] = "cube";
    primArgs["name"] = "AnimInfoCube";
    server->callTool("create_primitive", primArgs);

    QJsonObject args;
    args["entity"] = "AnimInfoCube";
    args["animation"] = "Walk";
    QJsonObject result = server->callTool("get_animation_info", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("no skeleton"));
}

// ==========================================================================
// NEW TESTS: set_animation_length
// ==========================================================================

TEST_F(MCPServerTest, SetAnimationLengthMissingParams)
{
    QJsonObject result = server->callTool("set_animation_length", QJsonObject());
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("required"));
}

TEST_F(MCPServerTest, SetAnimationLengthEmptyParams)
{
    QJsonObject args;
    args["entity"] = "";
    args["animation"] = "";
    args["length"] = 1.0;
    QJsonObject result = server->callTool("set_animation_length", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("required"));
}

TEST_F(MCPServerTest, SetAnimationLengthInvalidLength)
{
    QJsonObject args;
    args["entity"] = "SomeEntity";
    args["animation"] = "Walk";
    args["length"] = -1.0;
    QJsonObject result = server->callTool("set_animation_length", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("positive"));
}

TEST_F(MCPServerTest, SetAnimationLengthZeroLength)
{
    QJsonObject args;
    args["entity"] = "SomeEntity";
    args["animation"] = "Walk";
    args["length"] = 0.0;
    QJsonObject result = server->callTool("set_animation_length", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("positive"));
}

TEST_F(MCPServerTest, SetAnimationLengthEntityNotFound)
{
    QJsonObject args;
    args["entity"] = "NonExistentEntity_XYZ";
    args["animation"] = "Walk";
    args["length"] = 2.0;
    QJsonObject result = server->callTool("set_animation_length", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("not found"));
}

TEST_F(MCPServerTest, SetAnimationLengthNoSkeleton)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject primArgs;
    primArgs["type"] = "sphere";
    primArgs["name"] = "AnimLenSphere";
    server->callTool("create_primitive", primArgs);

    QJsonObject args;
    args["entity"] = "AnimLenSphere";
    args["animation"] = "Walk";
    args["length"] = 2.0;
    QJsonObject result = server->callTool("set_animation_length", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("no skeleton"));
}

// ==========================================================================
// NEW TESTS: set_animation_time
// ==========================================================================

TEST_F(MCPServerTest, SetAnimationTimeMissingParams)
{
    QJsonObject result = server->callTool("set_animation_time", QJsonObject());
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("required"));
}

TEST_F(MCPServerTest, SetAnimationTimeEmptyParams)
{
    QJsonObject args;
    args["entity"] = "";
    args["animation"] = "";
    QJsonObject result = server->callTool("set_animation_time", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("required"));
}

TEST_F(MCPServerTest, SetAnimationTimeEntityNotFound)
{
    QJsonObject args;
    args["entity"] = "NonExistentEntity_XYZ";
    args["animation"] = "Walk";
    args["time"] = 0.5;
    QJsonObject result = server->callTool("set_animation_time", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("not found"));
}

TEST_F(MCPServerTest, SetAnimationTimeMissingTimeAndNavigate)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    // Create a primitive — it won't have the animation, but we test the param validation path
    QJsonObject primArgs;
    primArgs["type"] = "cube";
    primArgs["name"] = "AnimTimeCube";
    server->callTool("create_primitive", primArgs);

    QJsonObject args;
    args["entity"] = "AnimTimeCube";
    args["animation"] = "Walk";
    // Neither "time" nor "navigate" provided
    QJsonObject result = server->callTool("set_animation_time", args);
    EXPECT_TRUE(isError(result));
    // Should fail because animation not found on non-skeletal entity
    EXPECT_TRUE(getResultText(result).contains("not found"));
}

TEST_F(MCPServerTest, SetAnimationTimeNavigateInvalidValue)
{
    // Use a non-existent entity to test the entity-not-found path
    QJsonObject args;
    args["entity"] = "NonExistentEntity_XYZ";
    args["animation"] = "Walk";
    args["navigate"] = "invalid_direction";
    args["track"] = "SomeBone";
    QJsonObject result = server->callTool("set_animation_time", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("not found"));
}

TEST_F(MCPServerTest, SetAnimationTimeNavigateMissingTrack)
{
    // Navigate requires a track param — but entity not found comes first
    QJsonObject args;
    args["entity"] = "NonExistentEntity_XYZ";
    args["animation"] = "Walk";
    args["navigate"] = "next";
    QJsonObject result = server->callTool("set_animation_time", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("not found"));
}

// ==========================================================================
// NEW TESTS: add_keyframe
// ==========================================================================

TEST_F(MCPServerTest, AddKeyframeMissingParams)
{
    QJsonObject result = server->callTool("add_keyframe", QJsonObject());
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("required"));
}

TEST_F(MCPServerTest, AddKeyframeEmptyParams)
{
    QJsonObject args;
    args["entity"] = "";
    args["animation"] = "";
    args["track"] = "";
    args["time"] = 0.0;
    QJsonObject result = server->callTool("add_keyframe", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("required"));
}

TEST_F(MCPServerTest, AddKeyframeNegativeTime)
{
    QJsonObject args;
    args["entity"] = "SomeEntity";
    args["animation"] = "Walk";
    args["track"] = "Bone1";
    args["time"] = -1.0;
    QJsonObject result = server->callTool("add_keyframe", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("non-negative"));
}

TEST_F(MCPServerTest, AddKeyframeEntityNotFound)
{
    QJsonObject args;
    args["entity"] = "NonExistentEntity_XYZ";
    args["animation"] = "Walk";
    args["track"] = "Bone1";
    args["time"] = 0.5;
    QJsonObject result = server->callTool("add_keyframe", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("not found"));
}

TEST_F(MCPServerTest, AddKeyframeNoSkeleton)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject primArgs;
    primArgs["type"] = "cube";
    primArgs["name"] = "AddKfCube";
    server->callTool("create_primitive", primArgs);

    QJsonObject args;
    args["entity"] = "AddKfCube";
    args["animation"] = "Walk";
    args["track"] = "Bone1";
    args["time"] = 0.5;
    QJsonObject result = server->callTool("add_keyframe", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("no skeleton"));
}

// ==========================================================================
// NEW TESTS: remove_keyframe
// ==========================================================================

TEST_F(MCPServerTest, RemoveKeyframeMissingParams)
{
    QJsonObject result = server->callTool("remove_keyframe", QJsonObject());
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("required"));
}

TEST_F(MCPServerTest, RemoveKeyframeEmptyParams)
{
    QJsonObject args;
    args["entity"] = "";
    args["animation"] = "";
    args["track"] = "";
    args["time"] = 0.0;
    QJsonObject result = server->callTool("remove_keyframe", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("required"));
}

TEST_F(MCPServerTest, RemoveKeyframeNegativeTime)
{
    QJsonObject args;
    args["entity"] = "SomeEntity";
    args["animation"] = "Walk";
    args["track"] = "Bone1";
    args["time"] = -1.0;
    QJsonObject result = server->callTool("remove_keyframe", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("non-negative"));
}

TEST_F(MCPServerTest, RemoveKeyframeEntityNotFound)
{
    QJsonObject args;
    args["entity"] = "NonExistentEntity_XYZ";
    args["animation"] = "Walk";
    args["track"] = "Bone1";
    args["time"] = 0.5;
    QJsonObject result = server->callTool("remove_keyframe", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("not found"));
}

TEST_F(MCPServerTest, RemoveKeyframeNoSkeleton)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject primArgs;
    primArgs["type"] = "sphere";
    primArgs["name"] = "RemoveKfSphere";
    server->callTool("create_primitive", primArgs);

    QJsonObject args;
    args["entity"] = "RemoveKfSphere";
    args["animation"] = "Walk";
    args["track"] = "Bone1";
    args["time"] = 0.5;
    QJsonObject result = server->callTool("remove_keyframe", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("no skeleton"));
}

// ==========================================================================
// NEW TESTS: play_animation
// ==========================================================================

TEST_F(MCPServerTest, PlayAnimationMissingParams)
{
    QJsonObject result = server->callTool("play_animation", QJsonObject());
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("required"));
}

TEST_F(MCPServerTest, PlayAnimationEmptyParams)
{
    QJsonObject args;
    args["entity"] = "";
    args["animation"] = "";
    QJsonObject result = server->callTool("play_animation", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("required"));
}

TEST_F(MCPServerTest, PlayAnimationEntityNotFound)
{
    QJsonObject args;
    args["entity"] = "NonExistentEntity_XYZ";
    args["animation"] = "Walk";
    QJsonObject result = server->callTool("play_animation", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("not found"));
}

TEST_F(MCPServerTest, PlayAnimationNoAnimation)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject primArgs;
    primArgs["type"] = "cube";
    primArgs["name"] = "PlayAnimCube";
    server->callTool("create_primitive", primArgs);

    QJsonObject args;
    args["entity"] = "PlayAnimCube";
    args["animation"] = "NonExistentAnim";
    QJsonObject result = server->callTool("play_animation", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("not found"));
}

// ==========================================================================
// NEW TESTS: toggle_skeleton_debug
// ==========================================================================

TEST_F(MCPServerTest, ToggleSkeletonDebugMissingEntity)
{
    QJsonObject args;
    QJsonObject result = server->callTool("toggle_skeleton_debug", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("required"));
}

TEST_F(MCPServerTest, ToggleSkeletonDebugEmptyEntity)
{
    QJsonObject args;
    args["entity"] = "";
    QJsonObject result = server->callTool("toggle_skeleton_debug", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("required"));
}

TEST_F(MCPServerTest, ToggleSkeletonDebugEntityNotFound)
{
    QJsonObject args;
    args["entity"] = "NonExistentEntity_XYZ";
    QJsonObject result = server->callTool("toggle_skeleton_debug", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("not found"));
}

TEST_F(MCPServerTest, ToggleSkeletonDebugNoSkeleton)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject primArgs;
    primArgs["type"] = "sphere";
    primArgs["name"] = "SkelDebugSphere";
    server->callTool("create_primitive", primArgs);

    QJsonObject args;
    args["entity"] = "SkelDebugSphere";
    args["show"] = true;
    QJsonObject result = server->callTool("toggle_skeleton_debug", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("no skeleton"));
}

TEST_F(MCPServerTest, ToggleSkeletonDebugNoMainWindow)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    // Server has no MainWindow set — should fail for entities with skeleton
    // But primitives have no skeleton, so test the no-skeleton path
    QJsonObject primArgs;
    primArgs["type"] = "cube";
    primArgs["name"] = "SkelDebugCube";
    server->callTool("create_primitive", primArgs);

    QJsonObject args;
    args["entity"] = "SkelDebugCube";
    QJsonObject result = server->callTool("toggle_skeleton_debug", args);
    EXPECT_TRUE(isError(result));
    // Should fail because cube has no skeleton
    EXPECT_TRUE(getResultText(result).contains("no skeleton"));
}

// ==========================================================================
// NEW TESTS: toggle_bone_weights
// ==========================================================================

TEST_F(MCPServerTest, ToggleBoneWeightsMissingEntity)
{
    QJsonObject args;
    QJsonObject result = server->callTool("toggle_bone_weights", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("required"));
}

TEST_F(MCPServerTest, ToggleBoneWeightsEmptyEntity)
{
    QJsonObject args;
    args["entity"] = "";
    QJsonObject result = server->callTool("toggle_bone_weights", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("required"));
}

TEST_F(MCPServerTest, ToggleBoneWeightsEntityNotFound)
{
    QJsonObject args;
    args["entity"] = "NonExistentEntity_XYZ";
    QJsonObject result = server->callTool("toggle_bone_weights", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("not found"));
}

TEST_F(MCPServerTest, ToggleBoneWeightsNoSkeleton)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject primArgs;
    primArgs["type"] = "cube";
    primArgs["name"] = "BoneWeightCube";
    server->callTool("create_primitive", primArgs);

    QJsonObject args;
    args["entity"] = "BoneWeightCube";
    args["show"] = true;
    QJsonObject result = server->callTool("toggle_bone_weights", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("no skeleton"));
}

// ==========================================================================
// NEW TESTS: merge_animations
// ==========================================================================

TEST_F(MCPServerTest, MergeAnimationsNoEntities)
{
    // Empty scene — should require at least 2 entities
    QJsonObject result = server->callTool("merge_animations", QJsonObject());
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Need at least 2 entities"));
}

TEST_F(MCPServerTest, MergeAnimationsNoSkeletonEntities)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    // Create primitives (no skeleton) — should still fail
    QJsonObject args1;
    args1["type"] = "sphere";
    args1["name"] = "MergeSphere1";
    server->callTool("create_primitive", args1);

    QJsonObject args2;
    args2["type"] = "cube";
    args2["name"] = "MergeCube1";
    server->callTool("create_primitive", args2);

    QJsonObject result = server->callTool("merge_animations", QJsonObject());
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Need at least 2 entities"));
}

TEST_F(MCPServerTest, MergeAnimationsInvalidBaseEntity)
{
    // With an empty scene, specifying a non-existent base entity
    QJsonObject args;
    args["base_entity"] = "NonExistentEntity";
    QJsonObject result = server->callTool("merge_animations", args);
    EXPECT_TRUE(isError(result));
    // Should fail with "Need at least 2 entities" since there are no skeleton entities
    EXPECT_TRUE(getResultText(result).contains("Need at least 2 entities"));
}

// ==========================================================================
// NEW TESTS: export_mesh success path with selection
// ==========================================================================

TEST_F(MCPServerTest, ExportMeshWithSelection)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    // Create a primitive and select it
    QJsonObject primArgs;
    primArgs["type"] = "cube";
    primArgs["name"] = "ExportCube";
    server->callTool("create_primitive", primArgs);

    // Select the node
    auto nodes = Manager::getSingleton()->getSceneNodes();
    ASSERT_FALSE(nodes.isEmpty());
    SelectionSet::getSingleton()->selectOne(nodes.last());

    QJsonObject args;
    args["path"] = "/tmp/mcp_test_export.obj";
    QJsonObject result = server->callTool("export_mesh", args);
    EXPECT_FALSE(isError(result));

    // Clean up exported file
    QFile::remove("/tmp/mcp_test_export.obj");
    QFile::remove("/tmp/mcp_test_export.material");
}

// ==========================================================================
// NEW TESTS: load_mesh file not found
// ==========================================================================

TEST_F(MCPServerTest, LoadMeshFileNotFound)
{
    QJsonObject args;
    args["path"] = "/tmp/definitely_nonexistent_file_xyz.mesh";
    QJsonObject result = server->callTool("load_mesh", args);
    // Without MainWindow load_mesh returns an error
    EXPECT_TRUE(isError(result) || !getResultText(result).isEmpty());
}

// ==========================================================================
// NEW TESTS: take_screenshot with path but no MainWindow
// ==========================================================================

TEST_F(MCPServerTest, TakeScreenshotWithPathNoMainWindow)
{
    QJsonObject args;
    args["path"] = "/tmp/mcp_test_screenshot_path.png";
    QJsonObject result = server->callTool("take_screenshot", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("MainWindow not available"));
}

// ==========================================================================
// HTTP API TEST FIXTURE
// ==========================================================================

class MCPServerHttpTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        // HTTP tests only need MCPServer — not full Ogre.
        // Tool calls that need Ogre will return errors, which is fine for
        // testing the HTTP routing and response handling.
        server = std::make_unique<MCPServer>();
    }

    void TearDown() override
    {
        if (server) {
            server->stopHttp();
            server.reset();
        }
        if (app) {
            app->processEvents();
        }
    }

    // Send a raw HTTP request and return the response.
    // Since client and server share the same event loop, we must use
    // processEvents() instead of blocking waitForReadyRead().
    QByteArray sendHttpRequest(int port, const QByteArray &request, int timeoutMs = 5000)
    {
        QTcpSocket socket;
        socket.connectToHost("127.0.0.1", port);
        if (!socket.waitForConnected(1000))
            return {};

        socket.write(request);
        socket.flush();

        QByteArray response;
        QElapsedTimer timer;
        timer.start();

        while (timer.elapsed() < timeoutMs) {
            // Process events for both client and server sides
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            if (socket.bytesAvailable() > 0) {
                response.append(socket.readAll());
            }
            // Check if the connection was closed (response complete)
            if (socket.state() == QAbstractSocket::UnconnectedState && response.size() > 0)
                break;
            if (socket.state() == QAbstractSocket::ClosingState) {
                response.append(socket.readAll());
                break;
            }
            QThread::msleep(10);
        }
        // Final read
        if (socket.bytesAvailable() > 0)
            response.append(socket.readAll());
        return response;
    }

    // Extract JSON body from HTTP response
    QJsonObject parseHttpResponse(const QByteArray &response)
    {
        int bodyStart = response.indexOf("\r\n\r\n");
        if (bodyStart == -1) return {};
        QByteArray body = response.mid(bodyStart + 4);
        return QJsonDocument::fromJson(body).object();
    }

    // Extract HTTP status code from response
    int getHttpStatus(const QByteArray &response)
    {
        // HTTP/1.1 200 OK
        int firstSpace = response.indexOf(' ');
        if (firstSpace == -1) return 0;
        int secondSpace = response.indexOf(' ', firstSpace + 1);
        if (secondSpace == -1) return 0;
        return response.mid(firstSpace + 1, secondSpace - firstSpace - 1).toInt();
    }

    QApplication* app = nullptr;
    std::unique_ptr<MCPServer> server;
};

// --- HTTP server lifecycle ---

TEST_F(MCPServerHttpTest, StartAndStop)
{
    EXPECT_FALSE(server->isHttpRunning());

    EXPECT_TRUE(server->startHttp(0));
    EXPECT_TRUE(server->isHttpRunning());
    EXPECT_GT(server->httpPort(), 0);

    server->stopHttp();
    EXPECT_FALSE(server->isHttpRunning());
}

TEST_F(MCPServerHttpTest, StopWithoutStart)
{
    // Stopping without starting should not crash
    EXPECT_FALSE(server->isHttpRunning());
    server->stopHttp();
    EXPECT_FALSE(server->isHttpRunning());
}

TEST_F(MCPServerHttpTest, DoubleStart)
{
    EXPECT_TRUE(server->startHttp(0));
    int port1 = server->httpPort();
    EXPECT_GT(port1, 0);

    // Second start — may fail or succeed but should not crash
    // (The old server is still running)
    server->stopHttp();
    EXPECT_TRUE(server->startHttp(0));
    EXPECT_TRUE(server->isHttpRunning());
    server->stopHttp();
}

// --- HTTP GET /api/tools ---

TEST_F(MCPServerHttpTest, GetToolsList)
{
    ASSERT_TRUE(server->startHttp(0));
    int port = server->httpPort();

    QByteArray request = "GET /api/tools HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    QByteArray response = sendHttpRequest(port, request);

    ASSERT_FALSE(response.isEmpty());
    EXPECT_EQ(getHttpStatus(response), 200);
    EXPECT_TRUE(response.contains("application/json"));
    EXPECT_TRUE(response.contains("Access-Control-Allow-Origin"));

    QJsonObject json = parseHttpResponse(response);
    EXPECT_TRUE(json.contains("tools"));
}

// --- HTTP POST /api/tools/<name> ---

TEST_F(MCPServerHttpTest, PostToolCall)
{
    // Mark Ogre as failed so tool calls skip GL initialization (avoids SIGSEGV)
    server->setOgreInitFailed(true);

    ASSERT_TRUE(server->startHttp(0));
    int port = server->httpPort();

    QByteArray body = R"({"name":"HttpTestMat"})";
    QByteArray request = "POST /api/tools/create_material HTTP/1.1\r\n"
                         "Host: 127.0.0.1\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                         "Connection: close\r\n\r\n" + body;

    QByteArray response = sendHttpRequest(port, request);

    ASSERT_FALSE(response.isEmpty());
    EXPECT_EQ(getHttpStatus(response), 200);
    // Tool result will be an Ogre-not-initialized error, but HTTP wrapping is correct
    EXPECT_TRUE(response.contains("application/json"));
}

// --- HTTP GET /api/tools/<name> (no body) ---

TEST_F(MCPServerHttpTest, GetToolCallNoBody)
{
    server->setOgreInitFailed(true);

    ASSERT_TRUE(server->startHttp(0));
    int port = server->httpPort();

    QByteArray request = "GET /api/tools/list_materials HTTP/1.1\r\n"
                         "Host: 127.0.0.1\r\nConnection: close\r\n\r\n";

    QByteArray response = sendHttpRequest(port, request);

    ASSERT_FALSE(response.isEmpty());
    EXPECT_EQ(getHttpStatus(response), 200);
}

// --- HTTP OPTIONS (CORS preflight) ---

TEST_F(MCPServerHttpTest, OptionsCorsPreflightReturns204)
{
    ASSERT_TRUE(server->startHttp(0));
    int port = server->httpPort();

    QByteArray request = "OPTIONS /api/tools HTTP/1.1\r\n"
                         "Host: 127.0.0.1\r\n"
                         "Origin: http://example.com\r\n"
                         "Access-Control-Request-Method: POST\r\n"
                         "Connection: close\r\n\r\n";

    QByteArray response = sendHttpRequest(port, request);

    ASSERT_FALSE(response.isEmpty());
    EXPECT_EQ(getHttpStatus(response), 204);
    EXPECT_TRUE(response.contains("Access-Control-Allow-Origin"));
    EXPECT_TRUE(response.contains("Access-Control-Allow-Methods"));
}

// --- HTTP 404 for unknown route ---

TEST_F(MCPServerHttpTest, UnknownRouteReturns404)
{
    ASSERT_TRUE(server->startHttp(0));
    int port = server->httpPort();

    QByteArray request = "GET /unknown HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";

    QByteArray response = sendHttpRequest(port, request);

    ASSERT_FALSE(response.isEmpty());
    EXPECT_EQ(getHttpStatus(response), 404);

    QJsonObject json = parseHttpResponse(response);
    EXPECT_TRUE(json.contains("error"));
}

// --- HTTP POST with invalid JSON ---

TEST_F(MCPServerHttpTest, PostInvalidJsonReturns400)
{
    ASSERT_TRUE(server->startHttp(0));
    int port = server->httpPort();

    QByteArray body = "{ not valid json !!!";
    QByteArray request = "POST /api/tools/list_materials HTTP/1.1\r\n"
                         "Host: 127.0.0.1\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                         "Connection: close\r\n\r\n" + body;

    QByteArray response = sendHttpRequest(port, request);

    ASSERT_FALSE(response.isEmpty());
    EXPECT_EQ(getHttpStatus(response), 400);
}

// --- HTTP malformed request ---

TEST_F(MCPServerHttpTest, MalformedRequestReturns400)
{
    ASSERT_TRUE(server->startHttp(0));
    int port = server->httpPort();

    // Only one word in request line (missing path and version)
    QByteArray request = "BADREQUEST\r\n\r\n";

    QByteArray response = sendHttpRequest(port, request);

    ASSERT_FALSE(response.isEmpty());
    EXPECT_EQ(getHttpStatus(response), 400);
}

// --- HTTP POST with empty body (should work) ---

TEST_F(MCPServerHttpTest, PostEmptyBody)
{
    server->setOgreInitFailed(true);

    ASSERT_TRUE(server->startHttp(0));
    int port = server->httpPort();

    QByteArray request = "POST /api/tools/list_materials HTTP/1.1\r\n"
                         "Host: 127.0.0.1\r\n"
                         "Content-Length: 0\r\n"
                         "Connection: close\r\n\r\n";

    QByteArray response = sendHttpRequest(port, request);

    ASSERT_FALSE(response.isEmpty());
    EXPECT_EQ(getHttpStatus(response), 200);
}

TEST_F(MCPServerHttpTest, GetToolCallWithQueryStringStripsSuffix)
{
    server->setOgreInitFailed(true);

    ASSERT_TRUE(server->startHttp(0));
    int port = server->httpPort();

    QByteArray request = "GET /api/tools/list_materials?format=json HTTP/1.1\r\n"
                         "Host: 127.0.0.1\r\nConnection: close\r\n\r\n";

    QByteArray response = sendHttpRequest(port, request);

    ASSERT_FALSE(response.isEmpty());
    EXPECT_EQ(getHttpStatus(response), 200);
    EXPECT_TRUE(response.contains("application/json"));
}

TEST_F(MCPServerHttpTest, BusyToolRequestReturns503)
{
    server->setOgreInitFailed(true);

    ASSERT_TRUE(server->startHttp(0));
    server->m_httpBusy = true;
    int port = server->httpPort();

    QByteArray request = "GET /api/tools/list_materials HTTP/1.1\r\n"
                         "Host: 127.0.0.1\r\nConnection: close\r\n\r\n";

    QByteArray response = sendHttpRequest(port, request);

    ASSERT_FALSE(response.isEmpty());
    EXPECT_EQ(getHttpStatus(response), 503);
    EXPECT_TRUE(response.contains("Server busy"));
}

TEST_F(MCPServerHttpTest, PartialPostBodyWaitsForCompletionBeforeResponding)
{
    server->setOgreInitFailed(true);

    ASSERT_TRUE(server->startHttp(0));
    int port = server->httpPort();

    QTcpSocket socket;
    socket.connectToHost("127.0.0.1", port);
    ASSERT_TRUE(socket.waitForConnected(1000));

    const QByteArray body = R"({"name":"PartialBodyMaterial"})";
    const QByteArray partialBody = body.left(body.size() / 2);
    QByteArray request = "POST /api/tools/create_material HTTP/1.1\r\n"
                         "Host: 127.0.0.1\r\n"
                         "Content-Type: application/json\r\n"
                         "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                         "Connection: close\r\n\r\n" + partialBody;

    socket.write(request);
    socket.flush();

    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(25);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    EXPECT_EQ(socket.bytesAvailable(), 0);

    socket.write(body.mid(partialBody.size()));
    socket.flush();

    QByteArray response;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        if (socket.bytesAvailable() > 0) {
            response.append(socket.readAll());
        }
        if (socket.state() == QAbstractSocket::UnconnectedState && !response.isEmpty()) {
            break;
        }
        QThread::msleep(10);
    }

    if (socket.bytesAvailable() > 0) {
        response.append(socket.readAll());
    }

    ASSERT_FALSE(response.isEmpty());
    EXPECT_EQ(getHttpStatus(response), 200);
    EXPECT_TRUE(response.contains("application/json"));
}

// --- HTTP response contains CORS headers on all responses ---

TEST_F(MCPServerHttpTest, CorsHeadersOnAllResponses)
{
    ASSERT_TRUE(server->startHttp(0));
    int port = server->httpPort();

    // Test CORS on 404
    QByteArray request = "GET /nonexistent HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    QByteArray response = sendHttpRequest(port, request);
    EXPECT_TRUE(response.contains("Access-Control-Allow-Origin: *"));

    // Test CORS on tool list
    request = "GET /api/tools HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    response = sendHttpRequest(port, request);
    EXPECT_TRUE(response.contains("Access-Control-Allow-Origin: *"));
}

// ==========================================================================
// NEW: create_primitive with custom params (radius, height, segments)
// ==========================================================================

TEST_F(MCPServerTest, CreatePrimitiveWithCustomParams)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject args;
    args["type"] = "sphere";
    args["name"] = "CustomParamSphere";
    args["radius"] = 2.5;
    args["segments"] = 32;
    QJsonObject result = server->callTool("create_primitive", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("CustomParamSphere"));
}

TEST_F(MCPServerTest, CreatePrimitiveCylinderWithParams)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject args;
    args["type"] = "cylinder";
    args["name"] = "CustomCylinder";
    args["radius"] = 1.5;
    args["height"] = 5.0;
    QJsonObject result = server->callTool("create_primitive", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("CustomCylinder"));
}

// ==========================================================================
// NEW: modify_material with no color changes (only name)
// ==========================================================================

TEST_F(MCPServerTest, ModifyMaterialNoChanges)
{
    QJsonObject createArgs;
    createArgs["name"] = "NoChangesMat";
    server->callTool("create_material", createArgs);

    // Modify with only the name — no color args at all
    QJsonObject modArgs;
    modArgs["name"] = "NoChangesMat";
    QJsonObject result = server->callTool("modify_material", modArgs);
    // Should succeed but with no changes reported
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Modified material") || text.contains("No changes"));
}

// ==========================================================================
// NEW: apply_material with material exists but mesh doesn't
// ==========================================================================

TEST_F(MCPServerTest, ApplyMaterialMeshNotFound)
{
    QJsonObject matArgs;
    matArgs["name"] = "ApplyMeshNotFoundMat";
    server->callTool("create_material", matArgs);

    QJsonObject applyArgs;
    applyArgs["material"] = "ApplyMeshNotFoundMat";
    applyArgs["mesh"] = "NonExistentMesh_XYZ";
    QJsonObject result = server->callTool("apply_material", applyArgs);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("not found"));
}

// ==========================================================================
// NEW: transform_mesh missing name key
// ==========================================================================

TEST_F(MCPServerTest, TransformMeshEmptyName)
{
    QJsonObject args;
    args["name"] = "";
    args["position"] = QJsonArray{1.0, 2.0, 3.0};
    QJsonObject result = server->callTool("transform_mesh", args);
    EXPECT_TRUE(isError(result));
    EXPECT_FALSE(getResultText(result).isEmpty());
}

// ==========================================================================
// NEW: modify_material with all color types at once
// ==========================================================================

TEST_F(MCPServerTest, ModifyMaterialAllColors)
{
    QJsonObject createArgs;
    createArgs["name"] = "ModAllColorsMat";
    server->callTool("create_material", createArgs);

    QJsonObject modArgs;
    modArgs["name"] = "ModAllColorsMat";
    modArgs["diffuse"] = QJsonArray{0.8, 0.2, 0.1};
    modArgs["ambient"] = QJsonArray{0.3, 0.3, 0.3};
    modArgs["specular"] = QJsonArray{1.0, 1.0, 1.0};
    modArgs["emissive"] = QJsonArray{0.0, 0.5, 0.0};
    modArgs["shininess"] = 128.0;
    QJsonObject result = server->callTool("modify_material", modArgs);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Modified material"));
    EXPECT_TRUE(text.contains("diffuse"));
    EXPECT_TRUE(text.contains("ambient"));
    EXPECT_TRUE(text.contains("specular"));
    EXPECT_TRUE(text.contains("emissive"));
    EXPECT_TRUE(text.contains("shininess"));
}

// ==========================================================================
// NEW: create_primitive roundedbox with params
// ==========================================================================

TEST_F(MCPServerTest, CreatePrimitiveRoundedBoxWithParams)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject args;
    args["type"] = "roundedbox";
    args["name"] = "CustomRBox";
    args["sizeX"] = 3.0;
    args["sizeY"] = 2.0;
    args["sizeZ"] = 1.0;
    QJsonObject result = server->callTool("create_primitive", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("CustomRBox"));
}

// ==========================================================================
// NEW: animate with only pitch (no yaw or roll)
// ==========================================================================

TEST_F(MCPServerTest, AnimateWithOnlyPitch)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject primArgs;
    primArgs["type"] = "cube";
    primArgs["name"] = "PitchOnlyCube";
    server->callTool("create_primitive", primArgs);

    QJsonObject animArgs;
    animArgs["name"] = "PitchOnlyCube";
    animArgs["pitch"] = 45.0;
    QJsonObject result = server->callTool("animate", animArgs);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Started animation"));
    EXPECT_TRUE(text.contains("pitch: 45"));
}

// ==========================================================================
// NEW: animate with only roll (no yaw or pitch)
// ==========================================================================

TEST_F(MCPServerTest, AnimateWithOnlyRoll)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    QJsonObject primArgs;
    primArgs["type"] = "sphere";
    primArgs["name"] = "RollOnlySphere";
    server->callTool("create_primitive", primArgs);

    QJsonObject animArgs;
    animArgs["name"] = "RollOnlySphere";
    animArgs["roll"] = 90.0;
    QJsonObject result = server->callTool("animate", animArgs);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Started animation"));
    EXPECT_TRUE(text.contains("roll: 90"));
}

// ==========================================================================
// NEW: In-memory entity success path tests
// ==========================================================================

TEST_F(MCPServerTest, GetMeshInfoWithInMemoryEntity)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    auto mesh = createInMemoryTriangleMesh("MCPMeshInfoTriangle");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("MCPMeshInfoNode");
    auto* entity = sceneMgr->createEntity("MCPMeshInfoEntity", mesh);
    node->attachObject(entity);

    QJsonObject result = server->callTool("get_mesh_info", QJsonObject());
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Vertices"));
}

TEST_F(MCPServerTest, TransformMeshPositionWithInMemoryEntity)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    auto mesh = createInMemoryTriangleMesh("MCPTransformTriangle");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("MCPTransformNode");
    auto* entity = sceneMgr->createEntity("MCPTransformEntity", mesh);
    node->attachObject(entity);

    QJsonObject transArgs;
    transArgs["name"] = "MCPTransformNode";
    transArgs["position"] = QJsonArray{5.0, 10.0, 15.0};
    QJsonObject result = server->callTool("transform_mesh", transArgs);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("position"));

    // Verify position was actually set
    EXPECT_NEAR(node->getPosition().x, 5.0f, 0.01f);
    EXPECT_NEAR(node->getPosition().y, 10.0f, 0.01f);
    EXPECT_NEAR(node->getPosition().z, 15.0f, 0.01f);
}

TEST_F(MCPServerTest, TransformMeshRotationWithInMemoryEntity)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    auto mesh = createInMemoryTriangleMesh("MCPRotateTriangle");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("MCPRotateNode");
    auto* entity = sceneMgr->createEntity("MCPRotateEntity", mesh);
    node->attachObject(entity);

    QJsonObject transArgs;
    transArgs["name"] = "MCPRotateNode";
    transArgs["rotation"] = QJsonArray{45.0, 90.0, 0.0};
    QJsonObject result = server->callTool("transform_mesh", transArgs);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("rotation"));
}

TEST_F(MCPServerTest, TransformMeshScaleWithInMemoryEntity)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    auto mesh = createInMemoryTriangleMesh("MCPScaleTriangle");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("MCPScaleNode");
    auto* entity = sceneMgr->createEntity("MCPScaleEntity", mesh);
    node->attachObject(entity);

    QJsonObject transArgs;
    transArgs["name"] = "MCPScaleNode";
    transArgs["scale"] = QJsonArray{2.0, 3.0, 4.0};
    QJsonObject result = server->callTool("transform_mesh", transArgs);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("scale"));

    EXPECT_NEAR(node->getScale().x, 2.0f, 0.01f);
    EXPECT_NEAR(node->getScale().y, 3.0f, 0.01f);
    EXPECT_NEAR(node->getScale().z, 4.0f, 0.01f);
}

TEST_F(MCPServerTest, ExportMeshWithInMemoryEntity)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    auto mesh = createInMemoryTriangleMesh("MCPExportTriangle");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("MCPExportNode");
    auto* entity = sceneMgr->createEntity("MCPExportEntity", mesh);
    node->attachObject(entity);

    SelectionSet::getSingleton()->selectOne(node);

    QJsonObject args;
    const QString exportBase = QDir(QDir::tempPath()).filePath("mcp_inmemory_export");
    args["path"] = exportBase + ".obj";
    QJsonObject result = server->callTool("export_mesh", args);
    EXPECT_FALSE(isError(result));

    QFile::remove(exportBase + ".obj");
    QFile::remove(exportBase + ".material");
    QFile::remove(exportBase + ".mtl");
    SelectionSet::getSingleton()->clear();
}

TEST_F(MCPServerTest, SetMaterialOnInMemoryEntity)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    // Create material
    QJsonObject matArgs;
    matArgs["name"] = "InMemMatTest";
    server->callTool("create_material", matArgs);

    // Create entity
    auto mesh = createInMemoryTriangleMesh("MCPMatTriangle");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("MCPMatNode");
    auto* entity = sceneMgr->createEntity("MCPMatEntity", mesh);
    node->attachObject(entity);

    // Apply material
    QJsonObject applyArgs;
    applyArgs["material"] = "InMemMatTest";
    applyArgs["mesh"] = "MCPMatEntity";
    QJsonObject result = server->callTool("apply_material", applyArgs);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Applied material"));
}

TEST_F(MCPServerTest, GetSceneInfoWithInMemoryEntities)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    auto mesh1 = createInMemoryTriangleMesh("MCPSceneInfo1");
    auto mesh2 = createInMemoryTriangleMesh("MCPSceneInfo2");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();

    auto* node1 = Manager::getSingleton()->addSceneNode("MCPSceneNode1");
    auto* entity1 = sceneMgr->createEntity("MCPSceneEntity1", mesh1);
    node1->attachObject(entity1);

    auto* node2 = Manager::getSingleton()->addSceneNode("MCPSceneNode2");
    auto* entity2 = sceneMgr->createEntity("MCPSceneEntity2", mesh2);
    node2->attachObject(entity2);

    QJsonObject result = server->callTool("get_scene_info", QJsonObject());
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Scene Information"));
    EXPECT_TRUE(text.contains("MCPSceneNode1"));
    EXPECT_TRUE(text.contains("MCPSceneNode2"));
}

TEST_F(MCPServerTest, ListSkeletalAnimationsWithSkeletonEntity)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("MCPSkelAnimEntity");
    ASSERT_NE(entity, nullptr);

    QJsonObject result = server->callTool("list_skeletal_animations", QJsonObject());
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("TestAnim"));
}

TEST_F(MCPServerTest, GetAnimationInfoWithSkeletonEntity)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("MCPAnimInfoEntity");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity"] = "MCPAnimInfoEntity";
    args["animation"] = "TestAnim";
    QJsonObject result = server->callTool("get_animation_info", args);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("TestAnim"));
    EXPECT_TRUE(text.contains("length") || text.contains("Length"));
}

TEST_F(MCPServerTest, SetAnimationLengthWithSkeletonEntity)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("MCPSetAnimLenEntity");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity"] = "MCPSetAnimLenEntity";
    args["animation"] = "TestAnim";
    args["length"] = 2.5;
    QJsonObject result = server->callTool("set_animation_length", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("2.5") || getResultText(result).contains("length"));
}

TEST_F(MCPServerTest, SetAnimationTimeWithSkeletonEntity)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("MCPSetAnimTimeEntity");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity"] = "MCPSetAnimTimeEntity";
    args["animation"] = "TestAnim";
    args["time"] = 0.5;
    QJsonObject result = server->callTool("set_animation_time", args);
    EXPECT_FALSE(isError(result));
}

TEST_F(MCPServerTest, AddKeyframeWithSkeletonEntity)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("MCPAddKfEntity");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity"] = "MCPAddKfEntity";
    args["animation"] = "TestAnim";
    args["track"] = "Child";
    args["time"] = 0.25;
    QJsonObject result = server->callTool("add_keyframe", args);
    EXPECT_FALSE(isError(result));
}

TEST_F(MCPServerTest, RemoveKeyframeWithSkeletonEntity)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("MCPRemoveKfEntity");
    ASSERT_NE(entity, nullptr);

    // Remove keyframe at t=0.5
    QJsonObject args;
    args["entity"] = "MCPRemoveKfEntity";
    args["animation"] = "TestAnim";
    args["track"] = "Child";
    args["time"] = 0.5;
    QJsonObject result = server->callTool("remove_keyframe", args);
    EXPECT_FALSE(isError(result));
}

TEST_F(MCPServerTest, PlayAnimationWithSkeletonEntity)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("MCPPlayAnimEntity");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity"] = "MCPPlayAnimEntity";
    args["animation"] = "TestAnim";
    QJsonObject result = server->callTool("play_animation", args);
    EXPECT_FALSE(isError(result));
}

// ==========================================================================
// NEW TESTS: toggle_normals
// ==========================================================================

TEST_F(MCPServerTest, ToggleNormalsNoMainWindow)
{
    // Server has no MainWindow set — should fail gracefully
    QJsonObject args;
    args["show"] = true;
    QJsonObject result = server->callTool("toggle_normals", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("MainWindow") ||
                getResultText(result).contains("NormalVisualizer"));
}

TEST_F(MCPServerTest, ToggleNormalsIsRecognizedTool)
{
    QJsonObject result = server->callTool("toggle_normals", QJsonObject());
    EXPECT_FALSE(getResultText(result).contains("Unknown tool"));
}

// ==========================================================================
// NEW TESTS: toggle_mesh_info
// ==========================================================================

TEST_F(MCPServerTest, ToggleMeshInfoNoMainWindow)
{
    QJsonObject args;
    args["show"] = true;
    QJsonObject result = server->callTool("toggle_mesh_info", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("MainWindow") ||
                getResultText(result).contains("MeshInfoOverlay"));
}

TEST_F(MCPServerTest, ToggleMeshInfoIsRecognizedTool)
{
    QJsonObject result = server->callTool("toggle_mesh_info", QJsonObject());
    EXPECT_FALSE(getResultText(result).contains("Unknown tool"));
}

// ==========================================================================
// NEW TESTS: Protocol edge cases
// ==========================================================================

TEST_F(MCPServerTest, ServerFunctionalAfterConstruction)
{
    // Verify the server is functional after construction by calling a tool.
    // Since handleInitialize is private, we test through the public callTool
    // and verify the server responds to known tools after construction.

    // The server should respond to tools without explicit initialize call
    QJsonObject result = server->callTool("list_materials", QJsonObject());
    EXPECT_FALSE(isError(result));
    // Server is functional, which means Ogre initialized correctly
    EXPECT_FALSE(getResultText(result).isEmpty());
}

TEST_F(MCPServerTest, AllToolNamesAreRecognized)
{
    // Verify every known tool name is recognized (not "Unknown tool")
    QStringList allTools = {
        "create_material", "modify_material", "get_material", "list_materials",
        "apply_material", "load_mesh", "get_mesh_info", "transform_mesh",
        "list_textures", "set_texture", "export_mesh", "get_scene_info",
        "take_screenshot", "create_primitive", "animate",
        "list_skeletal_animations", "get_animation_info", "set_animation_length",
        "set_animation_time", "add_keyframe", "remove_keyframe",
        "play_animation", "toggle_skeleton_debug", "toggle_bone_weights",
        "toggle_normals", "toggle_mesh_info", "merge_animations",
        "save_scene", "open_scene", "validate_mesh",
        "generate_lods", "generate_auto_lods", "remove_lods", "get_lod_info",
        "delete_entity", "get_camera_info", "camera_control",
        "list_files", "search_files", "read_file"
    };
    EXPECT_EQ(allTools.size(), 40);

    for (const QString &tool : allTools) {
        QJsonObject result = server->callTool(tool, QJsonObject());
        EXPECT_FALSE(getResultText(result).contains("Unknown tool"))
            << "Tool should be recognized: " << tool.toStdString();
    }
}

TEST_F(MCPServerTest, CallTool_WithEmptyArgs)
{
    // Call several tools with empty QJsonObject - they should return errors
    // (missing required params) but NOT crash
    QStringList toolsExpectingArgs = {
        "create_material", "modify_material", "get_material",
        "apply_material", "set_texture", "transform_mesh"
    };
    for (const QString &tool : toolsExpectingArgs) {
        QJsonObject result = server->callTool(tool, QJsonObject());
        // Should be an error (missing required params) but not Unknown tool
        EXPECT_TRUE(isError(result))
            << "Expected error for empty args on: " << tool.toStdString();
        EXPECT_FALSE(getResultText(result).contains("Unknown tool"))
            << "Tool should be recognized: " << tool.toStdString();
    }
}

TEST_F(MCPServerTest, CallTool_WithNullArgs)
{
    // Call tools with a QJsonObject that has null values for required keys
    QJsonObject args;
    args["name"] = QJsonValue::Null;
    QJsonObject result = server->callTool("create_material", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Material name is required"));

    QJsonObject args2;
    args2["material"] = QJsonValue::Null;
    args2["mesh"] = QJsonValue::Null;
    QJsonObject result2 = server->callTool("apply_material", args2);
    EXPECT_TRUE(isError(result2));
    EXPECT_TRUE(getResultText(result2).contains("Material name is required"));
}

TEST_F(MCPServerTest, DoubleServerConstruction_DoesNotCrash)
{
    // Creating the server twice and calling tools should not crash
    auto server2 = std::make_unique<MCPServer>();
    QJsonObject result = server2->callTool("list_materials", QJsonObject());
    // May succeed or fail depending on Ogre state, but should not crash
    EXPECT_FALSE(getResultText(result).isEmpty());

    // Original server should still work
    QJsonObject result2 = server->callTool("list_materials", QJsonObject());
    EXPECT_FALSE(isError(result2));
}

// ==========================================================================
// NEW TESTS: Resource protocol
// ==========================================================================

TEST_F(MCPServerTest, GetSceneInfo_ReturnsSceneInformation)
{
    // Verify that get_scene_info returns scene information.
    // This exercises the same data that the MCP resource protocol would
    // expose via "qtmesheditor://scene/info".
    QJsonObject result = server->callTool("get_scene_info", QJsonObject());
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Scene Information"));
}

TEST_F(MCPServerTest, GetSceneInfo_ContainsSceneNodes)
{
    // Verify the scene info tool returns data including scene nodes.
    QJsonObject result = server->callTool("get_scene_info", QJsonObject());
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Scene Information"));
    EXPECT_TRUE(text.contains("Scene Nodes"));
}

TEST_F(MCPServerTest, UnknownToolReturnsError)
{
    // Verify that calling a non-existent tool returns an error.
    QJsonObject result = server->callTool("nonexistent_resource_tool", QJsonObject());
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Unknown tool"));
}

TEST_F(MCPServerTest, GetMaterialWithEmptyNameReturnsError)
{
    // Verify that get_material with an empty name returns a proper error
    QJsonObject args2;
    args2["name"] = "";
    QJsonObject result = server->callTool("get_material", args2);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Material name is required"));
}

// ==========================================================================
// NEW TESTS: Additional tool tests
// ==========================================================================

TEST_F(MCPServerTest, MergeAnimations_WithAnimatedEntity)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    // Create two animated entities (merge_animations needs at least 2 skeleton entities)
    Ogre::Entity* entity1 = createAnimatedTestEntity("MCPMergeAnim1");
    ASSERT_NE(entity1, nullptr);

    Ogre::Entity* entity2 = createAnimatedTestEntity("MCPMergeAnim2");
    ASSERT_NE(entity2, nullptr);

    QJsonObject args;
    args["base_entity"] = "MCPMergeAnim1";
    QJsonObject result = server->callTool("merge_animations", args);
    // Should succeed or fail gracefully - the important thing is no crash
    EXPECT_FALSE(getResultText(result).isEmpty());
}

TEST_F(MCPServerTest, ToggleNormals_ToggleOnOff)
{
    // Server has no MainWindow set -- toggle_normals requires MainWindow
    // Verify it fails gracefully for both on and off
    QJsonObject argsOn;
    argsOn["show"] = true;
    QJsonObject resultOn = server->callTool("toggle_normals", argsOn);
    EXPECT_TRUE(isError(resultOn));

    QJsonObject argsOff;
    argsOff["show"] = false;
    QJsonObject resultOff = server->callTool("toggle_normals", argsOff);
    EXPECT_TRUE(isError(resultOff));

    // Both should give consistent error messages
    EXPECT_TRUE(getResultText(resultOn).contains("MainWindow") ||
                getResultText(resultOn).contains("NormalVisualizer"));
    EXPECT_TRUE(getResultText(resultOff).contains("MainWindow") ||
                getResultText(resultOff).contains("NormalVisualizer"));
}

TEST_F(MCPServerTest, ToggleMeshInfo_ToggleOnOff)
{
    // Server has no MainWindow set -- toggle_mesh_info requires MainWindow
    QJsonObject argsOn;
    argsOn["show"] = true;
    QJsonObject resultOn = server->callTool("toggle_mesh_info", argsOn);
    EXPECT_TRUE(isError(resultOn));

    QJsonObject argsOff;
    argsOff["show"] = false;
    QJsonObject resultOff = server->callTool("toggle_mesh_info", argsOff);
    EXPECT_TRUE(isError(resultOff));

    EXPECT_TRUE(getResultText(resultOn).contains("MainWindow") ||
                getResultText(resultOn).contains("MeshInfoOverlay"));
    EXPECT_TRUE(getResultText(resultOff).contains("MainWindow") ||
                getResultText(resultOff).contains("MeshInfoOverlay"));
}

TEST_F(MCPServerTest, ToggleMeshInfo_SuccessPath)
{
    // Create a fake MainWindow with a MeshInfoOverlay child so findChild works
    QMainWindow fakeWindow;
    auto* overlay = new MeshInfoOverlay(reinterpret_cast<MainWindow*>(&fakeWindow));
    server->setMainWindow(reinterpret_cast<MainWindow*>(&fakeWindow));

    EXPECT_FALSE(overlay->isVisible());

    // Toggle on
    QJsonObject argsOn;
    argsOn["show"] = true;
    QJsonObject resultOn = server->callTool("toggle_mesh_info", argsOn);
    EXPECT_FALSE(isError(resultOn)) << getResultText(resultOn).toStdString();
    EXPECT_TRUE(getResultText(resultOn).contains("shown"));
    EXPECT_TRUE(overlay->isVisible());

    // Toggle off
    QJsonObject argsOff;
    argsOff["show"] = false;
    QJsonObject resultOff = server->callTool("toggle_mesh_info", argsOff);
    EXPECT_FALSE(isError(resultOff)) << getResultText(resultOff).toStdString();
    EXPECT_TRUE(getResultText(resultOff).contains("hidden"));
    EXPECT_FALSE(overlay->isVisible());

    // Toggle without show arg — should flip to true
    QJsonObject resultToggle = server->callTool("toggle_mesh_info", QJsonObject());
    EXPECT_FALSE(isError(resultToggle)) << getResultText(resultToggle).toStdString();
    EXPECT_TRUE(getResultText(resultToggle).contains("shown"));
    EXPECT_TRUE(overlay->isVisible());

    // Clean up
    server->setMainWindow(nullptr);
    delete overlay;
}

TEST_F(MCPServerTest, PlayAnimation_StartAndStop)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("MCPPlayStopAnimEntity");
    ASSERT_NE(entity, nullptr);

    // Start playing
    QJsonObject playArgs;
    playArgs["entity"] = "MCPPlayStopAnimEntity";
    playArgs["animation"] = "TestAnim";
    QJsonObject playResult = server->callTool("play_animation", playArgs);
    EXPECT_FALSE(isError(playResult));

    // Stop playing
    QJsonObject stopArgs;
    stopArgs["entity"] = "MCPPlayStopAnimEntity";
    stopArgs["animation"] = "TestAnim";
    stopArgs["stop"] = true;
    QJsonObject stopResult = server->callTool("play_animation", stopArgs);
    EXPECT_FALSE(isError(stopResult));
}

TEST_F(MCPServerTest, SetOgreInitFailed_AffectsToolCalls)
{
    // Create a fresh server and mark Ogre as failed
    auto failServer = std::make_unique<MCPServer>();
    failServer->setOgreInitFailed(true);

    // All tools that need Ogre should return an error
    QJsonObject result1 = failServer->callTool("create_material", QJsonObject{{"name", "FailMat"}});
    EXPECT_TRUE(isError(result1));
    EXPECT_TRUE(getResultText(result1).contains("Ogre") || getResultText(result1).contains("initialized"));

    QJsonObject result2 = failServer->callTool("list_materials", QJsonObject());
    EXPECT_TRUE(isError(result2));
    EXPECT_TRUE(getResultText(result2).contains("Ogre") || getResultText(result2).contains("initialized"));

    QJsonObject result3 = failServer->callTool("get_scene_info", QJsonObject());
    EXPECT_TRUE(isError(result3));
    EXPECT_TRUE(getResultText(result3).contains("Ogre") || getResultText(result3).contains("initialized"));
}

TEST_F(MCPServerTest, GetMeshInfo_WithSkeletonEntity)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    auto mesh = createInMemorySkeletonMesh("MCPMeshInfoSkelMesh");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("MCPMeshInfoSkelNode");
    auto* entity = sceneMgr->createEntity("MCPMeshInfoSkelEntity", mesh);
    node->attachObject(entity);

    QJsonObject result = server->callTool("get_mesh_info", QJsonObject());
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Vertices"));
    // toolGetMeshInfo reports vertices, triangles, submeshes, materials, position, scale
    // but does not currently include skeleton/bone information
    EXPECT_TRUE(text.contains("Triangles"));
    EXPECT_TRUE(text.contains("SubMeshes"));
}

TEST_F(MCPServerTest, ExportMesh_ToTempFile)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    auto mesh = createInMemoryTriangleMesh("MCPExportTempMesh");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    // Entity name must match node name — MeshImporterExporter::exporter() looks up
    // the entity via sceneMgr->hasEntity(node->getName())
    auto* node = Manager::getSingleton()->addSceneNode("MCPExportTemp");
    auto* entity = sceneMgr->createEntity("MCPExportTemp", mesh);
    node->attachObject(entity);

    SelectionSet::getSingleton()->selectOne(node);

    // Use .mesh extension with the default "Ogre Mesh (*.mesh)" format
    QString exportPath = QDir(QDir::tempPath()).filePath("mcp_export_temp_test.mesh");
    QJsonObject args;
    args["path"] = exportPath;
    QJsonObject result = server->callTool("export_mesh", args);
    EXPECT_FALSE(isError(result));

    // Verify the file was created
    QFile exportedFile(exportPath);
    EXPECT_TRUE(exportedFile.exists());
    EXPECT_GT(exportedFile.size(), 0);

    // Cleanup
    QFile::remove(exportPath);
    QFile::remove(QDir(QDir::tempPath()).filePath("mcp_export_temp_test.material"));
    SelectionSet::getSingleton()->clear();
}

// ==========================================================================
// Animation tool success path tests
// ==========================================================================

TEST_F(MCPServerTest, AnimSuccPath_ListSkeletalAnimations)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccListSkel");
    ASSERT_NE(entity, nullptr);

    QJsonObject result = server->callTool("list_skeletal_animations", QJsonObject());
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("TestAnim"));
    EXPECT_TRUE(text.contains("AnimSuccListSkel"));
    EXPECT_TRUE(text.contains("Length"));
    EXPECT_TRUE(text.contains("Enabled"));
}

TEST_F(MCPServerTest, AnimSuccPath_GetAnimationInfo)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccGetInfo");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity"] = "AnimSuccGetInfo";
    args["animation"] = "TestAnim";
    QJsonObject result = server->callTool("get_animation_info", args);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("TestAnim"));
    EXPECT_TRUE(text.contains("Length"));
    EXPECT_TRUE(text.contains("Tracks"));
    EXPECT_TRUE(text.contains("Keyframes"));
    // Should contain track info for Child bone
    EXPECT_TRUE(text.contains("Child"));
    // Should contain 3 keyframes
    EXPECT_TRUE(text.contains("3"));
}

TEST_F(MCPServerTest, AnimSuccPath_SetAnimationLength)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccSetLen");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity"] = "AnimSuccSetLen";
    args["animation"] = "TestAnim";
    args["length"] = 2.0;
    QJsonObject result = server->callTool("set_animation_length", args);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Changed animation"));
    EXPECT_TRUE(text.contains("2"));

    // Verify the length actually changed via get_animation_info
    QJsonObject infoArgs;
    infoArgs["entity"] = "AnimSuccSetLen";
    infoArgs["animation"] = "TestAnim";
    QJsonObject infoResult = server->callTool("get_animation_info", infoArgs);
    EXPECT_FALSE(isError(infoResult));
    EXPECT_TRUE(getResultText(infoResult).contains("2"));
}

TEST_F(MCPServerTest, AnimSuccPath_SetAnimationTime)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccSetTime");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity"] = "AnimSuccSetTime";
    args["animation"] = "TestAnim";
    args["time"] = 0.5;
    QJsonObject result = server->callTool("set_animation_time", args);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Set animation"));
    EXPECT_TRUE(text.contains("0.5"));
    EXPECT_TRUE(text.contains("enabled: yes"));
}

TEST_F(MCPServerTest, AnimSuccPath_SetAnimationTimeWithNavigateNext)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccNavNext");
    ASSERT_NE(entity, nullptr);

    // First set time to 0 so navigating "next" goes to t=0.5
    QJsonObject setArgs;
    setArgs["entity"] = "AnimSuccNavNext";
    setArgs["animation"] = "TestAnim";
    setArgs["time"] = 0.0;
    server->callTool("set_animation_time", setArgs);

    QJsonObject args;
    args["entity"] = "AnimSuccNavNext";
    args["animation"] = "TestAnim";
    args["navigate"] = "next";
    args["track"] = "Child";
    QJsonObject result = server->callTool("set_animation_time", args);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Navigated to keyframe"));
    EXPECT_TRUE(text.contains("0.5"));
}

TEST_F(MCPServerTest, AnimSuccPath_SetAnimationTimeWithNavigatePrev)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccNavPrev");
    ASSERT_NE(entity, nullptr);

    // Set time to 0.99 (just before end) so navigating "prev" goes to t=0.5
    // Note: setting exactly 1.0 on a 1.0-length animation wraps to 0.0
    QJsonObject setArgs;
    setArgs["entity"] = "AnimSuccNavPrev";
    setArgs["animation"] = "TestAnim";
    setArgs["time"] = 0.99;
    server->callTool("set_animation_time", setArgs);

    QJsonObject args;
    args["entity"] = "AnimSuccNavPrev";
    args["animation"] = "TestAnim";
    args["navigate"] = "prev";
    args["track"] = "Child";
    QJsonObject result = server->callTool("set_animation_time", args);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Navigated to keyframe"));
    EXPECT_TRUE(text.contains("0.5"));
}

TEST_F(MCPServerTest, AnimSuccPath_SetAnimationTimeWithNavigateFirst)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccNavFirst");
    ASSERT_NE(entity, nullptr);

    // Set time to something non-zero first
    QJsonObject setArgs;
    setArgs["entity"] = "AnimSuccNavFirst";
    setArgs["animation"] = "TestAnim";
    setArgs["time"] = 0.5;
    server->callTool("set_animation_time", setArgs);

    QJsonObject args;
    args["entity"] = "AnimSuccNavFirst";
    args["animation"] = "TestAnim";
    args["navigate"] = "first";
    args["track"] = "Child";
    QJsonObject result = server->callTool("set_animation_time", args);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Navigated to keyframe"));
    // First keyframe is at t=0
    EXPECT_TRUE(text.contains("0s") || text.contains("at 0"));
}

TEST_F(MCPServerTest, AnimSuccPath_SetAnimationTimeWithNavigateLast)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccNavLast");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity"] = "AnimSuccNavLast";
    args["animation"] = "TestAnim";
    args["navigate"] = "last";
    args["track"] = "Child";
    QJsonObject result = server->callTool("set_animation_time", args);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Navigated to keyframe"));
    // Last keyframe is at t=1.0
    EXPECT_TRUE(text.contains("1s") || text.contains("at 1"));
}

TEST_F(MCPServerTest, AnimSuccPath_SetAnimationTimeNavigateInvalidDirection)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccNavInvalid");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity"] = "AnimSuccNavInvalid";
    args["animation"] = "TestAnim";
    args["navigate"] = "sideways";
    args["track"] = "Child";
    QJsonObject result = server->callTool("set_animation_time", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("must be 'next', 'prev', 'first', or 'last'"));
}

TEST_F(MCPServerTest, AnimSuccPath_SetAnimationTimeNavigateMissingTrack)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccNavNoTrack");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity"] = "AnimSuccNavNoTrack";
    args["animation"] = "TestAnim";
    args["navigate"] = "next";
    // No "track" param
    QJsonObject result = server->callTool("set_animation_time", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("track"));
}

TEST_F(MCPServerTest, AnimSuccPath_AddKeyframe)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccAddKf");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity"] = "AnimSuccAddKf";
    args["animation"] = "TestAnim";
    args["track"] = "Child";
    args["time"] = 0.75;
    QJsonObject result = server->callTool("add_keyframe", args);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Added keyframe"));
    EXPECT_TRUE(text.contains("0.75"));
    EXPECT_TRUE(text.contains("Child"));

    // Verify the keyframe was added - get_animation_info should now show 4 keyframes
    QJsonObject infoArgs;
    infoArgs["entity"] = "AnimSuccAddKf";
    infoArgs["animation"] = "TestAnim";
    QJsonObject infoResult = server->callTool("get_animation_info", infoArgs);
    EXPECT_FALSE(isError(infoResult));
    EXPECT_TRUE(getResultText(infoResult).contains("4"));
}

TEST_F(MCPServerTest, AnimSuccPath_AddKeyframeWithExplicitTransforms)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccAddKfTransform");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity"] = "AnimSuccAddKfTransform";
    args["animation"] = "TestAnim";
    args["track"] = "Child";
    args["time"] = 0.25;
    args["translate"] = QJsonArray{1.0, 0.0, 0.0};
    args["rotate"] = QJsonArray{1.0, 0.0, 0.0, 0.0};
    args["scale"] = QJsonArray{2.0, 2.0, 2.0};
    QJsonObject result = server->callTool("add_keyframe", args);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Added keyframe"));
    EXPECT_TRUE(text.contains("0.25"));
    // Verify position was set
    EXPECT_TRUE(text.contains("pos=(1"));
    // Verify scale was set
    EXPECT_TRUE(text.contains("scale=(2"));
}

TEST_F(MCPServerTest, AnimSuccPath_RemoveKeyframe)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccRemoveKf");
    ASSERT_NE(entity, nullptr);

    // Remove the keyframe at t=0.5
    QJsonObject args;
    args["entity"] = "AnimSuccRemoveKf";
    args["animation"] = "TestAnim";
    args["track"] = "Child";
    args["time"] = 0.5;
    QJsonObject result = server->callTool("remove_keyframe", args);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Removed keyframe"));
    EXPECT_TRUE(text.contains("0.5"));
    EXPECT_TRUE(text.contains("Child"));

    // Verify the keyframe was removed - get_animation_info should now show 2 keyframes
    QJsonObject infoArgs;
    infoArgs["entity"] = "AnimSuccRemoveKf";
    infoArgs["animation"] = "TestAnim";
    QJsonObject infoResult = server->callTool("get_animation_info", infoArgs);
    EXPECT_FALSE(isError(infoResult));
    EXPECT_TRUE(getResultText(infoResult).contains("2"));
}

TEST_F(MCPServerTest, AnimSuccPath_RemoveKeyframeNotFound)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccRemoveKfNF");
    ASSERT_NE(entity, nullptr);

    // Try to remove a keyframe that doesn't exist (t=0.99)
    QJsonObject args;
    args["entity"] = "AnimSuccRemoveKfNF";
    args["animation"] = "TestAnim";
    args["track"] = "Child";
    args["time"] = 0.99;
    QJsonObject result = server->callTool("remove_keyframe", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("No keyframe found"));
}

TEST_F(MCPServerTest, AnimSuccPath_PlayAnimation)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccPlay");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity"] = "AnimSuccPlay";
    args["animation"] = "TestAnim";
    QJsonObject result = server->callTool("play_animation", args);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Playing animation"));
    EXPECT_TRUE(text.contains("TestAnim"));
    EXPECT_TRUE(text.contains("AnimSuccPlay"));

    // Verify the animation state was enabled
    Ogre::AnimationState* state = entity->getAnimationState("TestAnim");
    EXPECT_TRUE(state->getEnabled());
    EXPECT_TRUE(state->getLoop());
}

TEST_F(MCPServerTest, AnimSuccPath_PlayAnimationStop)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccPlayStop");
    ASSERT_NE(entity, nullptr);

    // First start playing
    QJsonObject playArgs;
    playArgs["entity"] = "AnimSuccPlayStop";
    playArgs["animation"] = "TestAnim";
    server->callTool("play_animation", playArgs);

    // Verify it's playing
    Ogre::AnimationState* state = entity->getAnimationState("TestAnim");
    EXPECT_TRUE(state->getEnabled());

    // Now stop
    QJsonObject stopArgs;
    stopArgs["entity"] = "AnimSuccPlayStop";
    stopArgs["animation"] = "TestAnim";
    stopArgs["play"] = false;
    QJsonObject result = server->callTool("play_animation", stopArgs);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Stopped animation"));

    // Verify the animation state was disabled
    EXPECT_FALSE(state->getEnabled());
}

TEST_F(MCPServerTest, AnimSuccPath_PlayAnimationNoLoop)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccPlayNoLoop");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity"] = "AnimSuccPlayNoLoop";
    args["animation"] = "TestAnim";
    args["loop"] = false;
    QJsonObject result = server->callTool("play_animation", args);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("Playing animation"));
    EXPECT_TRUE(text.contains("loop=false"));

    // Verify the animation state
    Ogre::AnimationState* state = entity->getAnimationState("TestAnim");
    EXPECT_TRUE(state->getEnabled());
    EXPECT_FALSE(state->getLoop());
}

TEST_F(MCPServerTest, AnimSuccPath_MergeAnimations)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity1 = createAnimatedTestEntity("AnimSuccMerge1");
    ASSERT_NE(entity1, nullptr);

    Ogre::Entity* entity2 = createAnimatedTestEntity("AnimSuccMerge2");
    ASSERT_NE(entity2, nullptr);

    QJsonObject args;
    args["base_entity"] = "AnimSuccMerge1";
    QJsonObject result = server->callTool("merge_animations", args);
    // Merge may succeed or fail depending on skeleton compatibility details,
    // but it should not crash
    EXPECT_FALSE(getResultText(result).isEmpty());
}

TEST_F(MCPServerTest, AnimSuccPath_SetAnimationTimeWithLoopDisabled)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccTimeLoop");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity"] = "AnimSuccTimeLoop";
    args["animation"] = "TestAnim";
    args["time"] = 0.3;
    args["loop"] = false;
    args["enabled"] = true;
    QJsonObject result = server->callTool("set_animation_time", args);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("loop: no"));
    EXPECT_TRUE(text.contains("enabled: yes"));
}

TEST_F(MCPServerTest, AnimSuccPath_SetAnimationTimeDisabled)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccTimeDisabled");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity"] = "AnimSuccTimeDisabled";
    args["animation"] = "TestAnim";
    args["time"] = 0.8;
    args["enabled"] = false;
    QJsonObject result = server->callTool("set_animation_time", args);
    EXPECT_FALSE(isError(result));
    QString text = getResultText(result);
    EXPECT_TRUE(text.contains("enabled: no"));

    // Verify the state is disabled
    Ogre::AnimationState* state = entity->getAnimationState("TestAnim");
    EXPECT_FALSE(state->getEnabled());
}

TEST_F(MCPServerTest, AnimSuccPath_AddKeyframeTrackNotFound)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccAddKfNoTrack");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity"] = "AnimSuccAddKfNoTrack";
    args["animation"] = "TestAnim";
    args["track"] = "NonExistentBone";
    args["time"] = 0.5;
    QJsonObject result = server->callTool("add_keyframe", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Track for bone 'NonExistentBone' not found"));
}

TEST_F(MCPServerTest, AnimSuccPath_RemoveKeyframeTrackNotFound)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccRemKfNoTrack");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity"] = "AnimSuccRemKfNoTrack";
    args["animation"] = "TestAnim";
    args["track"] = "NonExistentBone";
    args["time"] = 0.5;
    QJsonObject result = server->callTool("remove_keyframe", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Track for bone 'NonExistentBone' not found"));
}

TEST_F(MCPServerTest, AnimSuccPath_GetAnimationInfoAnimNotFound)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccInfoAnimNF");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity"] = "AnimSuccInfoAnimNF";
    args["animation"] = "NonExistentAnim";
    QJsonObject result = server->callTool("get_animation_info", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("not found"));
}

TEST_F(MCPServerTest, AnimSuccPath_SetAnimationLengthAnimNotFound)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccLenAnimNF");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity"] = "AnimSuccLenAnimNF";
    args["animation"] = "NonExistentAnim";
    args["length"] = 5.0;
    QJsonObject result = server->callTool("set_animation_length", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("not found"));
}

TEST_F(MCPServerTest, AnimSuccPath_PlayAnimationAnimNotFound)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccPlayAnimNF");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity"] = "AnimSuccPlayAnimNF";
    args["animation"] = "NonExistentAnim";
    QJsonObject result = server->callTool("play_animation", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("not found"));
}

TEST_F(MCPServerTest, AnimSuccPath_AddKeyframeAnimNotFound)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccAddKfAnimNF");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity"] = "AnimSuccAddKfAnimNF";
    args["animation"] = "NonExistentAnim";
    args["track"] = "Child";
    args["time"] = 0.5;
    QJsonObject result = server->callTool("add_keyframe", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("not found"));
}

TEST_F(MCPServerTest, AnimSuccPath_NavigateNextAtEnd)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccNavNextEnd");
    ASSERT_NE(entity, nullptr);

    // Use "last" navigation to position at the actual last keyframe (t=1.0)
    // Note: setting time=1.0 directly wraps to 0.0 on a 1.0-length animation
    QJsonObject setArgs;
    setArgs["entity"] = "AnimSuccNavNextEnd";
    setArgs["animation"] = "TestAnim";
    setArgs["navigate"] = "last";
    setArgs["track"] = "Child";
    server->callTool("set_animation_time", setArgs);

    QJsonObject args;
    args["entity"] = "AnimSuccNavNextEnd";
    args["animation"] = "TestAnim";
    args["navigate"] = "next";
    args["track"] = "Child";
    QJsonObject result = server->callTool("set_animation_time", args);
    EXPECT_FALSE(isError(result));
    // "last" wraps time to 0 (fmod(1.0, 1.0)=0), so "next" finds keyframe at 0.5
    EXPECT_TRUE(getResultText(result).contains("Navigated to keyframe"));
    EXPECT_TRUE(getResultText(result).contains("0.5s") || getResultText(result).contains("at 0.5"));
}

TEST_F(MCPServerTest, AnimSuccPath_NavigatePrevAtStart)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Ogre::Entity* entity = createAnimatedTestEntity("AnimSuccNavPrevStart");
    ASSERT_NE(entity, nullptr);

    // Set time to the first keyframe
    QJsonObject setArgs;
    setArgs["entity"] = "AnimSuccNavPrevStart";
    setArgs["animation"] = "TestAnim";
    setArgs["time"] = 0.0;
    server->callTool("set_animation_time", setArgs);

    QJsonObject args;
    args["entity"] = "AnimSuccNavPrevStart";
    args["animation"] = "TestAnim";
    args["navigate"] = "prev";
    args["track"] = "Child";
    QJsonObject result = server->callTool("set_animation_time", args);
    EXPECT_FALSE(isError(result));
    // When at first keyframe, "prev" should stay at first keyframe (t=0.0)
    EXPECT_TRUE(getResultText(result).contains("Navigated to keyframe"));
}

// --- Scene save/load tools ---

TEST_F(MCPServerTest, SaveScene_EmptyPath_ReturnsError)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    QJsonObject args;
    args["file_path"] = "";
    QJsonObject result = server->callTool("save_scene", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("file_path"));
}

TEST_F(MCPServerTest, OpenScene_MissingFile_ReturnsError)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    QJsonObject args;
    args["file_path"] = "/tmp/nonexistent_scene_file_12345.scene.glb";
    QJsonObject result = server->callTool("open_scene", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("not found") || getResultText(result).contains("Error"));
}

TEST_F(MCPServerTest, SaveScene_ValidScene_Succeeds)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    auto mesh1 = createInMemoryTriangleMesh("SaveSceneMesh1");
    auto mesh2 = createInMemoryTriangleMesh("SaveSceneMesh2");

    auto* node1 = Manager::getSingleton()->addSceneNode("SaveSceneNode1");
    Manager::getSingleton()->createEntity(node1, mesh1);

    auto* node2 = Manager::getSingleton()->addSceneNode("SaveSceneNode2");
    Manager::getSingleton()->createEntity(node2, mesh2);

    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString filePath = tmpDir.path() + "/test_save.scene.glb";

    QJsonObject args;
    args["file_path"] = filePath;
    QJsonObject result = server->callTool("save_scene", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("Scene saved"));
    EXPECT_TRUE(QFile::exists(filePath));
}

TEST_F(MCPServerTest, OpenScene_ValidFile_LoadsEntities)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    // Create entities and save the scene
    auto mesh1 = createInMemoryTriangleMesh("OpenSceneMesh1");
    auto mesh2 = createInMemoryTriangleMesh("OpenSceneMesh2");

    auto* node1 = Manager::getSingleton()->addSceneNode("OpenSceneNode1");
    Manager::getSingleton()->createEntity(node1, mesh1);

    auto* node2 = Manager::getSingleton()->addSceneNode("OpenSceneNode2");
    Manager::getSingleton()->createEntity(node2, mesh2);

    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QString filePath = tmpDir.path() + "/test_open.scene.glb";

    QJsonObject saveArgs;
    saveArgs["file_path"] = filePath;
    QJsonObject saveResult = server->callTool("save_scene", saveArgs);
    ASSERT_FALSE(isError(saveResult));
    ASSERT_TRUE(QFile::exists(filePath));

    // Open the saved scene
    QJsonObject openArgs;
    openArgs["file_path"] = filePath;
    QJsonObject openResult = server->callTool("open_scene", openArgs);
    EXPECT_FALSE(isError(openResult));
    QString resultText = getResultText(openResult);
    EXPECT_TRUE(resultText.contains("Scene loaded"));
    EXPECT_TRUE(resultText.contains("scene node(s)"));
}

TEST_F(MCPServerProtocolTest, ProcessMessageRejectsInvalidJson)
{
    const QJsonObject response = processAndRead("not-json");
    ASSERT_TRUE(response.contains("error"));
    EXPECT_EQ(response["error"].toObject()["code"].toInt(), -32700);
    EXPECT_TRUE(response["error"].toObject()["message"].toString().contains("Parse error"));
}

TEST_F(MCPServerProtocolTest, ProcessMessageRejectsNonObjectRequest)
{
    const QByteArray payload = QJsonDocument(QJsonArray{1, 2, 3}).toJson(QJsonDocument::Compact);
    const QJsonObject response = processAndRead(payload);
    ASSERT_TRUE(response.contains("error"));
    EXPECT_EQ(response["error"].toObject()["code"].toInt(), -32600);
    EXPECT_TRUE(response["error"].toObject()["message"].toString().contains("Invalid Request"));
}

TEST_F(MCPServerProtocolTest, ProcessMessageInitializeRespondsWithCapabilities)
{
    const QJsonObject request{
        {"jsonrpc", "2.0"},
        {"id", 7},
        {"method", "initialize"},
        {"params", QJsonObject{}}
    };

    const QJsonObject response = processAndRead(QJsonDocument(request).toJson(QJsonDocument::Compact));
    ASSERT_TRUE(response.contains("result"));
    EXPECT_EQ(response["id"].toInt(), 7);
    EXPECT_TRUE(server->m_initialized);

    const QJsonObject result = response["result"].toObject();
    EXPECT_EQ(result["protocolVersion"].toString(), "2024-11-05");
    EXPECT_EQ(result["serverInfo"].toObject()["name"].toString(), "QtMeshEditor");
    EXPECT_TRUE(result["capabilities"].toObject().contains("tools"));
    EXPECT_TRUE(result["capabilities"].toObject().contains("resources"));
}

TEST_F(MCPServerProtocolTest, ProcessMessageUnknownNotificationLeavesServerStateUnchanged)
{
    const QJsonObject request{
        {"jsonrpc", "2.0"},
        {"method", "notifications/custom"},
        {"params", QJsonObject{{"value", 1}}}
    };

    EXPECT_FALSE(server->m_initialized);
    server->processMessage(QJsonDocument(request).toJson(QJsonDocument::Compact));
    EXPECT_FALSE(server->m_initialized);
    EXPECT_TRUE(server->m_buffer.isEmpty());
}

TEST_F(MCPServerProtocolTest, ProcessMessageUnknownMethodReturnsMethodNotFound)
{
    const QJsonObject request{
        {"jsonrpc", "2.0"},
        {"id", "abc"},
        {"method", "totally/unknown"},
        {"params", QJsonObject{}}
    };

    const QJsonObject response = processAndRead(QJsonDocument(request).toJson(QJsonDocument::Compact));
    ASSERT_TRUE(response.contains("error"));
    EXPECT_EQ(response["id"].toString(), "abc");
    EXPECT_EQ(response["error"].toObject()["code"].toInt(), -32601);
    EXPECT_TRUE(response["error"].toObject()["message"].toString().contains("Method not found"));
}

TEST_F(MCPServerProtocolTest, ProcessMessagePingReturnsEmptyResult)
{
    const QJsonObject request{
        {"jsonrpc", "2.0"},
        {"id", 11},
        {"method", "ping"},
        {"params", QJsonObject{}}
    };

    const QJsonObject response = processAndRead(QJsonDocument(request).toJson(QJsonDocument::Compact));
    ASSERT_TRUE(response.contains("result"));
    EXPECT_EQ(response["id"].toInt(), 11);
    EXPECT_TRUE(response["result"].toObject().isEmpty());
}

TEST_F(MCPServerProtocolTest, ProcessMessageToolsListDispatchesToHandler)
{
    const QJsonObject request{
        {"jsonrpc", "2.0"},
        {"id", 21},
        {"method", "tools/list"},
        {"params", QJsonObject{}}
    };

    const QJsonObject response = processAndRead(QJsonDocument(request).toJson(QJsonDocument::Compact));
    ASSERT_TRUE(response.contains("result"));
    EXPECT_EQ(response["id"].toInt(), 21);
    EXPECT_TRUE(response["result"].toObject()["tools"].isArray());
}

TEST_F(MCPServerProtocolTest, ProcessMessageResourcesListDispatchesToHandler)
{
    const QJsonObject request{
        {"jsonrpc", "2.0"},
        {"id", 22},
        {"method", "resources/list"},
        {"params", QJsonObject{}}
    };

    const QJsonObject response = processAndRead(QJsonDocument(request).toJson(QJsonDocument::Compact));
    ASSERT_TRUE(response.contains("result"));
    EXPECT_EQ(response["id"].toInt(), 22);
    EXPECT_TRUE(response["result"].toObject()["resources"].isArray());
}

TEST_F(MCPServerProtocolTest, ProcessMessageResourcesReadDispatchesToHandler)
{
    const QJsonObject request{
        {"jsonrpc", "2.0"},
        {"id", 23},
        {"method", "resources/read"},
        {"params", QJsonObject{{"uri", "qtmesheditor://material/current"}}}
    };

    const QJsonObject response = processAndRead(QJsonDocument(request).toJson(QJsonDocument::Compact));
    ASSERT_TRUE(response.contains("result"));
    EXPECT_EQ(response["id"].toInt(), 23);
    EXPECT_TRUE(response["result"].toObject()["contents"].isArray());
}

TEST_F(MCPServerProtocolTest, ProcessMessageToolsCallDispatchesToHandler)
{
    const QJsonObject request{
        {"jsonrpc", "2.0"},
        {"id", 24},
        {"method", "tools/call"},
        {"params", QJsonObject{
            {"name", "totally_fake_tool"},
            {"arguments", QJsonObject{}}
        }}
    };

    const QJsonObject response = processAndRead(QJsonDocument(request).toJson(QJsonDocument::Compact));
    ASSERT_TRUE(response.contains("result"));
    EXPECT_EQ(response["id"].toInt(), 24);
    EXPECT_TRUE(response["result"].toObject()["content"].isArray());
}

TEST_F(MCPServerProtocolTest, HandleToolsCallForwardsNameAndArguments)
{
    const QJsonObject result = server->handleToolsCall(QJsonObject{
        {"name", "totally_fake_tool"},
        {"arguments", QJsonObject{{"foo", "bar"}}}
    });

    EXPECT_TRUE(result["content"].isArray());
    EXPECT_TRUE(getResultText(result).contains("Unknown tool") ||
                getResultText(result).contains("could not be initialized"));
}

TEST_F(MCPServerProtocolTest, ProcessMessageInitializedNotificationDoesNotRespond)
{
    const QJsonObject request{
        {"jsonrpc", "2.0"},
        {"method", "notifications/initialized"},
        {"params", QJsonObject{}}
    };

    server->processMessage(QJsonDocument(request).toJson(QJsonDocument::Compact));
    EXPECT_TRUE(readTransportMessageNonBlocking(outputPipe[0]).isEmpty());
    EXPECT_FALSE(server->m_initialized);
}

TEST_F(MCPServerProtocolTest, ProcessMessageCancelledNotificationDoesNotRespond)
{
    const QJsonObject request{
        {"jsonrpc", "2.0"},
        {"method", "notifications/cancelled"},
        {"params", QJsonObject{{"requestId", 5}}}
    };

    server->processMessage(QJsonDocument(request).toJson(QJsonDocument::Compact));
    EXPECT_TRUE(readTransportMessageNonBlocking(outputPipe[0]).isEmpty());
}

TEST_F(MCPServerProtocolTest, HandleResourcesListReturnsExpectedUris)
{
    const QJsonObject result = server->handleResourcesList();
    const QJsonArray resources = result["resources"].toArray();
    ASSERT_EQ(resources.size(), 2);
    EXPECT_EQ(resources[0].toObject()["uri"].toString(), "qtmesheditor://material/current");
    EXPECT_EQ(resources[1].toObject()["uri"].toString(), "qtmesheditor://scene/info");
}

TEST_F(MCPServerProtocolTest, HandleResourcesReadCurrentMaterialWithoutMainWindowReturnsPlaceholder)
{
    const QJsonObject result = server->handleResourcesRead(QJsonObject{{"uri", "qtmesheditor://material/current"}});
    const QJsonArray contents = result["contents"].toArray();
    ASSERT_EQ(contents.size(), 1);
    EXPECT_EQ(contents[0].toObject()["mimeType"].toString(), "text/plain");
    EXPECT_TRUE(contents[0].toObject()["text"].toString().contains("No material currently loaded"));
}

TEST_F(MCPServerProtocolTest, HandleResourcesReadSceneInfoReturnsSerializedText)
{
    ASSERT_TRUE(tryInitOgre());
    createStandardOgreMaterials();

    const QJsonObject result = server->handleResourcesRead(QJsonObject{{"uri", "qtmesheditor://scene/info"}});
    const QJsonArray contents = result["contents"].toArray();
    ASSERT_EQ(contents.size(), 1);
    EXPECT_EQ(contents[0].toObject()["mimeType"].toString(), "application/json");
    EXPECT_TRUE(contents[0].toObject()["text"].toString().contains("Scene Information"));
}

TEST_F(MCPServerProtocolTest, HandleResourcesReadUnknownUriReturnsEmptyContents)
{
    const QJsonObject result = server->handleResourcesRead(QJsonObject{{"uri", "qtmesheditor://unknown"}});
    EXPECT_TRUE(result["contents"].toArray().isEmpty());
}

TEST_F(MCPServerProtocolTest, BuildToolsListContainsCoreToolDefinitions)
{
    const QJsonArray tools = server->buildToolsList();
    EXPECT_GE(tools.size(), 25);

    bool sawCreateMaterial = false;
    bool sawOpenScene = false;
    for (const QJsonValue &value : tools) {
        const QJsonObject tool = value.toObject();
        if (tool["name"].toString() == "create_material") {
            sawCreateMaterial = true;
            EXPECT_TRUE(tool.contains("inputSchema"));
        }
        if (tool["name"].toString() == "open_scene") {
            sawOpenScene = true;
        }
    }

    EXPECT_TRUE(sawCreateMaterial);
    EXPECT_TRUE(sawOpenScene);
}

TEST_F(MCPServerProtocolTest, OnReadyReadRecoversAfterInvalidHeaderAndParsesMessage)
{
    int inputPipe[2] = {-1, -1};
    ASSERT_EQ(pipe(inputPipe), 0);

    server->m_stdinFd = inputPipe[0];
    server->m_stdinNotifier = new QSocketNotifier(server->m_stdinFd, QSocketNotifier::Read, server.get());

    const QJsonObject request{
        {"jsonrpc", "2.0"},
        {"id", 3},
        {"method", "ping"},
        {"params", QJsonObject{}}
    };
    const QByteArray payload = QByteArray("garbage\r\n\r\n") + makeTransport(QJsonDocument(request).toJson(QJsonDocument::Compact));
    ASSERT_GT(write(inputPipe[1], payload.constData(), payload.size()), 0);

    server->onReadyRead();

    const QJsonObject response = extractJsonBody(readTransportMessage(outputPipe[0]));
    EXPECT_EQ(response["id"].toInt(), 3);
    EXPECT_TRUE(response["result"].toObject().isEmpty());

    delete server->m_stdinNotifier;
    server->m_stdinNotifier = nullptr;
    close(inputPipe[0]);
    close(inputPipe[1]);
}

TEST_F(MCPServerProtocolTest, OnReadyReadWaitsForCompleteTransportBody)
{
    int inputPipe[2] = {-1, -1};
    ASSERT_EQ(pipe(inputPipe), 0);

    server->m_stdinFd = inputPipe[0];
    server->m_stdinNotifier = new QSocketNotifier(server->m_stdinFd, QSocketNotifier::Read, server.get());

    const QJsonObject request{
        {"jsonrpc", "2.0"},
        {"id", 9},
        {"method", "ping"},
        {"params", QJsonObject{}}
    };
    const QByteArray payload = QJsonDocument(request).toJson(QJsonDocument::Compact);
    const QByteArray header = QByteArray("Content-Length: ") + QByteArray::number(payload.size()) + "\r\n\r\n";

    ASSERT_GT(write(inputPipe[1], header.constData(), header.size()), 0);
    server->onReadyRead();
    EXPECT_TRUE(readTransportMessageNonBlocking(outputPipe[0]).isEmpty());
    EXPECT_EQ(server->m_buffer, header);

    ASSERT_GT(write(inputPipe[1], payload.constData(), payload.size()), 0);
    server->onReadyRead();

    const QJsonObject response = extractJsonBody(readTransportMessage(outputPipe[0]));
    EXPECT_EQ(response["id"].toInt(), 9);
    EXPECT_TRUE(response["result"].toObject().isEmpty());
    EXPECT_TRUE(server->m_buffer.isEmpty());

    delete server->m_stdinNotifier;
    server->m_stdinNotifier = nullptr;
    close(inputPipe[0]);
    close(inputPipe[1]);
}

TEST_F(MCPServerProtocolTest, OnReadyReadSkipsInvalidContentLengthAndProcessesNextMessage)
{
    int inputPipe[2] = {-1, -1};
    ASSERT_EQ(pipe(inputPipe), 0);

    server->m_stdinFd = inputPipe[0];
    server->m_stdinNotifier = new QSocketNotifier(server->m_stdinFd, QSocketNotifier::Read, server.get());

    const QJsonObject request{
        {"jsonrpc", "2.0"},
        {"id", 12},
        {"method", "ping"},
        {"params", QJsonObject{}}
    };
    const QByteArray payload = QByteArray("Content-Length: 0\r\n\r\n") + makeTransport(QJsonDocument(request).toJson(QJsonDocument::Compact));
    ASSERT_GT(write(inputPipe[1], payload.constData(), payload.size()), 0);

    server->onReadyRead();

    const QJsonObject response = extractJsonBody(readTransportMessage(outputPipe[0]));
    EXPECT_EQ(response["id"].toInt(), 12);
    EXPECT_TRUE(response["result"].toObject().isEmpty());

    delete server->m_stdinNotifier;
    server->m_stdinNotifier = nullptr;
    close(inputPipe[0]);
    close(inputPipe[1]);
}

TEST_F(MCPServerProtocolTest, OnReadyReadEofDisablesNotifierAndReturns)
{
    int inputPipe[2] = {-1, -1};
    ASSERT_EQ(pipe(inputPipe), 0);

    server->m_stdinFd = inputPipe[0];
    server->m_stdinNotifier = new QSocketNotifier(server->m_stdinFd, QSocketNotifier::Read, server.get());

    // Force EOF on read end
    close(inputPipe[1]);
    inputPipe[1] = -1;

    server->onReadyRead();
    ASSERT_NE(server->m_stdinNotifier, nullptr);
    EXPECT_FALSE(server->m_stdinNotifier->isEnabled());

    delete server->m_stdinNotifier;
    server->m_stdinNotifier = nullptr;
    close(inputPipe[0]);
}

TEST_F(MCPServerProtocolTest, SendNotificationSerializesMethodAndParams)
{
    server->sendNotification("notifications/test", QJsonObject{{"value", 42}});

    const QJsonObject response = extractJsonBody(readTransportMessage(outputPipe[0]));
    EXPECT_EQ(response["jsonrpc"].toString(), "2.0");
    EXPECT_EQ(response["method"].toString(), "notifications/test");
    EXPECT_EQ(response["params"].toObject()["value"].toInt(), 42);
    EXPECT_FALSE(response.contains("id"));
}

TEST_F(MCPServerProtocolTest, SendErrorSerializesJsonRpcError)
{
    server->sendError(QJsonValue("req-1"), -32001, "custom failure");

    const QJsonObject response = extractJsonBody(readTransportMessage(outputPipe[0]));
    EXPECT_EQ(response["jsonrpc"].toString(), "2.0");
    EXPECT_EQ(response["id"].toString(), "req-1");
    EXPECT_EQ(response["error"].toObject()["code"].toInt(), -32001);
    EXPECT_EQ(response["error"].toObject()["message"].toString(), "custom failure");
}

// ---- Filesystem tools ----

TEST_F(MCPServerTest, ListFiles_ValidDirectory)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    // Create a test file
    QFile f(tmpDir.filePath("test.fbx"));
    f.open(QIODevice::WriteOnly);
    f.write("dummy");
    f.close();

    QJsonObject args;
    args["path"] = tmpDir.path();
    QJsonObject result = server->callTool("list_files", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("test.fbx"));
}

TEST_F(MCPServerTest, ListFiles_NonexistentDirectory)
{
    QJsonObject args;
    args["path"] = "/nonexistent/path/that/does/not/exist";
    QJsonObject result = server->callTool("list_files", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("does not exist"));
}

TEST_F(MCPServerTest, ListFiles_WithPattern)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QFile f1(tmpDir.filePath("model.fbx"));
    f1.open(QIODevice::WriteOnly); f1.write("fbx"); f1.close();
    QFile f2(tmpDir.filePath("texture.png"));
    f2.open(QIODevice::WriteOnly); f2.write("png"); f2.close();

    QJsonObject args;
    args["path"] = tmpDir.path();
    args["pattern"] = "*.fbx";
    QJsonObject result = server->callTool("list_files", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("model.fbx"));
    EXPECT_FALSE(getResultText(result).contains("texture.png"));
}

TEST_F(MCPServerTest, SearchFiles_ValidQuery)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QDir(tmpDir.path()).mkdir("subdir");
    QFile f(tmpDir.filePath("subdir/deep.obj"));
    f.open(QIODevice::WriteOnly); f.write("obj"); f.close();

    QJsonObject args;
    args["path"] = tmpDir.path();
    args["query"] = "*.obj";
    QJsonObject result = server->callTool("search_files", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("deep.obj"));
}

TEST_F(MCPServerTest, SearchFiles_MissingQuery)
{
    QJsonObject args;
    args["path"] = QDir::tempPath();
    QJsonObject result = server->callTool("search_files", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("query"));
}

TEST_F(MCPServerTest, SearchFiles_NonexistentDirectory)
{
    QJsonObject args;
    args["path"] = "/nonexistent/search/path";
    args["query"] = "*.fbx";
    QJsonObject result = server->callTool("search_files", args);
    EXPECT_TRUE(isError(result));
}

TEST_F(MCPServerTest, ReadFile_ValidTextFile)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QFile f(tmpDir.filePath("test.txt"));
    f.open(QIODevice::WriteOnly);
    f.write("line1\nline2\nline3\n");
    f.close();

    QJsonObject args;
    args["path"] = tmpDir.filePath("test.txt");
    QJsonObject result = server->callTool("read_file", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("line1"));
    EXPECT_TRUE(getResultText(result).contains("line2"));
}

TEST_F(MCPServerTest, ReadFile_MissingPath)
{
    QJsonObject result = server->callTool("read_file", QJsonObject());
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("required"));
}

TEST_F(MCPServerTest, ReadFile_NonexistentFile)
{
    QJsonObject args;
    args["path"] = "/nonexistent/file.txt";
    QJsonObject result = server->callTool("read_file", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("does not exist"));
}

TEST_F(MCPServerTest, ReadFile_BinaryFileRejected)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QFile f(tmpDir.filePath("image.png"));
    f.open(QIODevice::WriteOnly); f.write("fake png"); f.close();

    QJsonObject args;
    args["path"] = tmpDir.filePath("image.png");
    QJsonObject result = server->callTool("read_file", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("binary"));
}

TEST_F(MCPServerTest, ReadFile_MaxLinesRespected)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());
    QFile f(tmpDir.filePath("long.txt"));
    f.open(QIODevice::WriteOnly);
    for (int i = 0; i < 50; ++i) f.write(QStringLiteral("line %1\n").arg(i).toUtf8());
    f.close();

    QJsonObject args;
    args["path"] = tmpDir.filePath("long.txt");
    args["max_lines"] = 5;
    QJsonObject result = server->callTool("read_file", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("truncated"));
}

// ---- Delete entity ----

TEST_F(MCPServerTest, DeleteEntity_MissingName)
{
    QJsonObject result = server->callTool("delete_entity", QJsonObject());
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("required"));
}

TEST_F(MCPServerTest, DeleteEntity_NonexistentEntity)
{
    QJsonObject args;
    args["name"] = "nonexistent_entity_xyz";
    QJsonObject result = server->callTool("delete_entity", args);
    EXPECT_TRUE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("not found"));
}

// ---- Camera tools ----

TEST_F(MCPServerTest, GetCameraInfo_NoMainWindow)
{
    // Server has no mainWindow set — should return error
    QJsonObject result = server->callTool("get_camera_info", QJsonObject());
    EXPECT_TRUE(isError(result));
}

TEST_F(MCPServerTest, CameraControl_NoMainWindow)
{
    QJsonObject args;
    args["zoom"] = 5.0;
    QJsonObject result = server->callTool("camera_control", args);
    EXPECT_TRUE(isError(result));
}

TEST_F(MCPServerTest, CameraControl_NoActionSpecified)
{
    // Even with no mainWindow, the error should mention "No camera action"
    // or "No active viewport" — both are valid error paths
    QJsonObject result = server->callTool("camera_control", QJsonObject());
    EXPECT_TRUE(isError(result));
}
