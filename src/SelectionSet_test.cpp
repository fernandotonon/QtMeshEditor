#include <gtest/gtest.h>
#include <QSignalSpy>
#include "Manager.h"
#include <QMap>
#include "SelectionSet.h"
#include "PrimitiveObject.h"
#include "TestHelpers.h"

TEST(SelectionSetTests, AppendSceneNode)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
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

TEST(SelectionSetTests, RemoveSceneNode)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    auto sceneNode = Manager::getSingleton()->addSceneNode("test");

    selectionSet->append(sceneNode);
    bool removed = selectionSet->removeOne(sceneNode);

    EXPECT_TRUE(removed);
    EXPECT_EQ(selectionSet->getNodesCount(), 0);
    EXPECT_FALSE(selectionSet->contains(sceneNode));

    Manager::getSingleton()->destroySceneNode(sceneNode);
    SelectionSet::kill();
}

TEST(SelectionSetTests, SelectSceneNode)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    auto sceneNode = Manager::getSingleton()->addSceneNode("test");

    selectionSet->selectOne(sceneNode);

    EXPECT_EQ(selectionSet->getNodesCount(), 1);
    EXPECT_TRUE(selectionSet->contains(sceneNode));

    Manager::getSingleton()->destroySceneNode(sceneNode);
}

TEST(SelectionSetTests, Clear)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    auto sceneNode = Manager::getSingleton()->addSceneNode("test");

    selectionSet->append(sceneNode);
    selectionSet->clear();

    EXPECT_EQ(selectionSet->getNodesCount(), 0);
    EXPECT_FALSE(selectionSet->contains(sceneNode));

    Manager::getSingleton()->destroySceneNode(sceneNode);
}

TEST(SelectionSetTests, ClearList)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    auto sceneNode = Manager::getSingleton()->addSceneNode("test");

    selectionSet->append(sceneNode);
    selectionSet->clearList();

    EXPECT_EQ(selectionSet->getNodesCount(), 0);
    EXPECT_FALSE(selectionSet->contains(sceneNode));
    Manager::getSingleton()->destroySceneNode(sceneNode);
}

TEST(SelectionSetTests, GetSelectionNodesCenterEmpty)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
    SelectionSet::getSingleton()->clear();
    auto center = SelectionSet::getSingleton()->getSelectionNodesCenter();

    EXPECT_EQ(center.x, 0.0f);
    EXPECT_EQ(center.y, 0.0f);
    EXPECT_EQ(center.z, 0.0f);
}

TEST(SelectionSetTests, GetSelectionNodesCenter)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
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

TEST(SelectionSetTests, IsEmpty)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
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

TEST(SelectionSetTests, GetCount)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
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

TEST(SelectionSetTests, HasNodes)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
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

TEST(SelectionSetTests, EntitySelection)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
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

TEST(SelectionSetTests, EntityScaleRotationFactors)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
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

TEST(SelectionSetTests, GetSelectionOrientationEmpty)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();

    auto orientation = selectionSet->getSelectionOrientation();

    EXPECT_EQ(orientation.x, 0.0f);
    EXPECT_EQ(orientation.y, 0.0f);
    EXPECT_EQ(orientation.z, 0.0f);
}

TEST(SelectionSetTests, GetSelectionScaleEmpty)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();

    auto scale = selectionSet->getSelectionScale();

    EXPECT_EQ(scale.x, 0.0f);
    EXPECT_EQ(scale.y, 0.0f);
    EXPECT_EQ(scale.z, 0.0f);
}

TEST(SelectionSetTests, SubEntitySelection)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
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

TEST(SelectionSetTests, IndexedAccessors)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
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

TEST(SelectionSetTests, SelectionListGetters)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
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

TEST(SelectionSetTests, GetSelectionOrientationWithNode)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
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

TEST(SelectionSetTests, GetSelectionOrientationWithEntity)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
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

TEST(SelectionSetTests, GetSelectionScaleWithNode)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
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

TEST(SelectionSetTests, GetSelectionScaleWithEntity)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
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

TEST(SelectionSetTests, GetSelectionCenterWithEntity)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
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

TEST(SelectionSetTests, GetSelectionNodesCenterWithEntity)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
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

TEST(SelectionSetTests, SignalEmission)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
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

TEST(SelectionSetTests, RemoveNonExistent)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
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

// ==========================================================================
// NEW: SubEntity branch coverage for getSelectionCenter
// ==========================================================================

