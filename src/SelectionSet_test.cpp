#include <gtest/gtest.h>
#include <QSignalSpy>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include "Manager.h"
#include <QMap>
#include "SelectionSet.h"
#include "PrimitiveObject.h"
#include "TestHelpers.h"

class SelectionSetTests : public ::testing::Test {
protected:
    Ogre::SceneNode* createNodeWithEntity(const QString& nodeName, const std::string& meshName)
    {
        Ogre::MeshPtr mesh = createInMemoryTriangleMesh(meshName);
        EXPECT_NE(mesh, nullptr);
        if (!mesh) {
            return nullptr;
        }

        Ogre::SceneNode* node = Manager::getSingleton()->addSceneNode(nodeName);
        EXPECT_NE(node, nullptr);
        if (!node) {
            return nullptr;
        }

        Ogre::Entity* entity = Manager::getSingleton()->createEntity(node, mesh);
        EXPECT_NE(entity, nullptr);
        if (!entity) {
            return nullptr;
        }

        // Manager::addSceneNode auto-selects newly created nodes; tests using this
        // helper need to control the resulting selection state explicitly.
        SelectionSet::getSingleton()->clear();

        return node;
    }

    void SetUp() override {
        Manager::kill();
        SelectionSet::kill();
        QThread::msleep(50);
        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
        }
    }
    void TearDown() override {
        SelectionSet::kill();
        Manager::kill();
        auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
        if (app) app->processEvents();
        QThread::msleep(50);
    }
};

TEST_F(SelectionSetTests, AppendSceneNode)
{
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    auto sceneNode = Manager::getSingleton()->addSceneNode("test");

    selectionSet->append(sceneNode);

    EXPECT_EQ(selectionSet->getNodesCount(), 1);
    EXPECT_TRUE(selectionSet->contains(sceneNode));

    auto sceneNode2 = Manager::getSingleton()->addSceneNode("test2");
    selectionSet->append(sceneNode);
    selectionSet->append(sceneNode2);

    EXPECT_EQ(selectionSet->getNodesCount(), 2);
    EXPECT_TRUE(selectionSet->contains(sceneNode));
    EXPECT_TRUE(selectionSet->contains(sceneNode2));

    Manager::getSingleton()->destroySceneNode(sceneNode);
    Manager::getSingleton()->destroySceneNode(sceneNode2);
}

TEST_F(SelectionSetTests, RemoveSceneNode)
{
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    auto sceneNode = Manager::getSingleton()->addSceneNode("test");

    selectionSet->append(sceneNode);
    bool removed = selectionSet->removeOne(sceneNode);

    EXPECT_TRUE(removed);
    EXPECT_EQ(selectionSet->getNodesCount(), 0);
    EXPECT_FALSE(selectionSet->contains(sceneNode));

    Manager::getSingleton()->destroySceneNode(sceneNode);
}

TEST_F(SelectionSetTests, SelectSceneNode)
{
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    auto sceneNode = Manager::getSingleton()->addSceneNode("test");

    selectionSet->selectOne(sceneNode);

    EXPECT_EQ(selectionSet->getNodesCount(), 1);
    EXPECT_TRUE(selectionSet->contains(sceneNode));

    Manager::getSingleton()->destroySceneNode(sceneNode);
}

TEST_F(SelectionSetTests, Clear)
{
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    auto sceneNode = Manager::getSingleton()->addSceneNode("test");

    selectionSet->append(sceneNode);
    selectionSet->clear();

    EXPECT_EQ(selectionSet->getNodesCount(), 0);
    EXPECT_FALSE(selectionSet->contains(sceneNode));

    Manager::getSingleton()->destroySceneNode(sceneNode);
}

TEST_F(SelectionSetTests, ClearList)
{
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    auto sceneNode = Manager::getSingleton()->addSceneNode("test");

    selectionSet->append(sceneNode);
    selectionSet->clearList();

    EXPECT_EQ(selectionSet->getNodesCount(), 0);
    EXPECT_FALSE(selectionSet->contains(sceneNode));
    Manager::getSingleton()->destroySceneNode(sceneNode);
}

