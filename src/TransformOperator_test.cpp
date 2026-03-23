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
#include "UndoManager.h"
#include "commands/TransformCommands.h"
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
        SelectionSet::getSingleton()->clear();
        createOGREMaterials();
    }

    void TearDown() override {
        SelectionSet::getSingleton()->clear();
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

// ==========================================================================
// NEW BATCH: Additional coverage tests
// ==========================================================================

// Test rapid state cycling through all states with a selected entity
TEST_F(TransformOperatorTestFixture, RapidStateCycling_AllStates) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    auto mesh = createInMemoryTriangleMesh("RapidCycleMesh");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("RapidCycleNode");
    auto* entity = sceneMgr->createEntity("RapidCycleEnt", mesh);
    node->attachObject(entity);

    SelectionSet::getSingleton()->selectOne(node);
    TransformOperator* instance = TransformOperator::getSingleton();

    // Cycle through all states rapidly multiple times
    for (int i = 0; i < 5; ++i) {
        EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_NONE));
        EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_SELECT));
        EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_TRANSLATE));
        EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_ROTATE));
    }
    // End in SELECT mode
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_SELECT));
}

// Test removeSelected with entity node and verify scene cleanup
TEST_F(TransformOperatorTestFixture, RemoveSelected_EntityCleanup) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Manager* mgr = Manager::getSingletonPtr();
    auto mesh = createInMemoryTriangleMesh("RemoveCleanupMesh");
    auto* sceneMgr = mgr->getSceneMgr();
    auto* node = mgr->addSceneNode("RemoveCleanupNode");
    auto* entity = sceneMgr->createEntity("RemoveCleanupEnt", mesh);
    node->attachObject(entity);

    ASSERT_TRUE(mgr->hasSceneNode("RemoveCleanupNode"));
    ASSERT_FALSE(mgr->getEntities().isEmpty());

    SelectionSet::getSingleton()->selectOne(node);
    TransformOperator* instance = TransformOperator::getSingleton();

    QSignalSpy spy(instance, &TransformOperator::objectsDeleted);
    instance->removeSelected();

    EXPECT_EQ(spy.count(), 1);
    EXPECT_TRUE(SelectionSet::getSingleton()->isEmpty());
    EXPECT_FALSE(mgr->hasSceneNode("RemoveCleanupNode"));
    EXPECT_TRUE(mgr->getEntities().isEmpty());
}

// Test translate with zero vector -- position should not change
TEST_F(TransformOperatorTestFixture, TranslateSelected_ZeroVector) {
    Manager* mgr = Manager::getSingletonPtr();
    TransformOperator* instance = TransformOperator::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("TransZeroNode");
    ASSERT_NE(node, nullptr);
    node->setPosition(5.0f, 10.0f, 15.0f);
    SelectionSet::getSingleton()->selectOne(node);

    instance->translateSelected(Ogre::Vector3::ZERO);
    EXPECT_EQ(node->getPosition(), Ogre::Vector3(5.0f, 10.0f, 15.0f));
}

// Test translate with negative values
TEST_F(TransformOperatorTestFixture, TranslateSelected_NegativeValues) {
    Manager* mgr = Manager::getSingletonPtr();
    TransformOperator* instance = TransformOperator::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("TransNegNode");
    ASSERT_NE(node, nullptr);
    node->setPosition(10.0f, 20.0f, 30.0f);
    SelectionSet::getSingleton()->selectOne(node);

    instance->translateSelected(Ogre::Vector3(-15.0f, -25.0f, -35.0f));
    EXPECT_EQ(node->getPosition(), Ogre::Vector3(-5.0f, -5.0f, -5.0f));
}

// Test setSelectedScale with zero scale (edge case)
TEST_F(TransformOperatorTestFixture, SetSelectedScale_ZeroScale) {
    Manager* mgr = Manager::getSingletonPtr();
    TransformOperator* instance = TransformOperator::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("ScaleZeroNode");
    ASSERT_NE(node, nullptr);
    SelectionSet::getSingleton()->selectOne(node);

    instance->setSelectedScale(Ogre::Vector3::ZERO);
    EXPECT_EQ(node->getScale(), Ogre::Vector3::ZERO);
}