TEST(SelectionSetTests, GetSelectionCenterWithSubEntity)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();
    createStandardOgreMaterials();

    auto cubeNode = PrimitiveObject::createCube("testCenterSubEntity");
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_GT(entity->getNumSubEntities(), 0u);
    Ogre::SubEntity* subEntity = entity->getSubEntity(0);

    // Only select subEntity (no nodes, no entities)
    selectionSet->clear();
    selectionSet->append(subEntity);

    auto center = selectionSet->getSelectionCenter();
    // Should hit the hasSubEntities() branch and return finite values
    EXPECT_TRUE(std::isfinite(center.x));
    EXPECT_TRUE(std::isfinite(center.y));
    EXPECT_TRUE(std::isfinite(center.z));

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(cubeNode);
}

// ==========================================================================
// NEW: SubEntity branch coverage for getSelectionNodesCenter
// ==========================================================================

TEST(SelectionSetTests, GetSelectionNodesCenterWithSubEntity)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();
    createStandardOgreMaterials();

    auto cubeNode = PrimitiveObject::createCube("testNodesCenterSubEntity");
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_GT(entity->getNumSubEntities(), 0u);
    Ogre::SubEntity* subEntity = entity->getSubEntity(0);

    cubeNode->setPosition(5.0f, 10.0f, 15.0f);

    // Only select subEntity
    selectionSet->clear();
    selectionSet->append(subEntity);

    auto center = selectionSet->getSelectionNodesCenter();
    // Should go through hasSubEntities() branch and use parent's parent node position
    EXPECT_EQ(center.x, 5.0f);
    EXPECT_EQ(center.y, 10.0f);
    EXPECT_EQ(center.z, 15.0f);

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(cubeNode);
}

// ==========================================================================
// NEW: getResolvedEntities branches
// ==========================================================================

TEST(SelectionSetTests, GetResolvedEntitiesWithEntitySelection)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();
    createStandardOgreMaterials();

    auto cubeNode = PrimitiveObject::createCube("testResolvedEntity");
    ASSERT_FALSE(Manager::getSingleton()->getEntities().isEmpty());
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();

    // Select entity directly — hasEntities() branch returns the list
    selectionSet->clear();
    selectionSet->append(entity);

    auto resolved = selectionSet->getResolvedEntities();
    EXPECT_EQ(resolved.size(), 1);
    EXPECT_EQ(resolved.first(), entity);

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(cubeNode);
}

TEST(SelectionSetTests, GetResolvedEntitiesWithNodeSelection)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();
    createStandardOgreMaterials();

    auto cubeNode = PrimitiveObject::createCube("testResolvedNode");
    // Select node (not entity) — hasNodes() branch resolves via sceneMgr
    selectionSet->clear();
    selectionSet->append(cubeNode);

    auto resolved = selectionSet->getResolvedEntities();
    // The entity name matches the scene node name, so it should be resolved
    EXPECT_EQ(resolved.size(), 1);

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(cubeNode);
}

TEST(SelectionSetTests, GetResolvedEntitiesEmpty)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();

    auto resolved = selectionSet->getResolvedEntities();
    EXPECT_TRUE(resolved.isEmpty());
}

TEST(SelectionSetTests, GetResolvedEntitiesNodeWithoutEntity)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();

    // Add a plain scene node (no entity attached)
    auto node = Manager::getSingleton()->addSceneNode("testResolvedNoEntity");
    selectionSet->clear();
    selectionSet->append(node);

    auto resolved = selectionSet->getResolvedEntities();
    // Node exists but has no entity with the same name, so resolved is empty
    EXPECT_TRUE(resolved.isEmpty());

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(node);
}

// ==========================================================================
// NEW: hideBoundingBox branch coverage (tested via removeOne)
// ==========================================================================

TEST(SelectionSetTests, HideBoundingBoxEntityContainsNode)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();
    createStandardOgreMaterials();

    auto cubeNode = PrimitiveObject::createCube("testHideBboxEntity");
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();

    // Select both the node and the entity
    selectionSet->clear();
    selectionSet->append(cubeNode);
    selectionSet->append(entity);
    EXPECT_EQ(selectionSet->getNodesCount(), 1);
    EXPECT_EQ(selectionSet->getEntitiesCount(), 1);

    // Remove the node — hideBoundingBox(cubeNode) should return early
    // because entity->getParentSceneNode() == cubeNode (entity still selected)
    selectionSet->removeOne(cubeNode);
    EXPECT_EQ(selectionSet->getNodesCount(), 0);
    EXPECT_EQ(selectionSet->getEntitiesCount(), 1);
    // Bounding box should still be shown because entity is still selected
    EXPECT_TRUE(cubeNode->getShowBoundingBox());

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(cubeNode);
}