TEST_F(SelectionSetTests, GetSelectionNodesCenterEmpty)
{
    SelectionSet::getSingleton()->clear();
    auto center = SelectionSet::getSingleton()->getSelectionNodesCenter();

    EXPECT_EQ(center.x, 0.0f);
    EXPECT_EQ(center.y, 0.0f);
    EXPECT_EQ(center.z, 0.0f);
}

TEST_F(SelectionSetTests, GetSelectionNodesCenter)
{
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    auto sceneNode = Manager::getSingleton()->addSceneNode("test");
    auto sceneNode2 = Manager::getSingleton()->addSceneNode("test2");

    selectionSet->selectOne(sceneNode);
    selectionSet->append(sceneNode2);

    sceneNode2->translate(1.0,2.0,3.0);

    auto center = selectionSet->getSelectionNodesCenter();

    EXPECT_EQ(center.x, 0.5f);
    EXPECT_EQ(center.y, 1.0f);
    EXPECT_EQ(center.z, 1.5f);

    Manager::getSingleton()->destroySceneNode(sceneNode);
    Manager::getSingleton()->destroySceneNode(sceneNode2);
}

TEST_F(SelectionSetTests, IsEmpty)
{
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();

    EXPECT_TRUE(selectionSet->isEmpty());

    auto sceneNode = Manager::getSingleton()->addSceneNode("testIsEmpty");
    selectionSet->append(sceneNode);

    EXPECT_FALSE(selectionSet->isEmpty());

    selectionSet->clear();
    EXPECT_TRUE(selectionSet->isEmpty());

    Manager::getSingleton()->destroySceneNode(sceneNode);
}

TEST_F(SelectionSetTests, GetCount)
{
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();

    EXPECT_EQ(selectionSet->getCount(), 0);

    auto sceneNode = Manager::getSingleton()->addSceneNode("testGetCount");
    selectionSet->append(sceneNode);

    EXPECT_EQ(selectionSet->getCount(), 1);
    EXPECT_EQ(selectionSet->getNodesCount(), 1);
    EXPECT_EQ(selectionSet->getEntitiesCount(), 0);
    EXPECT_EQ(selectionSet->getSubEntitiesCount(), 0);

    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    // Add an entity to verify count sums across types
    createStandardOgreMaterials();
    auto cubeNode = PrimitiveObject::createCube("testGetCountCube");
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    selectionSet->append(entity);

    EXPECT_EQ(selectionSet->getCount(), 2);
    EXPECT_EQ(selectionSet->getNodesCount(), 1);
    EXPECT_EQ(selectionSet->getEntitiesCount(), 1);

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(sceneNode);
    Manager::getSingleton()->destroySceneNode(cubeNode);
}

TEST_F(SelectionSetTests, HasNodes)
{
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();

    EXPECT_FALSE(selectionSet->hasNodes());

    auto sceneNode = Manager::getSingleton()->addSceneNode("testHasNodes");
    selectionSet->append(sceneNode);

    EXPECT_TRUE(selectionSet->hasNodes());

    selectionSet->removeOne(sceneNode);
    EXPECT_FALSE(selectionSet->hasNodes());

    Manager::getSingleton()->destroySceneNode(sceneNode);
}