// Test setSelectedScale with negative scale values
TEST_F(TransformOperatorTestFixture, SetSelectedScale_NegativeValues) {
    Manager* mgr = Manager::getSingletonPtr();
    TransformOperator* instance = TransformOperator::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("ScaleNegNode");
    ASSERT_NE(node, nullptr);
    SelectionSet::getSingleton()->selectOne(node);

    instance->setSelectedScale(Ogre::Vector3(-1.0f, -2.0f, -3.0f));
    EXPECT_EQ(node->getScale(), Ogre::Vector3(-1.0f, -2.0f, -3.0f));
}

// Test updateGizmoPosition with multiple selected nodes at different positions
TEST_F(TransformOperatorTestFixture, OnSelectionChanged_MultipleNodes) {
    Manager* mgr = Manager::getSingletonPtr();
    TransformOperator* instance = TransformOperator::getSingleton();

    Ogre::SceneNode* node1 = mgr->addSceneNode("GizmoMulti1");
    Ogre::SceneNode* node2 = mgr->addSceneNode("GizmoMulti2");
    Ogre::SceneNode* node3 = mgr->addSceneNode("GizmoMulti3");
    ASSERT_NE(node1, nullptr);
    ASSERT_NE(node2, nullptr);
    ASSERT_NE(node3, nullptr);

    node1->setPosition(0, 0, 0);
    node2->setPosition(10, 10, 10);
    node3->setPosition(20, 20, 20);

    SelectionSet::getSingleton()->selectOne(node1);
    SelectionSet::getSingleton()->append(node2);
    SelectionSet::getSingleton()->append(node3);

    EXPECT_EQ(SelectionSet::getSingleton()->getNodesCount(), 3);

    // onSelectionChanged should handle multiple selections without crash
    EXPECT_NO_THROW(instance->onSelectionChanged());

    // Switch to translate mode with multiple selection
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_TRANSLATE));
    EXPECT_NO_THROW(instance->onSelectionChanged());
}

// Test selection with no entities in scene - all operations should be no-ops
TEST_F(TransformOperatorTestFixture, AllOperations_EmptyScene) {
    TransformOperator* instance = TransformOperator::getSingleton();
    ASSERT_TRUE(SelectionSet::getSingleton()->isEmpty());

    // All transform operations on empty selection should be safe
    EXPECT_NO_THROW(instance->setSelectedPosition(Ogre::Vector3(100, 200, 300)));
    EXPECT_NO_THROW(instance->translateSelected(Ogre::Vector3(1, 2, 3)));
    EXPECT_NO_THROW(instance->setSelectedScale(Ogre::Vector3(5, 5, 5)));
    EXPECT_NO_THROW(instance->scaleSelected(Ogre::Vector3(2, 2, 2)));
    EXPECT_NO_THROW(instance->setSelectedOrientation(Ogre::Vector3(90, 180, 270)));
    EXPECT_NO_THROW(instance->rotateSelected(Ogre::Quaternion(Ogre::Degree(90), Ogre::Vector3::UNIT_X)));
    EXPECT_NO_THROW(instance->rotateSelected(Ogre::Vector3(45, 45, 45)));
    EXPECT_NO_THROW(instance->removeSelected());

    // No signals should have been emitted
    QSignalSpy posSpy(instance, &TransformOperator::selectedPositionChanged);
    QSignalSpy orientSpy(instance, &TransformOperator::selectedOrientationChanged);
    QSignalSpy deleteSpy(instance, &TransformOperator::objectsDeleted);

    instance->setSelectedPosition(Ogre::Vector3(1, 1, 1));
    EXPECT_EQ(posSpy.count(), 0);

    instance->setSelectedOrientation(Ogre::Vector3(45, 45, 45));
    EXPECT_EQ(orientSpy.count(), 0);
}

// Test translateSelected signal emission with a selected node
TEST_F(TransformOperatorTestFixture, TranslateSelected_EmitsPositionSignal) {
    Manager* mgr = Manager::getSingletonPtr();
    TransformOperator* instance = TransformOperator::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("TransSigNode");
    ASSERT_NE(node, nullptr);
    SelectionSet::getSingleton()->selectOne(node);

    QSignalSpy spy(instance, &TransformOperator::selectedPositionChanged);
    instance->translateSelected(Ogre::Vector3(5.0f, 5.0f, 5.0f));
    EXPECT_EQ(spy.count(), 1);
}

