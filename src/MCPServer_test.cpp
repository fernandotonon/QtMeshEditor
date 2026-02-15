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
            Manager::getSingleton();  // headless -- no render window needed
        } catch (const Ogre::Exception& e) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed (" << e.getFullDescription() << ")";
        }
        createStandardOgreMaterials();

        server = std::make_unique<MCPServer>();
        // No mainWindow set -- tests that need it will test the error path
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
        "take_screenshot", "create_primitive", "animate"
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
    QJsonObject args;
    args["type"] = "plane";
    args["name"] = "TestPlane";
    QJsonObject result = server->callTool("create_primitive", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("TestPlane"));
}

TEST_F(MCPServerTest, CreatePrimitiveTorus)
{
    QJsonObject args;
    args["type"] = "torus";
    args["name"] = "TestTorus";
    QJsonObject result = server->callTool("create_primitive", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("TestTorus"));
}

TEST_F(MCPServerTest, CreatePrimitiveTube)
{
    QJsonObject args;
    args["type"] = "tube";
    args["name"] = "TestTube";
    QJsonObject result = server->callTool("create_primitive", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("TestTube"));
}

TEST_F(MCPServerTest, CreatePrimitiveCapsule)
{
    QJsonObject args;
    args["type"] = "capsule";
    args["name"] = "TestCapsule";
    QJsonObject result = server->callTool("create_primitive", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("TestCapsule"));
}

TEST_F(MCPServerTest, CreatePrimitiveIcoSphere)
{
    QJsonObject args;
    args["type"] = "icosphere";
    args["name"] = "TestIcoSphere";
    QJsonObject result = server->callTool("create_primitive", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("TestIcoSphere"));
}

TEST_F(MCPServerTest, CreatePrimitiveSpring)
{
    QJsonObject args;
    args["type"] = "spring";
    args["name"] = "TestSpring";
    QJsonObject result = server->callTool("create_primitive", args);
    EXPECT_FALSE(isError(result));
    EXPECT_TRUE(getResultText(result).contains("TestSpring"));
}

TEST_F(MCPServerTest, CreatePrimitiveAutoGeneratedName)
{
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
