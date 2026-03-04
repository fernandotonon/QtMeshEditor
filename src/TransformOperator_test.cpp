#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <Ogre.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include "TransformOperator.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "GlobalDefinitions.h"
#include <QSignalSpy>
#include "TestHelpers.h"

// Helper function to create required OGRE materials for tests
static void createOGREMaterials()
{
    ensureMaterialManagerInitialised();
    Ogre::MaterialPtr guiMat = Ogre::MaterialManager::getSingleton().getByName(GUI_MATERIAL_NAME, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    if (!guiMat)
    {
        guiMat = Ogre::MaterialManager::getSingleton().create(GUI_MATERIAL_NAME, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        guiMat->getTechnique(0)->setLightingEnabled(false);
        guiMat->getTechnique(0)->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
        guiMat->getTechnique(0)->setDepthCheckEnabled(false);
    }
}

// Test fixture for TransformOperator tests that require Manager
class TransformOperatorTestFixture : public ::testing::Test {
protected:
    QApplication* app = nullptr;

    void SetUp() override {
        TransformOperator::kill();
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
        }
        createOGREMaterials();
    }

    void TearDown() override {
        if (app) {
            app->processEvents();
        }
    }
};

// Test if getSingleton returns a valid pointer
TEST_F(TransformOperatorTestFixture, GetSingleton) {
    TransformOperator* instance = TransformOperator::getSingleton();
    EXPECT_NE(instance, nullptr);
}

// Test if getSingleton always returns the same instance
TEST_F(TransformOperatorTestFixture, SingletonInstance) {
    TransformOperator* instance1 = TransformOperator::getSingleton();
    TransformOperator* instance2 = TransformOperator::getSingleton();
    EXPECT_EQ(instance1, instance2);
}

// Test if setTransformState sets the state correctly
TEST_F(TransformOperatorTestFixture, SetSelectionBoxColour) {
    TransformOperator* instance = TransformOperator::getSingleton();
    instance->setSelectionBoxColour(Ogre::ColourValue(0.5, 0.5, 0.5, 1.0));
    EXPECT_EQ(instance->getSelectionBoxColour(), Ogre::ColourValue(0.5, 0.5, 0.5, 1.0));
}

// Swap test doesn't need Manager, so it can be standalone
TEST(TransformOperatorTest, Swap) {
    int x = 1;
    int y = 2;
    TransformOperator::swap(x, y);
    EXPECT_EQ(x, 2);
    EXPECT_EQ(y, 1);
}

TEST_F(TransformOperatorTestFixture, RayFromScreenPoint) {
    TransformOperator* instance = TransformOperator::getSingleton();
    Ogre::Ray ray = instance->rayFromScreenPoint(QPoint(0, 0));
    EXPECT_EQ(ray.getOrigin(), Ogre::Vector3::ZERO);
    EXPECT_EQ(ray.getDirection(), Ogre::Vector3::UNIT_Z);
}

// ---------- New tests ----------

// Test onTransformStateChange with TS_SELECT: gizmos should be hidden, no crash
TEST_F(TransformOperatorTestFixture, OnTransformStateChange_Select) {
    TransformOperator* instance = TransformOperator::getSingleton();
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_SELECT));
}

// Test onTransformStateChange with TS_TRANSLATE: no crash without selection
TEST_F(TransformOperatorTestFixture, OnTransformStateChange_Translate) {
    TransformOperator* instance = TransformOperator::getSingleton();
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_TRANSLATE));
}

// Test onTransformStateChange with TS_ROTATE: no crash without selection
TEST_F(TransformOperatorTestFixture, OnTransformStateChange_Rotate) {
    TransformOperator* instance = TransformOperator::getSingleton();
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_ROTATE));
}

// Test onTransformStateChange with TS_NONE: default/reset state, no crash
TEST_F(TransformOperatorTestFixture, OnTransformStateChange_None) {
    TransformOperator* instance = TransformOperator::getSingleton();
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_NONE));
}