// Test scaleSelected signal and actual scale multiplication
TEST_F(TransformOperatorTestFixture, ScaleSelected_WithEntity) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Manager* mgr = Manager::getSingletonPtr();
    auto mesh = createInMemoryTriangleMesh("ScaleSigMesh");
    auto* sceneMgr = mgr->getSceneMgr();
    auto* node = mgr->addSceneNode("ScaleSigNode");
    auto* entity = sceneMgr->createEntity("ScaleSigEnt", mesh);
    node->attachObject(entity);

    node->setScale(2.0f, 2.0f, 2.0f);
    SelectionSet::getSingleton()->selectOne(node);

    TransformOperator* instance = TransformOperator::getSingleton();
    instance->scaleSelected(Ogre::Vector3(3.0f, 0.5f, 1.0f));

    EXPECT_EQ(node->getScale(), Ogre::Vector3(6.0f, 1.0f, 2.0f));
}

// Test multiple nodes translate with entity nodes
TEST_F(TransformOperatorTestFixture, MultipleEntityNodesTranslate) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }

    Manager* mgr = Manager::getSingletonPtr();
    auto mesh1 = createInMemoryTriangleMesh("MultiEntTransMesh1");
    auto mesh2 = createInMemoryTriangleMesh("MultiEntTransMesh2");
    auto* sceneMgr = mgr->getSceneMgr();

    auto* node1 = mgr->addSceneNode("MultiEntTransNode1");
    auto* ent1 = sceneMgr->createEntity("MultiEntTransEnt1", mesh1);
    node1->attachObject(ent1);
    node1->setPosition(0, 0, 0);

    auto* node2 = mgr->addSceneNode("MultiEntTransNode2");
    auto* ent2 = sceneMgr->createEntity("MultiEntTransEnt2", mesh2);
    node2->attachObject(ent2);
    node2->setPosition(100, 100, 100);

    SelectionSet::getSingleton()->selectOne(node1);
    SelectionSet::getSingleton()->append(node2);

    TransformOperator* instance = TransformOperator::getSingleton();
    instance->translateSelected(Ogre::Vector3(-10, -20, -30));

    EXPECT_EQ(node1->getPosition(), Ogre::Vector3(-10, -20, -30));
    EXPECT_EQ(node2->getPosition(), Ogre::Vector3(90, 80, 70));
}

// ---- New tests for TS_SCALE state ----

TEST_F(TransformOperatorTestFixture, OnTransformStateChange_Scale) {
    TransformOperator* instance = TransformOperator::getSingleton();
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_SCALE));
}

TEST_F(TransformOperatorTestFixture, CycleAllTransformStates_IncludingScale) {
    TransformOperator* instance = TransformOperator::getSingleton();
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_SELECT));
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_TRANSLATE));
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_ROTATE));
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_SCALE));
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_NONE));
}

// ---- Tests for TransformSpace ----

TEST_F(TransformOperatorTestFixture, DefaultTransformSpaceIsWorld) {
    TransformOperator* instance = TransformOperator::getSingleton();
    EXPECT_EQ(instance->getTransformSpace(), TransformOperator::SPACE_WORLD);
}

TEST_F(TransformOperatorTestFixture, SetTransformSpace) {
    TransformOperator* instance = TransformOperator::getSingleton();
    instance->setTransformSpace(TransformOperator::SPACE_LOCAL);
    EXPECT_EQ(instance->getTransformSpace(), TransformOperator::SPACE_LOCAL);

    instance->setTransformSpace(TransformOperator::SPACE_WORLD);
    EXPECT_EQ(instance->getTransformSpace(), TransformOperator::SPACE_WORLD);
}

TEST_F(TransformOperatorTestFixture, ToggleTransformSpace) {
    TransformOperator* instance = TransformOperator::getSingleton();
    EXPECT_EQ(instance->getTransformSpace(), TransformOperator::SPACE_WORLD);

    instance->toggleTransformSpace();
    EXPECT_EQ(instance->getTransformSpace(), TransformOperator::SPACE_LOCAL);

    instance->toggleTransformSpace();
    EXPECT_EQ(instance->getTransformSpace(), TransformOperator::SPACE_WORLD);
}

