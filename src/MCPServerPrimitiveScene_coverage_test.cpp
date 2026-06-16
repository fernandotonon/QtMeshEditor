// Coverage test for MCPServer::toolCreatePrimitive (create_primitive) and
// MCPServer::toolGetSceneInfo (get_scene_info).
//
// This is a DISTINCT file/suite from MCPServer_test.cpp — it uses the suite
// name MCPServerPrimitiveSceneCoverageTest and local helpers so there is no
// ODR clash with the existing MCPServerTest suite.
//
// Target gaps exercised here (per coverage survey):
//   create_primitive:
//     - auto-name-generation branch (name omitted -> type_<timestamp>)
//     - 'box' alias mapping to AP_CUBE (text still says "box")
//     - the FULL typeMap set in one place
//       (cube/box/sphere/plane/cylinder/cone/torus/tube/capsule/
//        icosphere/roundedbox/spring)
//     - empty-type error
//     - unknown-type error
//     - actualName-returned path when Manager appends a suffix on a
//       duplicate explicit name
//   get_scene_info:
//     - entity-with-material detail line ("(material: X)")
//     - multiple nodes/entities counting
//     - materialCount iteration ("Materials loaded:")
//     - "(none)" branch for an empty scene
//     - node-name join

#include <gtest/gtest.h>

#include <QApplication>
#include <QThread>
#include <QJsonArray>
#include <QJsonObject>

#include <OgreEntity.h>
#include <OgreSubEntity.h>
#include <OgreSceneNode.h>
#include <OgreSceneManager.h>
#include <OgreMeshManager.h>

#include "MCPServer.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "PrimitiveObject.h"
#include "TestHelpers.h"

namespace {

// Local result accessors (kept local to avoid linking against the existing
// suite's file-static helpers — distinct translation unit).
QString primSceneResultText(const QJsonObject &result)
{
    const QJsonArray content = result.value("content").toArray();
    if (content.isEmpty()) return QString();
    return content.first().toObject().value("text").toString();
}

bool primSceneIsError(const QJsonObject &result)
{
    return result.value("isError").toBool(false);
}

} // namespace

class MCPServerPrimitiveSceneCoverageTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        server.reset();
        Manager::kill();
        QThread::msleep(20);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();

        server = std::make_unique<MCPServer>();
    }

    void TearDown() override
    {
        if (SelectionSet::getSingletonPtr()) {
            SelectionSet::getSingleton()->clear();
        }
        server.reset();
        Manager::kill();
        if (app) {
            app->processEvents();
        }
        QThread::msleep(10);
    }

    // Create an in-memory triangle entity attached to a fresh scene node and
    // select it. Returns the entity (or nullptr). The entity name differs
    // from the node base name (suffix "_entity"), matching the existing
    // suite's convention.
    Ogre::Entity* createAndSelectTriangleEntity(const QString& baseName)
    {
        auto* manager = Manager::getSingletonPtr();
        if (!manager) return nullptr;

        Ogre::MeshPtr mesh = createInMemoryTriangleMesh((baseName + "_mesh").toStdString());
        if (!mesh) return nullptr;

        Ogre::SceneManager* sceneMgr = manager->getSceneMgr();
        if (!sceneMgr) return nullptr;

        Ogre::SceneNode* node = manager->addSceneNode(baseName);
        if (!node) return nullptr;

        Ogre::Entity* entity = sceneMgr->createEntity((baseName + "_entity").toStdString(), mesh);
        if (!entity) return nullptr;

        node->attachObject(entity);
        SelectionSet::getSingleton()->clear();
        SelectionSet::getSingleton()->selectOne(entity);
        app->processEvents();
        return entity;
    }

    QJsonObject createPrimitive(const QString& type, const QString& name = QString())
    {
        QJsonObject args;
        if (!type.isNull()) args["type"] = type;
        if (!name.isEmpty()) args["name"] = name;
        return server->callTool("create_primitive", args);
    }

    QApplication* app = nullptr;
    std::unique_ptr<MCPServer> server;
};

// ---------------------------------------------------------------------------
// create_primitive — error branches
// ---------------------------------------------------------------------------

TEST_F(MCPServerPrimitiveSceneCoverageTest, CreatePrimitiveEmptyTypeReturnsError)
{
    QJsonObject args;
    args["type"] = ""; // empty -> the "type is required" branch
    QJsonObject result = server->callTool("create_primitive", args);
    EXPECT_TRUE(primSceneIsError(result));
    EXPECT_TRUE(primSceneResultText(result).contains("type is required"));
}

TEST_F(MCPServerPrimitiveSceneCoverageTest, CreatePrimitiveMissingTypeKeyReturnsError)
{
    // No "type" key at all -> toString() is empty -> same error branch.
    QJsonObject result = server->callTool("create_primitive", QJsonObject());
    EXPECT_TRUE(primSceneIsError(result));
    EXPECT_TRUE(primSceneResultText(result).contains("type is required"));
}