TEST(SelectionSetTests, HideBoundingBoxSubEntityContainsNode)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();
    createStandardOgreMaterials();

    auto cubeNode = PrimitiveObject::createCube("testHideBboxSubEnt");
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    ASSERT_GT(entity->getNumSubEntities(), 0u);
    Ogre::SubEntity* subEntity = entity->getSubEntity(0);

    // Select both the node and the subEntity
    selectionSet->clear();
    selectionSet->append(cubeNode);
    selectionSet->append(subEntity);

    // Remove the node — hideBoundingBox(cubeNode) should return early
    // because subEntity's parent's parent node == cubeNode
    selectionSet->removeOne(cubeNode);
    EXPECT_EQ(selectionSet->getNodesCount(), 0);
    EXPECT_EQ(selectionSet->getSubEntitiesCount(), 1);
    EXPECT_TRUE(cubeNode->getShowBoundingBox());

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(cubeNode);
}

TEST(SelectionSetTests, HideBoundingBoxNotInSelection)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();
    createStandardOgreMaterials();

    auto cubeNode = PrimitiveObject::createCube("testHideBboxNone");
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();

    // Select only the entity (not the node directly)
    selectionSet->clear();
    selectionSet->append(entity);

    // Remove the entity — hideBoundingBox(cubeNode) should hide bbox
    // because cubeNode is NOT in mNodesSelected, no other entity/subEntity points to it
    selectionSet->removeOne(entity);
    EXPECT_EQ(selectionSet->getEntitiesCount(), 0);
    EXPECT_FALSE(cubeNode->getShowBoundingBox());

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(cubeNode);
}

// ==========================================================================
// NEW: hideAllBoundingBox with mixed selection types
// ==========================================================================

TEST(SelectionSetTests, HideAllBoundingBoxMixed)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();
    createStandardOgreMaterials();

    auto cubeNode1 = PrimitiveObject::createCube("testHideAllMixed1");
    Ogre::Entity* entity1 = Manager::getSingleton()->getEntities().last();

    auto cubeNode2 = PrimitiveObject::createCube("testHideAllMixed2");
    Ogre::Entity* entity2 = Manager::getSingleton()->getEntities().last();
    ASSERT_GT(entity2->getNumSubEntities(), 0u);
    Ogre::SubEntity* subEntity2 = entity2->getSubEntity(0);

    auto plainNode = Manager::getSingleton()->addSceneNode("testHideAllMixedNode");

    // Build mixed selection: node + entity + subEntity
    selectionSet->clear();
    selectionSet->append(plainNode);
    selectionSet->append(entity1);
    selectionSet->append(subEntity2);
    EXPECT_EQ(selectionSet->getCount(), 3);

    // All should have bounding boxes shown
    EXPECT_TRUE(plainNode->getShowBoundingBox());
    EXPECT_TRUE(cubeNode1->getShowBoundingBox());
    EXPECT_TRUE(cubeNode2->getShowBoundingBox());

    // clear() calls hideAllBoundingBox then clears all lists
    selectionSet->clear();
    EXPECT_FALSE(plainNode->getShowBoundingBox());
    EXPECT_FALSE(cubeNode1->getShowBoundingBox());
    EXPECT_FALSE(cubeNode2->getShowBoundingBox());

    Manager::getSingleton()->destroySceneNode(plainNode);
    Manager::getSingleton()->destroySceneNode(cubeNode1);
    Manager::getSingleton()->destroySceneNode(cubeNode2);
}

// ==========================================================================
// NEW: getSelectionCenter with multiple nodes (average)
// ==========================================================================

TEST(SelectionSetTests, GetSelectionCenterMultipleNodes)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();

    auto node1 = Manager::getSingleton()->addSceneNode("testCenterMulti1");
    auto node2 = Manager::getSingleton()->addSceneNode("testCenterMulti2");
    auto node3 = Manager::getSingleton()->addSceneNode("testCenterMulti3");

    node1->setPosition(0.0f, 0.0f, 0.0f);
    node2->setPosition(3.0f, 6.0f, 9.0f);
    node3->setPosition(6.0f, 12.0f, 18.0f);

    selectionSet->clear();
    selectionSet->append(node1);
    selectionSet->append(node2);
    selectionSet->append(node3);

    auto center = selectionSet->getSelectionCenter();
    EXPECT_FLOAT_EQ(center.x, 3.0f);
    EXPECT_FLOAT_EQ(center.y, 6.0f);
    EXPECT_FLOAT_EQ(center.z, 9.0f);

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(node1);
    Manager::getSingleton()->destroySceneNode(node2);
    Manager::getSingleton()->destroySceneNode(node3);
}