// Test removeSelected when nothing is selected: should not crash
TEST_F(TransformOperatorTestFixture, RemoveSelectedEmpty) {
    TransformOperator* instance = TransformOperator::getSingleton();
    ASSERT_TRUE(SelectionSet::getSingleton()->isEmpty());
    EXPECT_NO_THROW(instance->removeSelected());
}

// Test setSelectedPosition when nothing is selected: should not crash or modify anything
TEST_F(TransformOperatorTestFixture, SetSelectedPositionNoSelection) {
    TransformOperator* instance = TransformOperator::getSingleton();
    ASSERT_TRUE(SelectionSet::getSingleton()->isEmpty());
    EXPECT_NO_THROW(instance->setSelectedPosition(Ogre::Vector3(10.0f, 20.0f, 30.0f)));
}

// Test translateSelected when nothing is selected: should not crash
TEST_F(TransformOperatorTestFixture, TranslateSelectedNoSelection) {
    TransformOperator* instance = TransformOperator::getSingleton();
    ASSERT_TRUE(SelectionSet::getSingleton()->isEmpty());
    EXPECT_NO_THROW(instance->translateSelected(Ogre::Vector3(5.0f, 5.0f, 5.0f)));
}

// Test setSelectedScale when nothing is selected: should not crash
TEST_F(TransformOperatorTestFixture, SetSelectedScaleNoSelection) {
    TransformOperator* instance = TransformOperator::getSingleton();
    ASSERT_TRUE(SelectionSet::getSingleton()->isEmpty());
    EXPECT_NO_THROW(instance->setSelectedScale(Ogre::Vector3(2.0f, 2.0f, 2.0f)));
}

// Test setSelectedOrientation when nothing is selected: should not crash
TEST_F(TransformOperatorTestFixture, SetSelectedOrientationNoSelection) {
    TransformOperator* instance = TransformOperator::getSingleton();
    ASSERT_TRUE(SelectionSet::getSingleton()->isEmpty());
    EXPECT_NO_THROW(instance->setSelectedOrientation(Ogre::Vector3(45.0f, 90.0f, 0.0f)));
}

// Test scaleSelected when nothing is selected: should not crash
TEST_F(TransformOperatorTestFixture, ScaleSelectedNoSelection) {
    TransformOperator* instance = TransformOperator::getSingleton();
    ASSERT_TRUE(SelectionSet::getSingleton()->isEmpty());
    EXPECT_NO_THROW(instance->scaleSelected(Ogre::Vector3(1.5f, 1.5f, 1.5f)));
}

// Test rotateSelected(Quaternion) when nothing is selected: should not crash
TEST_F(TransformOperatorTestFixture, RotateSelectedNoSelection) {
    TransformOperator* instance = TransformOperator::getSingleton();
    ASSERT_TRUE(SelectionSet::getSingleton()->isEmpty());
    Ogre::Quaternion rotation(Ogre::Degree(45), Ogre::Vector3::UNIT_Y);
    EXPECT_NO_THROW(instance->rotateSelected(rotation));
}

// Test rotateSelected(Vector3) when nothing is selected: should not crash
TEST_F(TransformOperatorTestFixture, RotateSelectedVectorNoSelection) {
    TransformOperator* instance = TransformOperator::getSingleton();
    ASSERT_TRUE(SelectionSet::getSingleton()->isEmpty());
    EXPECT_NO_THROW(instance->rotateSelected(Ogre::Vector3(15.0f, 30.0f, 45.0f)));
}

// Test setActiveWidget with nullptr: should not crash
TEST_F(TransformOperatorTestFixture, SetActiveWidgetNull) {
    TransformOperator* instance = TransformOperator::getSingleton();
    EXPECT_NO_THROW(instance->setActiveWidget(nullptr));
}

// Test onSelectionChanged when nothing is selected: should reset grid position and hide gizmos
TEST_F(TransformOperatorTestFixture, OnSelectionChangedEmpty) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    TransformOperator* instance = TransformOperator::getSingleton();
    ASSERT_TRUE(SelectionSet::getSingleton()->isEmpty());
    EXPECT_NO_THROW(instance->onSelectionChanged());
}