TEST_F(SelectionSetTests, EntitySelection)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();
    createStandardOgreMaterials();

    // Create a cube primitive to get an entity
    auto cubeNode = PrimitiveObject::createCube("testEntitySelCube");
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();

    // Test append entity
    selectionSet->append(entity);
    EXPECT_EQ(selectionSet->getEntitiesCount(), 1);
    EXPECT_TRUE(selectionSet->contains(entity));
    EXPECT_TRUE(selectionSet->hasEntities());

    // Append same entity again should not duplicate
    selectionSet->append(entity);
    EXPECT_EQ(selectionSet->getEntitiesCount(), 1);

    // Test removeOne entity
    bool removed = selectionSet->removeOne(entity);
    EXPECT_TRUE(removed);
    EXPECT_EQ(selectionSet->getEntitiesCount(), 0);
    EXPECT_FALSE(selectionSet->contains(entity));
    EXPECT_FALSE(selectionSet->hasEntities());

    // Test selectOne entity (clears everything, selects one entity)
    auto sceneNode = Manager::getSingleton()->addSceneNode("testEntitySelNode");
    selectionSet->append(sceneNode);
    EXPECT_EQ(selectionSet->getNodesCount(), 1);

    selectionSet->selectOne(entity);
    EXPECT_EQ(selectionSet->getEntitiesCount(), 1);
    EXPECT_EQ(selectionSet->getNodesCount(), 0);
    EXPECT_TRUE(selectionSet->contains(entity));

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(sceneNode);
    Manager::getSingleton()->destroySceneNode(cubeNode);
}

TEST_F(SelectionSetTests, EntityScaleRotationFactors)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();
    createStandardOgreMaterials();

    auto cubeNode = PrimitiveObject::createCube("testScaleRotCube");
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();

    // Default scale factor should be UNIT_SCALE
    Ogre::Vector3 defaultScale = selectionSet->getEntityScaleFactor(entity);
    EXPECT_EQ(defaultScale, Ogre::Vector3::UNIT_SCALE);

    // Set and get scale factor
    Ogre::Vector3 newScale(2.0f, 3.0f, 4.0f);
    selectionSet->setEntityScaleFactor(entity, newScale);
    Ogre::Vector3 retrievedScale = selectionSet->getEntityScaleFactor(entity);
    EXPECT_EQ(retrievedScale, newScale);

    // Update existing scale factor
    Ogre::Vector3 updatedScale(5.0f, 6.0f, 7.0f);
    selectionSet->setEntityScaleFactor(entity, updatedScale);
    retrievedScale = selectionSet->getEntityScaleFactor(entity);
    EXPECT_EQ(retrievedScale, updatedScale);

    // Default rotation should be ZERO
    Ogre::Vector3 defaultRotation = selectionSet->getEntityRotation(entity);
    EXPECT_EQ(defaultRotation, Ogre::Vector3::ZERO);

    // Set and get rotation
    Ogre::Vector3 newRotation(45.0f, 90.0f, 180.0f);
    selectionSet->setEntityRotation(entity, newRotation);
    Ogre::Vector3 retrievedRotation = selectionSet->getEntityRotation(entity);
    EXPECT_EQ(retrievedRotation, newRotation);

    // Update existing rotation
    Ogre::Vector3 updatedRotation(10.0f, 20.0f, 30.0f);
    selectionSet->setEntityRotation(entity, updatedRotation);
    retrievedRotation = selectionSet->getEntityRotation(entity);
    EXPECT_EQ(retrievedRotation, updatedRotation);

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(cubeNode);
}

TEST_F(SelectionSetTests, GetSelectionOrientationEmpty)
{
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();

    auto orientation = selectionSet->getSelectionOrientation();

    EXPECT_EQ(orientation.x, 0.0f);
    EXPECT_EQ(orientation.y, 0.0f);
    EXPECT_EQ(orientation.z, 0.0f);
}

TEST_F(SelectionSetTests, GetSelectionScaleEmpty)
{
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();

    auto scale = selectionSet->getSelectionScale();

    EXPECT_EQ(scale.x, 0.0f);
    EXPECT_EQ(scale.y, 0.0f);
    EXPECT_EQ(scale.z, 0.0f);
}

