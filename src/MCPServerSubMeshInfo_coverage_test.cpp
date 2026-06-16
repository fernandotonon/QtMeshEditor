// Coverage tests for MCPServer::toolTransformSubMesh ("transform_submesh") and
// the multi-entity / selection / empty-scene branches of MCPServer::toolGetMeshInfo
// ("get_mesh_info"). Distinct fixture + suite names to avoid ODR/registration
// clashes with MCPServer_test.cpp.

#include <gtest/gtest.h>
#include <QApplication>
#include <QThread>
#include <QJsonArray>
#include <QJsonObject>
#include <memory>

#define private public
#include "MCPServer.h"
#undef private

#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

#include <OgreEntity.h>
#include <OgreSceneNode.h>
#include <OgreSceneManager.h>

// File-local result accessors (originals in MCPServer_test.cpp are static/file-local).
namespace {

QString smiGetResultText(const QJsonObject &result)
{
    QJsonArray content = result["content"].toArray();
    if (content.isEmpty()) return QString();
    return content[0].toObject()["text"].toString();
}

bool smiIsError(const QJsonObject &result)
{
    return result["isError"].toBool(false);
}

} // namespace

class MCPServerSubMeshInfoCoverageTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        server.reset();
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();

        server = std::make_unique<MCPServer>();
    }

    void TearDown() override
    {
        SelectionSet::getSingleton()->clear();
        Manager::kill();
        if (app) {
            app->processEvents();
        }
    }

    // Creates a single-submesh triangle entity, attaches it to a fresh scene
    // node and selects it. Returns the entity (named baseName + "_entity").
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

    static QJsonArray vec3(double x, double y, double z)
    {
        QJsonArray a;
        a.append(x); a.append(y); a.append(z);
        return a;
    }

    QApplication* app = nullptr;
    std::unique_ptr<MCPServer> server;
};

// ---------------------------------------------------------------------------
// transform_submesh
// ---------------------------------------------------------------------------

TEST_F(MCPServerSubMeshInfoCoverageTest, TransformSubMeshMissingEntityNameErrors)
{
    QJsonObject args;
    args["submesh_index"] = 0;
    args["translate"] = vec3(1, 0, 0);
    QJsonObject result = server->callTool("transform_submesh", args);
    EXPECT_TRUE(smiIsError(result));
    EXPECT_TRUE(smiGetResultText(result).contains("entity_name"));
}

TEST_F(MCPServerSubMeshInfoCoverageTest, TransformSubMeshNegativeIndexErrors)
{
    QJsonObject args;
    args["entity_name"] = "AnyEntity";
    args["submesh_index"] = -1;
    args["translate"] = vec3(1, 0, 0);
    QJsonObject result = server->callTool("transform_submesh", args);
    EXPECT_TRUE(smiIsError(result));
    EXPECT_TRUE(smiGetResultText(result).contains("non-negative"));
}

TEST_F(MCPServerSubMeshInfoCoverageTest, TransformSubMeshAbsentIndexErrors)
{
    // submesh_index defaults to -1 when absent.
    QJsonObject args;
    args["entity_name"] = "AnyEntity";
    args["translate"] = vec3(1, 0, 0);
    QJsonObject result = server->callTool("transform_submesh", args);
    EXPECT_TRUE(smiIsError(result));
    EXPECT_TRUE(smiGetResultText(result).contains("non-negative"));
}

TEST_F(MCPServerSubMeshInfoCoverageTest, TransformSubMeshEntityNotFoundErrors)
{
    QJsonObject args;
    args["entity_name"] = "NoSuchEntityXYZ";
    args["submesh_index"] = 0;
    args["translate"] = vec3(1, 0, 0);
    QJsonObject result = server->callTool("transform_submesh", args);
    EXPECT_TRUE(smiIsError(result));
    QString text = smiGetResultText(result);
    EXPECT_TRUE(text.contains("not found"));
    EXPECT_TRUE(text.contains("NoSuchEntityXYZ"));
}