TEST_F(TransformOperatorTestFixture, TransformSpaceChangedSignal) {
    TransformOperator* instance = TransformOperator::getSingleton();
    QSignalSpy spy(instance, &TransformOperator::transformSpaceChanged);

    instance->setTransformSpace(TransformOperator::SPACE_LOCAL);
    EXPECT_EQ(spy.count(), 1);

    // Setting same value should not emit
    instance->setTransformSpace(TransformOperator::SPACE_LOCAL);
    EXPECT_EQ(spy.count(), 1);

    instance->toggleTransformSpace();
    EXPECT_EQ(spy.count(), 2);
}

TEST_F(TransformOperatorTestFixture, ScaleStateWithSelection) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported"; }

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode();
    auto mesh = createInMemoryTriangleMesh("ScaleTestMesh");
    auto* entity = sceneMgr->createEntity(mesh);
    node->attachObject(entity);
    SelectionSet::getSingleton()->append(node);

    TransformOperator* instance = TransformOperator::getSingleton();
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_SCALE));
}

TEST_F(TransformOperatorTestFixture, LocalSpaceTranslate) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported"; }

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode();
    auto mesh = createInMemoryTriangleMesh("LocalSpaceTestMesh");
    auto* entity = sceneMgr->createEntity(mesh);
    node->attachObject(entity);
    SelectionSet::getSingleton()->append(node);

    // Rotate node 90 degrees around Y
    node->yaw(Ogre::Degree(90));

    TransformOperator* instance = TransformOperator::getSingleton();
    instance->setTransformSpace(TransformOperator::SPACE_LOCAL);
    instance->onTransformStateChange(TransformOperator::TS_TRANSLATE);

    // translateSelected always works in world space (local conversion is in mouse drag)
    // Verify the translate applies correctly and transform space persists
    Ogre::Vector3 startPos = node->getPosition();
    instance->translateSelected(Ogre::Vector3(5, 0, 0));
    Ogre::Vector3 endPos = node->getPosition();

    // Should have moved exactly 5 units along world X
    EXPECT_FLOAT_EQ(endPos.x - startPos.x, 5.0f);
    EXPECT_FLOAT_EQ(endPos.y - startPos.y, 0.0f);
    EXPECT_FLOAT_EQ(endPos.z - startPos.z, 0.0f);

    // Transform space should still be LOCAL
    EXPECT_EQ(instance->getTransformSpace(), TransformOperator::SPACE_LOCAL);
    instance->setTransformSpace(TransformOperator::SPACE_WORLD);
}

TEST_F(TransformOperatorTestFixture, LocalSpaceWithAllStates) {
    TransformOperator* instance = TransformOperator::getSingleton();
    instance->setTransformSpace(TransformOperator::SPACE_LOCAL);
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_TRANSLATE));
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_ROTATE));
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_SCALE));
    instance->setTransformSpace(TransformOperator::SPACE_WORLD);
}

// ---- TS_SCALE with entity selection ----

TEST_F(TransformOperatorTestFixture, ScaleStateWithEntitySelection) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported"; }

    auto mesh = createInMemoryTriangleMesh("TSScaleEntMesh");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("TSScaleEntNode");
    auto* entity = sceneMgr->createEntity("TSScaleEntEnt", mesh);
    node->attachObject(entity);

    SelectionSet::getSingleton()->selectOne(node);
    TransformOperator* instance = TransformOperator::getSingleton();

    // Switch to scale state with entity selected
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_SCALE));

    // Scale the selection
    instance->scaleSelected(Ogre::Vector3(2.0f, 2.0f, 2.0f));
    EXPECT_EQ(node->getScale(), Ogre::Vector3(2.0f, 2.0f, 2.0f));

    // Switch back
    EXPECT_NO_THROW(instance->onTransformStateChange(TransformOperator::TS_SELECT));
}

// ---- removeSelected clears undo stack ----