// Test setSelectedPosition with a selected node
TEST_F(TransformOperatorTestFixture, SetSelectedPositionWithSelection) {
    Manager* mgr = Manager::getSingletonPtr();
    TransformOperator* instance = TransformOperator::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("TestPosNode");
    ASSERT_NE(node, nullptr);
    SelectionSet::getSingleton()->selectOne(node);
    ASSERT_FALSE(SelectionSet::getSingleton()->isEmpty());
    Ogre::Vector3 newPos(10.0f, 20.0f, 30.0f);
    instance->setSelectedPosition(newPos);
    EXPECT_EQ(node->getPosition(), newPos);
}

TEST_F(TransformOperatorTestFixture, TranslateSelectedWithSelection) {
    Manager* mgr = Manager::getSingletonPtr();
    TransformOperator* instance = TransformOperator::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("TestTransNode");
    ASSERT_NE(node, nullptr);
    node->setPosition(5.0f, 5.0f, 5.0f);
    SelectionSet::getSingleton()->selectOne(node);
    Ogre::Vector3 offset(10.0f, 15.0f, 20.0f);
    instance->translateSelected(offset);
    EXPECT_EQ(node->getPosition(), Ogre::Vector3(15.0f, 20.0f, 25.0f));
}

TEST_F(TransformOperatorTestFixture, SetSelectedScaleWithSelection) {
    Manager* mgr = Manager::getSingletonPtr();
    TransformOperator* instance = TransformOperator::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("TestScaleNode");
    ASSERT_NE(node, nullptr);
    SelectionSet::getSingleton()->selectOne(node);
    Ogre::Vector3 newScale(2.0f, 3.0f, 4.0f);
    instance->setSelectedScale(newScale);
    EXPECT_EQ(node->getScale(), newScale);
}

TEST_F(TransformOperatorTestFixture, ScaleSelectedWithSelection) {
    Manager* mgr = Manager::getSingletonPtr();
    TransformOperator* instance = TransformOperator::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("TestScaleMulNode");
    ASSERT_NE(node, nullptr);
    node->setScale(2.0f, 2.0f, 2.0f);
    SelectionSet::getSingleton()->selectOne(node);
    Ogre::Vector3 scaleFactor(1.5f, 2.0f, 0.5f);
    instance->scaleSelected(scaleFactor);
    EXPECT_EQ(node->getScale(), Ogre::Vector3(3.0f, 4.0f, 1.0f));
}

TEST_F(TransformOperatorTestFixture, SetSelectedOrientationWithSelection) {
    Manager* mgr = Manager::getSingletonPtr();
    TransformOperator* instance = TransformOperator::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("TestOrientNode");
    ASSERT_NE(node, nullptr);
    SelectionSet::getSingleton()->selectOne(node);
    Ogre::Vector3 eulerAngles(45.0f, 90.0f, 30.0f);
    instance->setSelectedOrientation(eulerAngles);
    EXPECT_NE(node->getOrientation(), Ogre::Quaternion::IDENTITY);
}

TEST_F(TransformOperatorTestFixture, RotateSelectedQuaternionWithSelection) {
    Manager* mgr = Manager::getSingletonPtr();
    TransformOperator* instance = TransformOperator::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("TestRotQNode");
    ASSERT_NE(node, nullptr);
    SelectionSet::getSingleton()->selectOne(node);
    Ogre::Quaternion originalOrientation = node->getOrientation();
    Ogre::Quaternion rotation(Ogre::Degree(45), Ogre::Vector3::UNIT_Y);
    instance->rotateSelected(rotation);
    EXPECT_NE(node->getOrientation(), originalOrientation);
}