TEST_F(MCPServerSubMeshInfoCoverageTest, TransformSubMeshIndexOutOfRangeErrors)
{
    Ogre::Entity* entity = createAndSelectTriangleEntity("SmiOutOfRange");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity_name"] = "SmiOutOfRange_entity";
    args["submesh_index"] = 1; // single-submesh mesh => index 1 is out of range
    args["translate"] = vec3(1, 0, 0);
    QJsonObject result = server->callTool("transform_submesh", args);
    EXPECT_TRUE(smiIsError(result));
    EXPECT_TRUE(smiGetResultText(result).contains("out of range"));
}

TEST_F(MCPServerSubMeshInfoCoverageTest, TransformSubMeshTranslateOnlySucceeds)
{
    Ogre::Entity* entity = createAndSelectTriangleEntity("SmiTranslate");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity_name"] = "SmiTranslate_entity";
    args["submesh_index"] = 0;
    args["translate"] = vec3(1, 2, 3);
    QJsonObject result = server->callTool("transform_submesh", args);
    EXPECT_FALSE(smiIsError(result));
    QString text = smiGetResultText(result);
    EXPECT_TRUE(text.contains("translate"));
    EXPECT_FALSE(text.contains("rotate"));
    EXPECT_FALSE(text.contains("scale"));
}

TEST_F(MCPServerSubMeshInfoCoverageTest, TransformSubMeshRotateOnlySucceeds)
{
    Ogre::Entity* entity = createAndSelectTriangleEntity("SmiRotate");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity_name"] = "SmiRotate_entity";
    args["submesh_index"] = 0;
    args["rotate"] = vec3(0, 90, 0);
    QJsonObject result = server->callTool("transform_submesh", args);
    EXPECT_FALSE(smiIsError(result));
    QString text = smiGetResultText(result);
    EXPECT_TRUE(text.contains("rotate"));
    EXPECT_FALSE(text.contains("translate"));
}

TEST_F(MCPServerSubMeshInfoCoverageTest, TransformSubMeshScaleOnlySucceeds)
{
    Ogre::Entity* entity = createAndSelectTriangleEntity("SmiScale");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity_name"] = "SmiScale_entity";
    args["submesh_index"] = 0;
    args["scale"] = vec3(2, 2, 2);
    QJsonObject result = server->callTool("transform_submesh", args);
    EXPECT_FALSE(smiIsError(result));
    QString text = smiGetResultText(result);
    EXPECT_TRUE(text.contains("scale"));
    EXPECT_FALSE(text.contains("rotate"));
}

TEST_F(MCPServerSubMeshInfoCoverageTest, TransformSubMeshCombinedSucceeds)
{
    Ogre::Entity* entity = createAndSelectTriangleEntity("SmiCombined");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity_name"] = "SmiCombined_entity";
    args["submesh_index"] = 0;
    args["translate"] = vec3(1, 0, 0);
    args["rotate"] = vec3(10, 20, 30);
    args["scale"] = vec3(1.5, 1.5, 1.5);
    QJsonObject result = server->callTool("transform_submesh", args);
    EXPECT_FALSE(smiIsError(result));
    QString text = smiGetResultText(result);
    EXPECT_TRUE(text.contains("translate"));
    EXPECT_TRUE(text.contains("rotate"));
    EXPECT_TRUE(text.contains("scale"));
}

TEST_F(MCPServerSubMeshInfoCoverageTest, TransformSubMeshNoTransformSpecifiedErrors)
{
    Ogre::Entity* entity = createAndSelectTriangleEntity("SmiNoTransform");
    ASSERT_NE(entity, nullptr);

    QJsonObject args;
    args["entity_name"] = "SmiNoTransform_entity";
    args["submesh_index"] = 0;
    QJsonObject result = server->callTool("transform_submesh", args);
    EXPECT_TRUE(smiIsError(result));
    EXPECT_TRUE(smiGetResultText(result).contains("No transform specified"));
}