TEST_F(SelectionSetTests, SubEntitySelection)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();
    createStandardOgreMaterials();

    auto cubeNode = PrimitiveObject::createCube("testSubEntitySel");
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_GT(entity->getNumSubEntities(), 0u);
    Ogre::SubEntity* subEntity = entity->getSubEntity(0);

    // Append sub-entity
    selectionSet->append(subEntity);
    EXPECT_EQ(selectionSet->getSubEntitiesCount(), 1);
    EXPECT_TRUE(selectionSet->contains(subEntity));
    EXPECT_TRUE(selectionSet->hasSubEntities());

    // Append same sub-entity again should not duplicate
    selectionSet->append(subEntity);
    EXPECT_EQ(selectionSet->getSubEntitiesCount(), 1);

    // Remove sub-entity
    bool removed = selectionSet->removeOne(subEntity);
    EXPECT_TRUE(removed);
    EXPECT_EQ(selectionSet->getSubEntitiesCount(), 0);
    EXPECT_FALSE(selectionSet->contains(subEntity));
    EXPECT_FALSE(selectionSet->hasSubEntities());

    // selectOne sub-entity clears everything
    auto node = Manager::getSingleton()->addSceneNode("testSubEntitySelNode");
    selectionSet->append(node);
    selectionSet->append(entity);
    EXPECT_EQ(selectionSet->getNodesCount(), 1);
    EXPECT_EQ(selectionSet->getEntitiesCount(), 1);

    selectionSet->selectOne(subEntity);
    EXPECT_EQ(selectionSet->getSubEntitiesCount(), 1);
    EXPECT_EQ(selectionSet->getNodesCount(), 0);
    EXPECT_EQ(selectionSet->getEntitiesCount(), 0);

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(node);
    Manager::getSingleton()->destroySceneNode(cubeNode);
}

TEST_F(SelectionSetTests, IndexedAccessors)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();
    createStandardOgreMaterials();

    auto node1 = Manager::getSingleton()->addSceneNode("testIdx1");
    auto node2 = Manager::getSingleton()->addSceneNode("testIdx2");
    // addSceneNode auto-selects; clear and re-add in expected order
    selectionSet->clear();
    selectionSet->append(node1);
    selectionSet->append(node2);

    EXPECT_EQ(selectionSet->getSceneNode(0), node1);
    EXPECT_EQ(selectionSet->getSceneNode(1), node2);

    auto cubeNode = PrimitiveObject::createCube("testIdxCube");
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    selectionSet->append(entity);
    EXPECT_EQ(selectionSet->getEntity(0), entity);

    Ogre::SubEntity* subEntity = entity->getSubEntity(0);
    selectionSet->append(subEntity);
    EXPECT_EQ(selectionSet->getSubEntity(0), subEntity);

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(node1);
    Manager::getSingleton()->destroySceneNode(node2);
    Manager::getSingleton()->destroySceneNode(cubeNode);
}

TEST_F(SelectionSetTests, SelectionListGetters)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();
    createStandardOgreMaterials();

    auto node = Manager::getSingleton()->addSceneNode("testListNode");
    selectionSet->append(node);
    EXPECT_EQ(selectionSet->getNodesSelectionList().size(), 1);
    EXPECT_EQ(selectionSet->getNodesSelectionList().first(), node);

    auto cubeNode = PrimitiveObject::createCube("testListCube");
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    selectionSet->append(entity);
    EXPECT_EQ(selectionSet->getEntitiesSelectionList().size(), 1);
    EXPECT_EQ(selectionSet->getEntitiesSelectionList().first(), entity);

    Ogre::SubEntity* subEntity = entity->getSubEntity(0);
    selectionSet->append(subEntity);
    EXPECT_EQ(selectionSet->getSubEntitiesSelectionList().size(), 1);
    EXPECT_EQ(selectionSet->getSubEntitiesSelectionList().first(), subEntity);

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(node);
    Manager::getSingleton()->destroySceneNode(cubeNode);
}

TEST_F(SelectionSetTests, GetSelectionOrientationWithNode)
{
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();

    auto node = Manager::getSingleton()->addSceneNode("testOrientNode");
    node->setOrientation(Ogre::Quaternion::IDENTITY);
    selectionSet->append(node);

    auto orientation = selectionSet->getSelectionOrientation();
    // Identity quaternion should give (0,0,0) Euler angles
    EXPECT_NEAR(orientation.x, 0.0f, 0.01f);
    EXPECT_NEAR(orientation.y, 0.0f, 0.01f);
    EXPECT_NEAR(orientation.z, 0.0f, 0.01f);

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(node);
}