// ==========================================================================
// NEW: getSelectionCenter with empty selection
// ==========================================================================

TEST(SelectionSetTests, GetSelectionCenterEmpty)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();

    auto center = selectionSet->getSelectionCenter();
    EXPECT_EQ(center.x, 0.0f);
    EXPECT_EQ(center.y, 0.0f);
    EXPECT_EQ(center.z, 0.0f);
}

// ==========================================================================
// NEW: selectOne cross-type clearing
// ==========================================================================

TEST(SelectionSetTests, SelectOneNodeClearsEntitiesAndSubEntities)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();
    createStandardOgreMaterials();

    auto cubeNode = PrimitiveObject::createCube("testSelectOneClear");
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    Ogre::SubEntity* subEntity = entity->getSubEntity(0);
    auto plainNode = Manager::getSingleton()->addSceneNode("testSelectOneClearNode");

    // Add entity and subEntity to selection
    selectionSet->clear();
    selectionSet->append(entity);
    selectionSet->append(subEntity);
    EXPECT_EQ(selectionSet->getEntitiesCount(), 1);
    EXPECT_EQ(selectionSet->getSubEntitiesCount(), 1);

    // selectOne(node) should clear entities and subEntities
    selectionSet->selectOne(plainNode);
    EXPECT_EQ(selectionSet->getNodesCount(), 1);
    EXPECT_EQ(selectionSet->getEntitiesCount(), 0);
    EXPECT_EQ(selectionSet->getSubEntitiesCount(), 0);

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(plainNode);
    Manager::getSingleton()->destroySceneNode(cubeNode);
}

TEST(SelectionSetTests, SelectOneSubEntityClearsNodesAndEntities)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();
    createStandardOgreMaterials();

    auto cubeNode = PrimitiveObject::createCube("testSelectOneSubEnt");
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    Ogre::SubEntity* subEntity = entity->getSubEntity(0);
    auto plainNode = Manager::getSingleton()->addSceneNode("testSelectOneSubEntNode");

    // Add node and entity to selection
    selectionSet->clear();
    selectionSet->append(plainNode);
    selectionSet->append(entity);
    EXPECT_EQ(selectionSet->getNodesCount(), 1);
    EXPECT_EQ(selectionSet->getEntitiesCount(), 1);

    // selectOne(subEntity) should clear nodes and entities
    selectionSet->selectOne(subEntity);
    EXPECT_EQ(selectionSet->getNodesCount(), 0);
    EXPECT_EQ(selectionSet->getEntitiesCount(), 0);
    EXPECT_EQ(selectionSet->getSubEntitiesCount(), 1);

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(plainNode);
    Manager::getSingleton()->destroySceneNode(cubeNode);
}

// ==========================================================================
// NEW: Signal emission for entity and subEntity selection changes
// ==========================================================================

TEST(SelectionSetTests, EntitySignalEmission)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();
    createStandardOgreMaterials();

    auto cubeNode = PrimitiveObject::createCube("testEntitySignal");
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();

    QSignalSpy entitySpy(selectionSet, &SelectionSet::entitySelectionChanged);
    QSignalSpy selectionSpy(selectionSet, &SelectionSet::selectionChanged);

    selectionSet->clear();
    selectionSet->append(entity);
    EXPECT_GE(entitySpy.count(), 1);
    EXPECT_GE(selectionSpy.count(), 1);

    int prevEntityCount = entitySpy.count();
    selectionSet->removeOne(entity);
    EXPECT_GT(entitySpy.count(), prevEntityCount);

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(cubeNode);
}

TEST(SelectionSetTests, SubEntitySignalEmission)
{
    if (!tryInitOgre()) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed";
    }
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();
    createStandardOgreMaterials();

    auto cubeNode = PrimitiveObject::createCube("testSubEntitySignal");
    Ogre::Entity* entity = Manager::getSingleton()->getEntities().last();
    Ogre::SubEntity* subEntity = entity->getSubEntity(0);

    QSignalSpy subEntitySpy(selectionSet, &SelectionSet::subEntitySelectionChanged);
    QSignalSpy selectionSpy(selectionSet, &SelectionSet::selectionChanged);

    selectionSet->clear();
    selectionSet->append(subEntity);
    EXPECT_GE(subEntitySpy.count(), 1);
    EXPECT_GE(selectionSpy.count(), 1);

    int prevSubEntityCount = subEntitySpy.count();
    selectionSet->removeOne(subEntity);
    EXPECT_GT(subEntitySpy.count(), prevSubEntityCount);

    selectionSet->clear();
    Manager::getSingleton()->destroySceneNode(cubeNode);
}