TEST_F(TransformOperatorTestFixture, RotateSelectedVectorWithSelection) {
    Manager* mgr = Manager::getSingletonPtr();
    TransformOperator* instance = TransformOperator::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("TestRotVNode");
    ASSERT_NE(node, nullptr);
    SelectionSet::getSingleton()->selectOne(node);
    Ogre::Quaternion originalOrientation = node->getOrientation();
    Ogre::Vector3 rotation(15.0f, 30.0f, 45.0f);
    instance->rotateSelected(rotation);
    EXPECT_NE(node->getOrientation(), originalOrientation);
}

TEST_F(TransformOperatorTestFixture, RemoveSelectedWithSelection) {
    Manager* mgr = Manager::getSingletonPtr();
    TransformOperator* instance = TransformOperator::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("TestNodeToRemove");
    ASSERT_NE(node, nullptr);
    QString nodeName = QString::fromStdString(node->getName());
    SelectionSet::getSingleton()->selectOne(node);
    ASSERT_FALSE(SelectionSet::getSingleton()->isEmpty());
    instance->removeSelected();
    EXPECT_TRUE(SelectionSet::getSingleton()->isEmpty());
    EXPECT_FALSE(mgr->hasSceneNode(nodeName));
}

TEST_F(TransformOperatorTestFixture, OnSelectionChangedWithSelection) {
    Manager* mgr = Manager::getSingletonPtr();
    TransformOperator* instance = TransformOperator::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("TestSelChgNode");
    ASSERT_NE(node, nullptr);
    node->setPosition(100.0f, 200.0f, 300.0f);
    SelectionSet::getSingleton()->selectOne(node);
    EXPECT_NO_THROW(instance->onSelectionChanged());
}

TEST_F(TransformOperatorTestFixture, SelectedPositionChangedSignal) {
    Manager* mgr = Manager::getSingletonPtr();
    TransformOperator* instance = TransformOperator::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("TestSigPosNode");
    ASSERT_NE(node, nullptr);
    SelectionSet::getSingleton()->selectOne(node);
    QSignalSpy spy(instance, &TransformOperator::selectedPositionChanged);
    Ogre::Vector3 newPos(10.0f, 20.0f, 30.0f);
    instance->setSelectedPosition(newPos);
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(TransformOperatorTestFixture, SelectedOrientationChangedSignal) {
    Manager* mgr = Manager::getSingletonPtr();
    TransformOperator* instance = TransformOperator::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("TestSigOrientNode");
    ASSERT_NE(node, nullptr);
    SelectionSet::getSingleton()->selectOne(node);
    QSignalSpy spy(instance, &TransformOperator::selectedOrientationChanged);
    Ogre::Vector3 eulerAngles(45.0f, 90.0f, 30.0f);
    instance->setSelectedOrientation(eulerAngles);
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(TransformOperatorTestFixture, ObjectsDeletedSignal) {
    Manager* mgr = Manager::getSingletonPtr();
    TransformOperator* instance = TransformOperator::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("TestNodeToDelete");
    ASSERT_NE(node, nullptr);
    SelectionSet::getSingleton()->selectOne(node);
    QSignalSpy spy(instance, &TransformOperator::objectsDeleted);
    instance->removeSelected();
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(TransformOperatorTestFixture, CycleTransformStates) {
    TransformOperator* instance = TransformOperator::getSingleton();
    Manager* mgr = Manager::getSingletonPtr();
    Ogre::SceneNode* node = mgr->addSceneNode("TestCycleNode");
    ASSERT_NE(node, nullptr);
    SelectionSet::getSingleton()->selectOne(node);
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_SELECT));
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_TRANSLATE));
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_ROTATE));
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_SELECT));
}

// --- New tests using in-memory entities ---

TEST_F(TransformOperatorTestFixture, TranslateEntityNode) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    auto mesh = createInMemoryTriangleMesh("TranslateEntityMesh");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("TranslateEntityNode");
    auto* entity = sceneMgr->createEntity("TranslateEntityEnt", mesh);
    node->attachObject(entity);

    SelectionSet::getSingleton()->selectOne(node);
    TransformOperator* instance = TransformOperator::getSingleton();
    instance->setSelectedPosition(Ogre::Vector3(10, 20, 30));

    EXPECT_EQ(node->getPosition(), Ogre::Vector3(10, 20, 30));
}