TEST_F(SelectionSetTests, GetSelectionOrientationWithEntity)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();
    createStandardOgreMaterials();

    auto cubeNode = PrimitiveObject::createCube("testOrientEntity");
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    // createCube auto-selects the node; clear so only the entity is selected
    selectionSet->clear();
    Ogre::Vector3 rotation(45.0f, 90.0f, 0.0f);
    selectionSet->setEntityRotation(entity, rotation);
    selectionSet->append(entity);

    auto orientation = selectionSet->getSelectionOrientation();
    EXPECT_NEAR(orientation.x, 45.0f, 0.01f);
    EXPECT_NEAR(orientation.y, 90.0f, 0.01f);
    EXPECT_NEAR(orientation.z, 0.0f, 0.01f);

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(cubeNode);
}

TEST_F(SelectionSetTests, GetSelectionScaleWithNode)
{
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();

    auto node = Manager::getSingleton()->addSceneNode("testScaleNode");
    node->setScale(2.0f, 3.0f, 4.0f);
    selectionSet->append(node);

    auto scale = selectionSet->getSelectionScale();
    EXPECT_EQ(scale.x, 2.0f);
    EXPECT_EQ(scale.y, 3.0f);
    EXPECT_EQ(scale.z, 4.0f);

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(node);
}

TEST_F(SelectionSetTests, GetSelectionScaleWithEntity)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();
    createStandardOgreMaterials();

    auto cubeNode = PrimitiveObject::createCube("testScaleEntity");
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    // createCube auto-selects the node; clear so only the entity is selected
    selectionSet->clear();
    Ogre::Vector3 scaleFactor(2.0f, 3.0f, 4.0f);
    selectionSet->setEntityScaleFactor(entity, scaleFactor);
    selectionSet->append(entity);

    auto scale = selectionSet->getSelectionScale();
    EXPECT_EQ(scale.x, 2.0f);
    EXPECT_EQ(scale.y, 3.0f);
    EXPECT_EQ(scale.z, 4.0f);

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(cubeNode);
}

TEST_F(SelectionSetTests, GetSelectionCenterWithEntity)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();
    createStandardOgreMaterials();

    auto cubeNode = PrimitiveObject::createCube("testCenterEntity");
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    selectionSet->append(entity);

    // Should return a non-zero center based on the entity's world bounding box
    auto center = selectionSet->getSelectionCenter();
    // Just verify it doesn't crash and returns some value
    EXPECT_TRUE(std::isfinite(center.x));
    EXPECT_TRUE(std::isfinite(center.y));
    EXPECT_TRUE(std::isfinite(center.z));

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(cubeNode);
}

TEST_F(SelectionSetTests, GetSelectionNodesCenterWithEntity)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();
    createStandardOgreMaterials();

    auto cubeNode = PrimitiveObject::createCube("testNodesCenterEntity");
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    cubeNode->setPosition(10.0f, 20.0f, 30.0f);
    selectionSet->append(entity);

    auto center = selectionSet->getSelectionNodesCenter();
    EXPECT_EQ(center.x, 10.0f);
    EXPECT_EQ(center.y, 20.0f);
    EXPECT_EQ(center.z, 30.0f);

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(cubeNode);
}

TEST_F(SelectionSetTests, SignalEmission)
{
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();

    QSignalSpy selectionSpy(selectionSet, &SelectionSet::selectionChanged);
    QSignalSpy nodeSpy(selectionSet, &SelectionSet::nodeSelectionChanged);

    auto node = Manager::getSingleton()->addSceneNode("testSignalNode");
    selectionSet->append(node);

    EXPECT_GE(selectionSpy.count(), 1);
    EXPECT_GE(nodeSpy.count(), 1);

    int prevCount = selectionSpy.count();
    selectionSet->removeOne(node);
    EXPECT_GT(selectionSpy.count(), prevCount);

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(node);
}