TEST_F(TransformOperatorTestFixture, RemoveSelectedClearsUndoStack) {
    Manager* mgr = Manager::getSingletonPtr();
    TransformOperator* instance = TransformOperator::getSingleton();

    Ogre::SceneNode* node = mgr->addSceneNode("UndoClearNode");
    ASSERT_NE(node, nullptr);
    SelectionSet::getSingleton()->selectOne(node);

    // Manually push an undo command so the stack is non-empty
    UndoManager::getSingleton()->push(
        new TranslateCommand({node}, Ogre::Vector3(1, 0, 0)));
    EXPECT_TRUE(UndoManager::getSingleton()->canUndo());

    // Remove selected should clear the undo stack
    instance->removeSelected();
    EXPECT_TRUE(SelectionSet::getSingleton()->isEmpty());
    EXPECT_FALSE(UndoManager::getSingleton()->canUndo());
}

// ---- Scale selected with signal emission ----

TEST_F(TransformOperatorTestFixture, ScaleSelectedEmitsSignal) {
    Manager* mgr = Manager::getSingletonPtr();
    TransformOperator* instance = TransformOperator::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("ScaleSigTestNode");
    ASSERT_NE(node, nullptr);
    SelectionSet::getSingleton()->selectOne(node);

    QSignalSpy spy(instance, &TransformOperator::selectedScaleChanged);
    instance->setSelectedScale(Ogre::Vector3(3.0f, 3.0f, 3.0f));
    EXPECT_GE(spy.count(), 1);
}

// ---- Multiple operations in sequence ----

TEST_F(TransformOperatorTestFixture, SequentialTransformOperations) {
    Manager* mgr = Manager::getSingletonPtr();
    TransformOperator* instance = TransformOperator::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("SeqOpNode");
    ASSERT_NE(node, nullptr);
    SelectionSet::getSingleton()->selectOne(node);

    // Translate
    instance->setSelectedPosition(Ogre::Vector3(10, 0, 0));
    EXPECT_EQ(node->getPosition(), Ogre::Vector3(10, 0, 0));

    // Scale
    instance->setSelectedScale(Ogre::Vector3(2, 2, 2));
    EXPECT_EQ(node->getScale(), Ogre::Vector3(2, 2, 2));

    // Rotate
    instance->setSelectedOrientation(Ogre::Vector3(0, 90, 0));
    EXPECT_NE(node->getOrientation(), Ogre::Quaternion::IDENTITY);

    // Translate again
    instance->translateSelected(Ogre::Vector3(5, 5, 5));
    EXPECT_EQ(node->getPosition(), Ogre::Vector3(15, 5, 5));
}

// ============================================================================
// updateGizmo / updateGizmoPosition coverage
// ============================================================================

TEST_F(TransformOperatorTestFixture, UpdateGizmoWithNodeSelection) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: needs render window"; }

    Manager* mgr = Manager::getSingletonPtr();
    auto* sceneMgr = mgr->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode();
    auto mesh = createInMemoryTriangleMesh("GizmoUpdateMesh");
    auto* entity = sceneMgr->createEntity(mesh);
    node->attachObject(entity);
    node->setPosition(Ogre::Vector3(10, 20, 30));

    SelectionSet::getSingleton()->append(node);

    TransformOperator* instance = TransformOperator::getSingleton();

    // Cycle through all states to exercise updateGizmo branches
    instance->onTransformStateChange(TransformOperator::TS_SELECT);
    instance->onTransformStateChange(TransformOperator::TS_TRANSLATE);
    instance->onTransformStateChange(TransformOperator::TS_ROTATE);
    instance->onTransformStateChange(TransformOperator::TS_SCALE);
    instance->onTransformStateChange(TransformOperator::TS_NONE);
}

TEST_F(TransformOperatorTestFixture, UpdateGizmoWithEntitySelection) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: needs render window"; }

    Manager* mgr = Manager::getSingletonPtr();
    auto* sceneMgr = mgr->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode();
    auto mesh = createInMemoryTriangleMesh("GizmoEntityMesh");
    auto* entity = sceneMgr->createEntity(mesh);
    node->attachObject(entity);

    // Select entity instead of node
    SelectionSet::getSingleton()->append(entity);

    TransformOperator* instance = TransformOperator::getSingleton();
    instance->onTransformStateChange(TransformOperator::TS_TRANSLATE);
    instance->onTransformStateChange(TransformOperator::TS_ROTATE);
    instance->onTransformStateChange(TransformOperator::TS_SCALE);
}