TEST_F(TransformOperatorTestFixture, ScaleEntityNode) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    auto mesh = createInMemoryTriangleMesh("ScaleEntityMesh");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("ScaleEntityNode");
    auto* entity = sceneMgr->createEntity("ScaleEntityEnt", mesh);
    node->attachObject(entity);

    SelectionSet::getSingleton()->selectOne(node);
    TransformOperator* instance = TransformOperator::getSingleton();
    instance->setSelectedScale(Ogre::Vector3(2, 3, 4));

    EXPECT_EQ(node->getScale(), Ogre::Vector3(2, 3, 4));
}

TEST_F(TransformOperatorTestFixture, RotateEntityNode) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    auto mesh = createInMemoryTriangleMesh("RotateEntityMesh");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("RotateEntityNode");
    auto* entity = sceneMgr->createEntity("RotateEntityEnt", mesh);
    node->attachObject(entity);

    SelectionSet::getSingleton()->selectOne(node);
    TransformOperator* instance = TransformOperator::getSingleton();
    instance->setSelectedOrientation(Ogre::Vector3(45, 90, 0));

    EXPECT_NE(node->getOrientation(), Ogre::Quaternion::IDENTITY);
}

TEST_F(TransformOperatorTestFixture, RemoveEntityNode) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    auto mesh = createInMemoryTriangleMesh("RemoveEntityMesh");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("RemoveEntityNode");
    auto* entity = sceneMgr->createEntity("RemoveEntityEnt", mesh);
    node->attachObject(entity);

    SelectionSet::getSingleton()->selectOne(node);
    TransformOperator* instance = TransformOperator::getSingleton();
    instance->removeSelected();

    EXPECT_TRUE(SelectionSet::getSingleton()->isEmpty());
    EXPECT_FALSE(Manager::getSingletonPtr()->hasSceneNode("RemoveEntityNode"));
}

TEST_F(TransformOperatorTestFixture, MultipleNodesTranslate) {
    Manager* mgr = Manager::getSingletonPtr();
    TransformOperator* instance = TransformOperator::getSingleton();

    Ogre::SceneNode* node1 = mgr->addSceneNode("MultiTrNode1");
    Ogre::SceneNode* node2 = mgr->addSceneNode("MultiTrNode2");
    ASSERT_NE(node1, nullptr);
    ASSERT_NE(node2, nullptr);

    node1->setPosition(0, 0, 0);
    node2->setPosition(10, 10, 10);

    SelectionSet::getSingleton()->selectOne(node1);
    SelectionSet::getSingleton()->append(node2);

    instance->translateSelected(Ogre::Vector3(5, 5, 5));

    EXPECT_EQ(node1->getPosition(), Ogre::Vector3(5, 5, 5));
    EXPECT_EQ(node2->getPosition(), Ogre::Vector3(15, 15, 15));
}

TEST_F(TransformOperatorTestFixture, OnSelectionChangedWithEntity) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    auto mesh = createInMemoryTriangleMesh("SelChangedEntityMesh");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("SelChangedEntityNode");
    auto* entity = sceneMgr->createEntity("SelChangedEntityEnt", mesh);
    node->attachObject(entity);
    node->setPosition(100, 200, 300);

    SelectionSet::getSingleton()->selectOne(node);
    TransformOperator* instance = TransformOperator::getSingleton();
    EXPECT_NO_THROW(instance->onSelectionChanged());
}

TEST_F(TransformOperatorTestFixture, TransformStateChangeWithEntity) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    auto mesh = createInMemoryTriangleMesh("StateChangeEntityMesh");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("StateChangeEntityNode");
    auto* entity = sceneMgr->createEntity("StateChangeEntityEnt", mesh);
    node->attachObject(entity);

    SelectionSet::getSingleton()->selectOne(node);
    TransformOperator* instance = TransformOperator::getSingleton();

    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_TRANSLATE));
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_ROTATE));
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_SELECT));
}