TEST_F(MCPServerPrimitiveSceneCoverageTest, CreatePrimitiveUnknownTypeReturnsError)
{
    QJsonObject args;
    args["type"] = "dodecahedron";
    QJsonObject result = server->callTool("create_primitive", args);
    EXPECT_TRUE(primSceneIsError(result));
    QString text = primSceneResultText(result);
    EXPECT_TRUE(text.contains("Unknown primitive type"));
    // The error echoes the offending lowercased type.
    EXPECT_TRUE(text.contains("dodecahedron"));
}

TEST_F(MCPServerPrimitiveSceneCoverageTest, CreatePrimitiveTypeIsCaseInsensitive)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
    // toLower() in the handler -> "SPHERE" maps the same as "sphere".
    QJsonObject result = createPrimitive("SPHERE", "MixedCaseSphere");
    EXPECT_FALSE(primSceneIsError(result));
    QString text = primSceneResultText(result);
    EXPECT_TRUE(text.contains("Created"));
    EXPECT_TRUE(text.contains("sphere")); // type is lowercased in the message
}

// ---------------------------------------------------------------------------
// create_primitive — full typeMap set (one assertion per key)
// ---------------------------------------------------------------------------

TEST_F(MCPServerPrimitiveSceneCoverageTest, CreatePrimitiveAllTypeMapKeys)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";

    const QStringList allTypes = {
        "cube", "box", "sphere", "plane", "cylinder", "cone",
        "torus", "tube", "capsule", "icosphere", "roundedbox", "spring"
    };

    for (const QString& type : allTypes) {
        QJsonObject result = createPrimitive(type, "All_" + type);
        EXPECT_FALSE(primSceneIsError(result))
            << "Failed to create primitive type: " << type.toStdString();
        QString text = primSceneResultText(result);
        EXPECT_TRUE(text.contains("Created"))
            << "No 'Created' in result for type: " << type.toStdString();
        // The result text echoes the requested type verbatim — crucially,
        // 'box' yields "box" even though it maps to AP_CUBE internally.
        EXPECT_TRUE(text.contains(type))
            << "Result text missing type token: " << type.toStdString();
    }
}

TEST_F(MCPServerPrimitiveSceneCoverageTest, CreatePrimitiveBoxAliasMapsToCubeButKeepsBoxText)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
    // 'box' is a typeMap alias for AP_CUBE; the success message still says
    // "box" (it echoes the requested type, not the enum).
    QJsonObject result = createPrimitive("box", "BoxAliasObj");
    EXPECT_FALSE(primSceneIsError(result));
    QString text = primSceneResultText(result);
    EXPECT_TRUE(text.contains("box"));
    EXPECT_FALSE(text.contains("cube")); // not remapped in the text
    EXPECT_TRUE(text.contains("BoxAliasObj"));
}

// ---------------------------------------------------------------------------
// create_primitive — auto name generation branch
// ---------------------------------------------------------------------------

TEST_F(MCPServerPrimitiveSceneCoverageTest, CreatePrimitiveAutoGeneratesNameWhenOmitted)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
    // No "name" key -> handler builds "type_<timestamp>".
    QJsonObject args;
    args["type"] = "cone";
    QJsonObject result = server->callTool("create_primitive", args);
    EXPECT_FALSE(primSceneIsError(result));
    QString text = primSceneResultText(result);
    EXPECT_TRUE(text.contains("Created"));
    // Auto name carries the type prefix followed by "_<digits>".
    EXPECT_TRUE(text.contains("cone_")) << text.toStdString();
}

TEST_F(MCPServerPrimitiveSceneCoverageTest, CreatePrimitiveEmptyNameAlsoAutoGenerates)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
    // Explicit empty name string -> isEmpty() true -> auto-gen branch.
    QJsonObject result = createPrimitive("torus", QString("")); // empty -> auto
    EXPECT_FALSE(primSceneIsError(result));
    EXPECT_TRUE(primSceneResultText(result).contains("torus_"));
}

// ---------------------------------------------------------------------------
// create_primitive — duplicate explicit name -> Manager appends a suffix,
// the handler returns the ACTUAL (suffixed) node name.
// ---------------------------------------------------------------------------

TEST_F(MCPServerPrimitiveSceneCoverageTest, CreatePrimitiveDuplicateNameReturnsSuffixedActualName)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";

    const QString dupName = "DupPrim";
    QJsonObject first = createPrimitive("sphere", dupName);
    ASSERT_FALSE(primSceneIsError(first));
    const QString firstText = primSceneResultText(first);
    EXPECT_TRUE(firstText.contains(dupName));

    // Second create with the SAME explicit name — Manager must rename to
    // avoid a collision, so the returned actualName differs from the first.
    QJsonObject second = createPrimitive("sphere", dupName);
    ASSERT_FALSE(primSceneIsError(second));
    const QString secondText = primSceneResultText(second);

    EXPECT_TRUE(secondText.contains("Created"));
    // The returned text must differ from the first (suffix appended), proving
    // the actualName path executed.
    EXPECT_NE(firstText, secondText)
        << "first='" << firstText.toStdString()
        << "' second='" << secondText.toStdString() << "'";
}