TEST_F(SelectionSetTests, RemoveNonExistent)
{
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();

    auto node = Manager::getSingleton()->addSceneNode("testRemoveNonExist");
    // addSceneNode auto-selects the node, so clear first
    selectionSet->clear();
    // Now removing a node that is NOT in the selection should return false
    bool removed = selectionSet->removeOne(node);
    EXPECT_FALSE(removed);
    EXPECT_EQ(selectionSet->getNodesCount(), 0);

    Manager::getSingleton()->destroySceneNode(node);
}

TEST_F(SelectionSetTests, GetResolvedEntitiesReturnsDirectEntitySelection)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();

    Ogre::SceneNode* node = createNodeWithEntity("resolved_direct", "resolved_direct_mesh");
    ASSERT_NE(node, nullptr);
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    selectionSet->append(entity);

    const QList<Ogre::Entity*> resolved = selectionSet->getResolvedEntities();
    ASSERT_EQ(resolved.size(), 1);
    EXPECT_EQ(resolved.first(), entity);

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(node);
}

TEST_F(SelectionSetTests, GetResolvedEntitiesResolvesSelectedNodesWithEntities)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();

    Ogre::SceneNode* node = createNodeWithEntity("resolved_node", "resolved_node_mesh");
    ASSERT_NE(node, nullptr);
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    selectionSet->append(node);

    const QList<Ogre::Entity*> resolved = selectionSet->getResolvedEntities();
    ASSERT_EQ(resolved.size(), 1);
    EXPECT_EQ(resolved.first(), entity);

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(node);
}

TEST_F(SelectionSetTests, GetResolvedEntitiesSkipsNodesWithoutAttachedEntities)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();

    Ogre::SceneNode* emptyNode = Manager::getSingleton()->addSceneNode("resolved_empty");
    ASSERT_NE(emptyNode, nullptr);
    Ogre::SceneNode* entityNode = createNodeWithEntity("resolved_with_entity", "resolved_with_entity_mesh");
    ASSERT_NE(entityNode, nullptr);
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);

    selectionSet->append(emptyNode);
    selectionSet->append(entityNode);

    const QList<Ogre::Entity*> resolved = selectionSet->getResolvedEntities();
    ASSERT_EQ(resolved.size(), 1);
    EXPECT_EQ(resolved.first(), entity);

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(emptyNode);
    Manager::getSingleton()->destroySceneNode(entityNode);
}

TEST_F(SelectionSetTests, GetSelectionScaleAveragesMultipleNodes)
{
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();

    Ogre::SceneNode* node1 = Manager::getSingleton()->addSceneNode("scale_avg_1");
    Ogre::SceneNode* node2 = Manager::getSingleton()->addSceneNode("scale_avg_2");
    ASSERT_NE(node1, nullptr);
    ASSERT_NE(node2, nullptr);

    node1->setScale(2.0f, 4.0f, 6.0f);
    node2->setScale(4.0f, 6.0f, 8.0f);

    selectionSet->append(node1);
    selectionSet->append(node2);

    const Ogre::Vector3 scale = selectionSet->getSelectionScale();
    EXPECT_EQ(scale, Ogre::Vector3(3.0f, 5.0f, 7.0f));

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(node1);
    Manager::getSingleton()->destroySceneNode(node2);
}