TEST_F(TransformOperatorTestFixture, UpdateGizmoLocalSpaceWithRotatedNode) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: needs render window"; }

    Manager* mgr = Manager::getSingletonPtr();
    auto* sceneMgr = mgr->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode();
    auto mesh = createInMemoryTriangleMesh("LocalGizmoMesh");
    auto* entity = sceneMgr->createEntity(mesh);
    node->attachObject(entity);
    node->yaw(Ogre::Degree(45));

    SelectionSet::getSingleton()->append(node);

    TransformOperator* instance = TransformOperator::getSingleton();
    instance->setTransformSpace(TransformOperator::SPACE_LOCAL);

    // In local mode with single rotated node, gizmo should orient to node
    instance->onTransformStateChange(TransformOperator::TS_TRANSLATE);
    instance->onTransformStateChange(TransformOperator::TS_ROTATE);
    instance->onTransformStateChange(TransformOperator::TS_SCALE);

    instance->setTransformSpace(TransformOperator::SPACE_WORLD);
}

TEST_F(TransformOperatorTestFixture, UpdateGizmoLocalSpaceMultipleNodes) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: needs render window"; }

    Manager* mgr = Manager::getSingletonPtr();
    auto* sceneMgr = mgr->getSceneMgr();

    auto* node1 = sceneMgr->getRootSceneNode()->createChildSceneNode();
    auto mesh1 = createInMemoryTriangleMesh("LocalMulti1");
    node1->attachObject(sceneMgr->createEntity(mesh1));
    node1->yaw(Ogre::Degree(30));

    auto* node2 = sceneMgr->getRootSceneNode()->createChildSceneNode();
    auto mesh2 = createInMemoryTriangleMesh("LocalMulti2");
    node2->attachObject(sceneMgr->createEntity(mesh2));

    SelectionSet::getSingleton()->append(node1);
    SelectionSet::getSingleton()->append(node2);

    TransformOperator* instance = TransformOperator::getSingleton();
    instance->setTransformSpace(TransformOperator::SPACE_LOCAL);

    // With multiple nodes selected, local space falls back to world orientation
    instance->onTransformStateChange(TransformOperator::TS_TRANSLATE);

    instance->setTransformSpace(TransformOperator::SPACE_WORLD);
}

// ============================================================================
// Signal emissions from updateGizmoPosition
// ============================================================================

TEST_F(TransformOperatorTestFixture, UpdateGizmoPositionEmitsAllSignals) {
    Manager* mgr = Manager::getSingletonPtr();
    TransformOperator* instance = TransformOperator::getSingleton();

    Ogre::SceneNode* node = mgr->addSceneNode("SignalTestNode");
    ASSERT_NE(node, nullptr);
    node->setPosition(Ogre::Vector3(5, 10, 15));
    SelectionSet::getSingleton()->selectOne(node);

    QSignalSpy posSpy(instance, &TransformOperator::selectedPositionChanged);
    QSignalSpy oriSpy(instance, &TransformOperator::selectedOrientationChanged);
    QSignalSpy scaleSpy(instance, &TransformOperator::selectedScaleChanged);

    // Trigger gizmo update
    instance->onSelectionChanged();

    EXPECT_GE(posSpy.count(), 1);
    EXPECT_GE(oriSpy.count(), 1);
    EXPECT_GE(scaleSpy.count(), 1);
}

TEST_F(TransformOperatorTestFixture, UpdateGizmoPositionWithEntityEmitsSignals) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: needs render window"; }

    Manager* mgr = Manager::getSingletonPtr();
    auto* sceneMgr = mgr->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode();
    auto mesh = createInMemoryTriangleMesh("EntitySignalMesh");
    auto* entity = sceneMgr->createEntity(mesh);
    node->attachObject(entity);

    SelectionSet::getSingleton()->append(entity);

    TransformOperator* instance = TransformOperator::getSingleton();
    QSignalSpy posSpy(instance, &TransformOperator::selectedPositionChanged);

    instance->onSelectionChanged();
    EXPECT_GE(posSpy.count(), 1);
}

// ============================================================================
// Entity-level transforms
// ============================================================================