// ---------------------------------------------------------------------------
// get_mesh_info
// ---------------------------------------------------------------------------

TEST_F(MCPServerSubMeshInfoCoverageTest, GetMeshInfoEmptySceneReportsNoEntities)
{
    SelectionSet::getSingleton()->clear();
    app->processEvents();

    QJsonObject result = server->callTool("get_mesh_info", QJsonObject());
    EXPECT_FALSE(smiIsError(result));
    EXPECT_TRUE(smiGetResultText(result).contains("No entities in scene"));
}

TEST_F(MCPServerSubMeshInfoCoverageTest, GetMeshInfoExplicitSelectionPath)
{
    Ogre::Entity* entity = createAndSelectTriangleEntity("MiSelected");
    ASSERT_NE(entity, nullptr);
    ASSERT_GT(SelectionSet::getSingleton()->getEntitiesCount(), 0);

    QJsonObject result = server->callTool("get_mesh_info", QJsonObject());
    EXPECT_FALSE(smiIsError(result));
    QString text = smiGetResultText(result);
    EXPECT_TRUE(text.contains("Mesh Information"));
    EXPECT_TRUE(text.contains("MiSelected_entity"));
    EXPECT_TRUE(text.contains("Vertices:"));
    EXPECT_TRUE(text.contains("Triangles:"));
    EXPECT_TRUE(text.contains("SubMeshes:"));
    EXPECT_TRUE(text.contains("Materials:"));
    EXPECT_TRUE(text.contains("Position:"));
    EXPECT_TRUE(text.contains("Scale:"));
    EXPECT_TRUE(text.contains("1 entities"));
}

TEST_F(MCPServerSubMeshInfoCoverageTest, GetMeshInfoMultipleSelectedEntities)
{
    Ogre::Entity* e1 = createAndSelectTriangleEntity("MiMultiA");
    ASSERT_NE(e1, nullptr);
    Ogre::Entity* e2 = createAndSelectTriangleEntity("MiMultiB");
    ASSERT_NE(e2, nullptr);

    // Select both so the explicit-selection loop builds two info blocks.
    SelectionSet::getSingleton()->clear();
    SelectionSet::getSingleton()->append(e1);
    SelectionSet::getSingleton()->append(e2);
    app->processEvents();
    ASSERT_EQ(SelectionSet::getSingleton()->getEntitiesCount(), 2);

    QJsonObject result = server->callTool("get_mesh_info", QJsonObject());
    EXPECT_FALSE(smiIsError(result));
    QString text = smiGetResultText(result);
    EXPECT_TRUE(text.contains("2 entities"));
    EXPECT_TRUE(text.contains("MiMultiA_entity"));
    EXPECT_TRUE(text.contains("MiMultiB_entity"));
}

TEST_F(MCPServerSubMeshInfoCoverageTest, GetMeshInfoNoSelectionReportsAllEntities)
{
    Ogre::Entity* e1 = createAndSelectTriangleEntity("MiAllA");
    ASSERT_NE(e1, nullptr);
    Ogre::Entity* e2 = createAndSelectTriangleEntity("MiAllB");
    ASSERT_NE(e2, nullptr);

    // No selection => entitiesToReport = mgr->getEntities() path.
    SelectionSet::getSingleton()->clear();
    app->processEvents();
    ASSERT_EQ(SelectionSet::getSingleton()->getEntitiesCount(), 0);

    QJsonObject result = server->callTool("get_mesh_info", QJsonObject());
    EXPECT_FALSE(smiIsError(result));
    QString text = smiGetResultText(result);
    EXPECT_TRUE(text.contains("Mesh Information"));
    EXPECT_TRUE(text.contains("MiAllA_entity"));
    EXPECT_TRUE(text.contains("MiAllB_entity"));
}
