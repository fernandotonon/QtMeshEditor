#include <gtest/gtest.h>
#include "Manager.h"
#include <QMap>
#include "SelectionSet.h"
#include "PrimitiveObject.h"
#include <OgreException.h>
#include <OgreMaterialManager.h>
#include <OgreResourceGroupManager.h>

// Helper function to create required OGRE materials for entity tests
static void createOGREMaterials()
{
    Ogre::MaterialPtr baseWhiteMat = Ogre::MaterialManager::getSingleton().getByName(
        "BaseWhiteNoLighting", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    if (!baseWhiteMat)
    {
        baseWhiteMat = Ogre::MaterialManager::getSingleton().create(
            "BaseWhiteNoLighting", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        baseWhiteMat->getTechnique(0)->getPass(0)->setDiffuse(1, 1, 1, 1);
        baseWhiteMat->getTechnique(0)->getPass(0)->setAmbient(1, 1, 1);
        baseWhiteMat->getTechnique(0)->getPass(0)->setSelfIllumination(1, 1, 1);
        baseWhiteMat->getTechnique(0)->setLightingEnabled(false);
    }

    Ogre::MaterialPtr baseWhiteMat2 = Ogre::MaterialManager::getSingleton().getByName(
        "BaseWhite", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    if (!baseWhiteMat2)
    {
        baseWhiteMat2 = Ogre::MaterialManager::getSingleton().create(
            "BaseWhite", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        baseWhiteMat2->getTechnique(0)->getPass(0)->setDiffuse(1, 1, 1, 1);
        baseWhiteMat2->getTechnique(0)->getPass(0)->setAmbient(1, 1, 1);
    }
}

TEST(SelectionSetTests, AppendSceneNode)
{
    try {
        Manager::getSingleton();
    } catch (const Ogre::Exception& e) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed (" << e.getFullDescription() << ")";
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
    try {
        Manager::getSingleton();
    } catch (const Ogre::Exception& e) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed (" << e.getFullDescription() << ")";
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
    try {
        Manager::getSingleton();
    } catch (const Ogre::Exception& e) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed (" << e.getFullDescription() << ")";
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
    try {
        Manager::getSingleton();
    } catch (const Ogre::Exception& e) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed (" << e.getFullDescription() << ")";
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
    try {
        Manager::getSingleton();
    } catch (const Ogre::Exception& e) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed (" << e.getFullDescription() << ")";
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
    try {
        Manager::getSingleton();
    } catch (const Ogre::Exception& e) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed (" << e.getFullDescription() << ")";
    }
    SelectionSet::getSingleton()->clear();
    auto center = SelectionSet::getSingleton()->getSelectionNodesCenter();

    EXPECT_EQ(center.x, 0.0f);
    EXPECT_EQ(center.y, 0.0f);
    EXPECT_EQ(center.z, 0.0f);
}

TEST(SelectionSetTests, GetSelectionNodesCenter)
{
    try {
        Manager::getSingleton();
    } catch (const Ogre::Exception& e) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed (" << e.getFullDescription() << ")";
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
    try {
        Manager::getSingleton();
    } catch (const Ogre::Exception& e) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed (" << e.getFullDescription() << ")";
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
    try {
        Manager::getSingleton();
    } catch (const Ogre::Exception& e) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed (" << e.getFullDescription() << ")";
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

    // Add an entity to verify count sums across types
    createOGREMaterials();
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
    try {
        Manager::getSingleton();
    } catch (const Ogre::Exception& e) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed (" << e.getFullDescription() << ")";
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
    try {
        Manager::getSingleton();
    } catch (const Ogre::Exception& e) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed (" << e.getFullDescription() << ")";
    }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();
    createOGREMaterials();

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
    try {
        Manager::getSingleton();
    } catch (const Ogre::Exception& e) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed (" << e.getFullDescription() << ")";
    }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();
    createOGREMaterials();

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
    try {
        Manager::getSingleton();
    } catch (const Ogre::Exception& e) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed (" << e.getFullDescription() << ")";
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
    try {
        Manager::getSingleton();
    } catch (const Ogre::Exception& e) {
        GTEST_SKIP() << "Skipping: Ogre initialization failed (" << e.getFullDescription() << ")";
    }
    SelectionSet* selectionSet = SelectionSet::getSingleton();
    selectionSet->clear();

    auto scale = selectionSet->getSelectionScale();

    EXPECT_EQ(scale.x, 0.0f);
    EXPECT_EQ(scale.y, 0.0f);
    EXPECT_EQ(scale.z, 0.0f);
}