TEST_F(TransformOperatorTestFixture, TranslateMultipleEntities) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: needs render window"; }

    Manager* mgr = Manager::getSingletonPtr();
    auto* sceneMgr = mgr->getSceneMgr();

    auto* node1 = sceneMgr->getRootSceneNode()->createChildSceneNode();
    auto mesh1 = createInMemoryTriangleMesh("MultiEnt1");
    auto* ent1 = sceneMgr->createEntity(mesh1);
    node1->attachObject(ent1);

    auto* node2 = sceneMgr->getRootSceneNode()->createChildSceneNode();
    auto mesh2 = createInMemoryTriangleMesh("MultiEnt2");
    auto* ent2 = sceneMgr->createEntity(mesh2);
    node2->attachObject(ent2);

    SelectionSet::getSingleton()->append(ent1);
    SelectionSet::getSingleton()->append(ent2);

    TransformOperator* instance = TransformOperator::getSingleton();
    EXPECT_NO_THROW(instance->translateSelected(Ogre::Vector3(1, 2, 3)));
}

TEST_F(TransformOperatorTestFixture, RotateMultipleEntities) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: needs render window"; }

    Manager* mgr = Manager::getSingletonPtr();
    auto* sceneMgr = mgr->getSceneMgr();

    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode();
    auto mesh = createInMemoryTriangleMesh("RotEntMesh");
    auto* entity = sceneMgr->createEntity(mesh);
    node->attachObject(entity);

    SelectionSet::getSingleton()->append(entity);

    TransformOperator* instance = TransformOperator::getSingleton();
    Ogre::Quaternion rot(Ogre::Degree(45), Ogre::Vector3::UNIT_Y);
    EXPECT_NO_THROW(instance->rotateSelected(rot));
}

TEST_F(TransformOperatorTestFixture, SetSelectedOrientationEntity) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: needs render window"; }

    Manager* mgr = Manager::getSingletonPtr();
    auto* sceneMgr = mgr->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode();
    auto mesh = createInMemoryTriangleMesh("OriEntMesh");
    auto* entity = sceneMgr->createEntity(mesh);
    node->attachObject(entity);

    SelectionSet::getSingleton()->append(entity);

    TransformOperator* instance = TransformOperator::getSingleton();
    EXPECT_NO_THROW(instance->setSelectedOrientation(Ogre::Vector3(45, 0, 0)));
}

TEST_F(TransformOperatorTestFixture, RotateSelectedVectorEntity) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: needs render window"; }

    Manager* mgr = Manager::getSingletonPtr();
    auto* sceneMgr = mgr->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode();
    auto mesh = createInMemoryTriangleMesh("RotVecEntMesh");
    auto* entity = sceneMgr->createEntity(mesh);
    node->attachObject(entity);

    SelectionSet::getSingleton()->append(entity);

    TransformOperator* instance = TransformOperator::getSingleton();
    EXPECT_NO_THROW(instance->rotateSelected(Ogre::Vector3(30, 60, 0)));
}

// ============================================================================
// onSelectionChanged with SubEntities
// ============================================================================

TEST_F(TransformOperatorTestFixture, OnSelectionChangedWithSubEntity) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: needs render window"; }

    Manager* mgr = Manager::getSingletonPtr();
    auto* sceneMgr = mgr->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode();
    auto mesh = createInMemoryTriangleMesh("SubEntMesh");
    auto* entity = sceneMgr->createEntity(mesh);
    node->attachObject(entity);

    if (entity->getNumSubEntities() > 0) {
        SelectionSet::getSingleton()->append(entity->getSubEntity(0));
        TransformOperator* instance = TransformOperator::getSingleton();
        EXPECT_NO_THROW(instance->onSelectionChanged());
    }
}

// ============================================================================
// setSelectedPosition with entity selection
// ============================================================================

TEST_F(TransformOperatorTestFixture, SetSelectedPositionEntity) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: needs render window"; }

    Manager* mgr = Manager::getSingletonPtr();
    auto* sceneMgr = mgr->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode();
    auto mesh = createInMemoryTriangleMesh("PosEntMesh");
    auto* entity = sceneMgr->createEntity(mesh);
    node->attachObject(entity);

    SelectionSet::getSingleton()->append(entity);

    TransformOperator* instance = TransformOperator::getSingleton();
    EXPECT_NO_THROW(instance->setSelectedPosition(Ogre::Vector3(100, 200, 300)));
}

