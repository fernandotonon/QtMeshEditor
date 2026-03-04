#include <gtest/gtest.h>
#include <QApplication>
#include <QThread>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QTcpSocket>
#include <QSignalSpy>
#include <QElapsedTimer>
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
        "merge_animations"
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
    args["path"] = "/tmp/mcp_inmemory_export.obj";
    QJsonObject result = server->callTool("export_mesh", args);
    EXPECT_FALSE(isError(result));

    QFile::remove("/tmp/mcp_inmemory_export.obj");
    QFile::remove("/tmp/mcp_inmemory_export.material");
    QFile::remove("/tmp/mcp_inmemory_export.mtl");
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
