#include <gtest/gtest.h>
#include <QApplication>
#include <QThread>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <memory>
#include "MCPServer.h"
#include "Manager.h"
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

class MCPServerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        try {
            Manager::getSingleton();  // headless — no render window needed
        } catch (const Ogre::Exception& e) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed (" << e.getFullDescription() << ")";
        }
        createStandardOgreMaterials();

        server = std::make_unique<MCPServer>();
        // No mainWindow set — tests that need it will test the error path
    }

    void TearDown() override
    {
        server.reset();

        Manager::kill();

        if (app)
        {
            app->processEvents();
        }

        QThread::msleep(50);
    }

    QApplication* app = nullptr;
    std::unique_ptr<MCPServer> server;
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
    QJsonObject args;
    args["type"] = "sphere";
    args["name"] = "TestSphere";
    QJsonObject result = server->callTool("create_primitive", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("TestSphere"));
}

TEST_F(MCPServerTest, CreatePrimitiveTypes)
{
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

// --- Take screenshot (no MainWindow — test error path) ---

TEST_F(MCPServerTest, TakeScreenshotNoMainWindow)
{
    QJsonObject args;
    args["path"] = "/tmp/mcp_test_screenshot.png";
    QJsonObject result = server->callTool("take_screenshot", args);
    // Without MainWindow, screenshot should return an error or handle gracefully
    EXPECT_FALSE(getResultText(result).isEmpty());
}

// --- Load mesh (no MainWindow — test error path) ---

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
        "take_screenshot", "create_primitive", "animate"
    };

    for (const QString &tool : knownTools) {
        // Just call with empty args - we only care that it's not "Unknown tool"
        QJsonObject result = server->callTool(tool, QJsonObject());
        EXPECT_FALSE(getResultText(result).contains("Unknown tool"))
            << "Tool should be recognized: " << tool.toStdString();
    }
}