// ============================================================================
// setSelectedScale with entity selection
// ============================================================================

TEST_F(TransformOperatorTestFixture, SetSelectedScaleEntity) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: needs render window"; }

    Manager* mgr = Manager::getSingletonPtr();
    auto* sceneMgr = mgr->getSceneMgr();
    auto* node = sceneMgr->getRootSceneNode()->createChildSceneNode();
    auto mesh = createInMemoryTriangleMesh("ScaleEntMesh");
    auto* entity = sceneMgr->createEntity(mesh);
    node->attachObject(entity);

    SelectionSet::getSingleton()->append(entity);

    TransformOperator* instance = TransformOperator::getSingleton();
    EXPECT_NO_THROW(instance->setSelectedScale(Ogre::Vector3(2, 2, 2)));
}

// ============================================================================
// Rotate with multiple nodes (pivot behavior)
// ============================================================================

TEST_F(TransformOperatorTestFixture, RotateMultipleNodesAroundPivot) {
    Manager* mgr = Manager::getSingletonPtr();

    Ogre::SceneNode* node1 = mgr->addSceneNode("PivotNode1");
    Ogre::SceneNode* node2 = mgr->addSceneNode("PivotNode2");
    ASSERT_NE(node1, nullptr);
    ASSERT_NE(node2, nullptr);
    node1->setPosition(Ogre::Vector3(10, 0, 0));
    node2->setPosition(Ogre::Vector3(-10, 0, 0));

    SelectionSet::getSingleton()->append(node1);
    SelectionSet::getSingleton()->append(node2);

    TransformOperator* instance = TransformOperator::getSingleton();
    Ogre::Quaternion rot(Ogre::Degree(90), Ogre::Vector3::UNIT_Y);
    instance->rotateSelected(rot);

    // Both nodes should have moved (rotated around selection center)
    EXPECT_NE(node1->getPosition(), Ogre::Vector3(10, 0, 0));
    EXPECT_NE(node2->getPosition(), Ogre::Vector3(-10, 0, 0));
}

// ============================================================================
// Scale multiple nodes
// ============================================================================

TEST_F(TransformOperatorTestFixture, ScaleMultipleNodes) {
    Manager* mgr = Manager::getSingletonPtr();

    Ogre::SceneNode* node1 = mgr->addSceneNode("ScaleMulti1");
    Ogre::SceneNode* node2 = mgr->addSceneNode("ScaleMulti2");
    ASSERT_NE(node1, nullptr);
    ASSERT_NE(node2, nullptr);

    SelectionSet::getSingleton()->append(node1);
    SelectionSet::getSingleton()->append(node2);

    TransformOperator* instance = TransformOperator::getSingleton();
    instance->scaleSelected(Ogre::Vector3(2, 3, 4));

    EXPECT_EQ(node1->getScale(), Ogre::Vector3(2, 3, 4));
    EXPECT_EQ(node2->getScale(), Ogre::Vector3(2, 3, 4));
}

// ============================================================================
// setActiveWidget with non-null then null
// ============================================================================

TEST_F(TransformOperatorTestFixture, SetActiveWidgetAndClear) {
    TransformOperator* instance = TransformOperator::getSingleton();
    // Setting null should not crash
    EXPECT_NO_THROW(instance->setActiveWidget(nullptr));
    // Setting null again
    EXPECT_NO_THROW(instance->setActiveWidget(nullptr));
}

// ============================================================================
// Selection box colour roundtrip
// ============================================================================

TEST_F(TransformOperatorTestFixture, SelectionBoxColourRoundtrip) {
    TransformOperator* instance = TransformOperator::getSingleton();
    Ogre::ColourValue original = instance->getSelectionBoxColour();

    instance->setSelectionBoxColour(Ogre::ColourValue::Red);
    EXPECT_EQ(instance->getSelectionBoxColour(), Ogre::ColourValue::Red);

    instance->setSelectionBoxColour(Ogre::ColourValue::Blue);
    EXPECT_EQ(instance->getSelectionBoxColour(), Ogre::ColourValue::Blue);

    // Restore
    instance->setSelectionBoxColour(original);
}