// ---------------------------------------------------------------------------
// get_scene_info — empty scene "(none)" branches
// ---------------------------------------------------------------------------

TEST_F(MCPServerPrimitiveSceneCoverageTest, GetSceneInfoEmptySceneShowsNone)
{
    QJsonObject result = server->callTool("get_scene_info", QJsonObject());
    EXPECT_FALSE(primSceneIsError(result));
    QString text = primSceneResultText(result);
    EXPECT_TRUE(text.contains("Scene Information"));
    EXPECT_TRUE(text.contains("Scene Nodes:"));
    EXPECT_TRUE(text.contains("Entities:"));
    EXPECT_TRUE(text.contains("Materials loaded:"));
    // No nodes / no entities -> both "(none)" branches fire.
    EXPECT_TRUE(text.contains("(none)"));
}

// ---------------------------------------------------------------------------
// get_scene_info — header fields + materialCount iteration with content
// ---------------------------------------------------------------------------

TEST_F(MCPServerPrimitiveSceneCoverageTest, GetSceneInfoReportsHeaderFieldsAndMaterials)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";

    QJsonObject prim = createPrimitive("cube", "HeaderCube");
    ASSERT_FALSE(primSceneIsError(prim));

    QJsonObject result = server->callTool("get_scene_info", QJsonObject());
    EXPECT_FALSE(primSceneIsError(result));
    QString text = primSceneResultText(result);

    EXPECT_TRUE(text.contains("Scene Nodes:"));
    EXPECT_TRUE(text.contains("Entities:"));
    EXPECT_TRUE(text.contains("Materials loaded:"));
    // createStandardOgreMaterials registered several materials, so the
    // materialCount iteration loop ran and reported a non-(none) count.
    EXPECT_FALSE(text.contains("Materials loaded: 0"));
    // The created node name should appear in the joined node list.
    EXPECT_TRUE(text.contains("HeaderCube"));
}

// ---------------------------------------------------------------------------
// get_scene_info — entity-with-material detail line "(material: X)"
// ---------------------------------------------------------------------------

TEST_F(MCPServerPrimitiveSceneCoverageTest, GetSceneInfoShowsEntityMaterialDetail)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";

    // Build an entity with a known sub-entity material, then verify the
    // "(material: BaseWhite)" detail line is emitted.
    Ogre::Entity* entity = createAndSelectTriangleEntity("MatEntity");
    ASSERT_NE(entity, nullptr);
    ASSERT_GT(entity->getNumSubEntities(), 0u);

    // Apply a registered material via the MCP tool (drives the entity-name
    // lookup path); BaseWhite is created by createStandardOgreMaterials().
    QJsonObject applyArgs;
    applyArgs["material"] = "BaseWhite";
    applyArgs["entity"] = QString::fromStdString(entity->getName());
    QJsonObject applyResult = server->callTool("apply_material", applyArgs);
    ASSERT_FALSE(primSceneIsError(applyResult)) << primSceneResultText(applyResult).toStdString();

    QJsonObject result = server->callTool("get_scene_info", QJsonObject());
    EXPECT_FALSE(primSceneIsError(result));
    QString text = primSceneResultText(result);

    // The entity name and its sub-entity material detail line.
    EXPECT_TRUE(text.contains(QString::fromStdString(entity->getName())));
    EXPECT_TRUE(text.contains("(material:")) << text.toStdString();
    EXPECT_TRUE(text.contains("BaseWhite")) << text.toStdString();
}

// ---------------------------------------------------------------------------
// get_scene_info — multiple nodes/entities counting + node-name join
// ---------------------------------------------------------------------------

TEST_F(MCPServerPrimitiveSceneCoverageTest, GetSceneInfoCountsMultipleNodesAndEntities)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";

    ASSERT_FALSE(primSceneIsError(createPrimitive("cube", "MultiA")));
    ASSERT_FALSE(primSceneIsError(createPrimitive("sphere", "MultiB")));
    ASSERT_FALSE(primSceneIsError(createPrimitive("cylinder", "MultiC")));

    QJsonObject result = server->callTool("get_scene_info", QJsonObject());
    EXPECT_FALSE(primSceneIsError(result));
    QString text = primSceneResultText(result);

    // All three node names appear in the joined Nodes list (node-name join).
    EXPECT_TRUE(text.contains("MultiA"));
    EXPECT_TRUE(text.contains("MultiB"));
    EXPECT_TRUE(text.contains("MultiC"));
    // Entities are listed with the "  - " prefix used by the handler.
    EXPECT_TRUE(text.contains("  - "));
    // Counts must be at least 3 nodes — assert the field exists with a
    // multi-digit-or-3+ value indirectly by confirming none-branch absent.
    EXPECT_FALSE(text.contains("- Scene Nodes: 0"));
    EXPECT_FALSE(text.contains("- Entities: 0"));
}