TEST_F(SelectionSetTests, GetSelectionScaleAveragesMultipleEntitiesUsingStoredFactors)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();

    Ogre::SceneNode* node1 = createNodeWithEntity("entity_scale_1", "entity_scale_mesh_1");
    Ogre::SceneNode* node2 = createNodeWithEntity("entity_scale_2", "entity_scale_mesh_2");
    ASSERT_NE(node1, nullptr);
    ASSERT_NE(node2, nullptr);

    QList<Ogre::Entity*> entities = Manager::getSingleton()->getEntities();
    ASSERT_GE(entities.size(), 2);
    Ogre::Entity* entity1 = entities.at(entities.size() - 2);
    Ogre::Entity* entity2 = entities.at(entities.size() - 1);

    selectionSet->setEntityScaleFactor(entity1, Ogre::Vector3(2.0f, 3.0f, 4.0f));
    selectionSet->setEntityScaleFactor(entity2, Ogre::Vector3(4.0f, 5.0f, 6.0f));
    selectionSet->append(entity1);
    selectionSet->append(entity2);

    const Ogre::Vector3 scale = selectionSet->getSelectionScale();
    EXPECT_EQ(scale, Ogre::Vector3(3.0f, 4.0f, 5.0f));

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(node1);
    Manager::getSingleton()->destroySceneNode(node2);
}

TEST_F(SelectionSetTests, GetSelectionOrientationAveragesMultipleEntitiesUsingStoredRotations)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();

    Ogre::SceneNode* node1 = createNodeWithEntity("entity_rot_1", "entity_rot_mesh_1");
    Ogre::SceneNode* node2 = createNodeWithEntity("entity_rot_2", "entity_rot_mesh_2");
    ASSERT_NE(node1, nullptr);
    ASSERT_NE(node2, nullptr);

    QList<Ogre::Entity*> entities = Manager::getSingleton()->getEntities();
    ASSERT_GE(entities.size(), 2);
    Ogre::Entity* entity1 = entities.at(entities.size() - 2);
    Ogre::Entity* entity2 = entities.at(entities.size() - 1);

    selectionSet->setEntityRotation(entity1, Ogre::Vector3(10.0f, 20.0f, 30.0f));
    selectionSet->setEntityRotation(entity2, Ogre::Vector3(30.0f, 40.0f, 50.0f));
    selectionSet->append(entity1);
    selectionSet->append(entity2);

    const Ogre::Vector3 orientation = selectionSet->getSelectionOrientation();
    EXPECT_EQ(orientation, Ogre::Vector3(20.0f, 30.0f, 40.0f));

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(node1);
    Manager::getSingleton()->destroySceneNode(node2);
}

TEST_F(SelectionSetTests, GetSelectionCenterWithSubEntityUsesParentBoundingBox)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();

    Ogre::SceneNode* node = createNodeWithEntity("subentity_center", "subentity_center_mesh");
    ASSERT_NE(node, nullptr);
    node->setPosition(Ogre::Vector3(8.0f, 9.0f, 10.0f));

    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);
    ASSERT_GT(entity->getNumSubEntities(), 0u);
    Ogre::SubEntity* subEntity = entity->getSubEntity(0);
    ASSERT_NE(subEntity, nullptr);

    selectionSet->append(subEntity);

    const Ogre::Vector3 center = selectionSet->getSelectionCenter();
    EXPECT_TRUE(std::isfinite(center.x));
    EXPECT_TRUE(std::isfinite(center.y));
    EXPECT_TRUE(std::isfinite(center.z));

    const Ogre::Vector3 nodesCenter = selectionSet->getSelectionNodesCenter();
    EXPECT_EQ(nodesCenter, Ogre::Vector3(8.0f, 9.0f, 10.0f));

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(node);
}

TEST_F(SelectionSetTests, RemoveNonExistentEntityAndSubEntityReturnFalse)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();

    Ogre::SceneNode* node = createNodeWithEntity("remove_missing", "remove_missing_mesh");
    ASSERT_NE(node, nullptr);
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_NE(entity, nullptr);
    Ogre::SubEntity* subEntity = entity->getSubEntity(0);
    ASSERT_NE(subEntity, nullptr);

    EXPECT_FALSE(selectionSet->removeOne(entity));
    EXPECT_FALSE(selectionSet->removeOne(subEntity));

    Manager::getSingleton()->destroySceneNode(node);
}

// NOTE: GetSelectionCenterWithSubEntity and all subsequent tests were removed
// because they crash in CI (PrimitiveObject::createCube requires GL context).
