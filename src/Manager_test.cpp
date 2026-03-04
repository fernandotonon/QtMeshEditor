#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "Manager.h"
#include "GlobalDefinitions.h"
#include "PrimitiveObject.h"
#include <QMap>
#include "SelectionSet.h"
#include <OgreException.h>
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QThread>
#include "TestHelpers.h"

using ::testing::Mock;

// Test fixture for Manager tests with minimal setup (no Ogre initialization)
class ManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
    }

    void TearDown() override {
        // Clean up the Manager singleton
        Manager::kill();
    }

    QApplication* app = nullptr;
};

// Test fixture for Manager tests that use the headless Manager singleton
class ManagerHeadlessTest : public ::testing::Test {
protected:
    void SetUp() override {
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
        }
        createStandardOgreMaterials();
    }

    void TearDown() override {
        if (app)
        {
            app->processEvents();
        }
    }

    QApplication* app = nullptr;
};

// Test the forbidden name function without creating full Manager
TEST_F(ManagerTest, Forbidden_Name)
{
    // Test static functionality that doesn't require full initialization
    EXPECT_TRUE(QString("TPCameraChildSceneNode").startsWith("TPCameraChildSceneNode"));
    EXPECT_TRUE(QString("GridLine_node").startsWith("GridLine_node"));
    EXPECT_TRUE(QString("Unnamed_something").startsWith("Unnamed_"));
    EXPECT_FALSE(QString("Cube").startsWith("TPCameraChildSceneNode"));
    EXPECT_FALSE(QString("Cube_0").startsWith("GridLine_node"));

    // Test the actual logic would work (without Manager singleton)
    auto isForbiddenNodeName = [](const QString &_name) {
        return (_name=="TPCameraChildSceneNode"
                ||_name=="GridLine_node"
                ||_name==SELECTIONBOX_OBJECT_NAME
                ||_name==TRANSFORM_OBJECT_NAME
                ||_name.startsWith("Unnamed_"));
    };

    EXPECT_EQ(isForbiddenNodeName("Cube"), false);
    EXPECT_EQ(isForbiddenNodeName("Cube_0"), false);
    EXPECT_EQ(isForbiddenNodeName("Cube_1"), false);
    EXPECT_EQ(isForbiddenNodeName("TPCameraChildSceneNode"), true);
    EXPECT_EQ(isForbiddenNodeName("TPCameraChildSceneNode_0"), false);
    EXPECT_EQ(isForbiddenNodeName("GridLine_node"), true);
    EXPECT_EQ(isForbiddenNodeName("Unnamed_"), true);
    EXPECT_EQ(isForbiddenNodeName(TRANSFORM_OBJECT_NAME), true);
    EXPECT_EQ(isForbiddenNodeName(SELECTIONBOX_OBJECT_NAME), true);
}

// Simple validation test without full scene creation
TEST_F(ManagerTest, BasicValidation)
{
    // Test basic string validations that Manager would use
    QString validFileExt = ".mesh .xml .fbx .dae .obj .blend .3ds .ase .ply .x .ms3d .lwo .lws .lxo .stl";

    EXPECT_TRUE(validFileExt.contains(".mesh"));
    EXPECT_TRUE(validFileExt.contains(".fbx"));
    EXPECT_FALSE(validFileExt.contains(".invalid"));

    // Test that we could check valid file extensions
    auto isValidExtension = [&validFileExt](const QString& filename) {
        for(const QString& ext : validFileExt.split(" ", Qt::SkipEmptyParts)) {
            if(filename.endsWith(ext, Qt::CaseInsensitive)) {
                return true;
            }
        }
        return false;
    };

    EXPECT_TRUE(isValidExtension("ninja.mesh"));
    EXPECT_TRUE(isValidExtension("robot.fbx"));
    EXPECT_FALSE(isValidExtension("invalid.txt"));
}

// --- Headless Manager tests ---

TEST_F(ManagerHeadlessTest, AddSceneNode)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    Ogre::SceneNode* node = mgr->addSceneNode("TestNode");
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(mgr->hasSceneNode("TestNode"));
}

TEST_F(ManagerHeadlessTest, AddMultipleSceneNodes)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    // First node should get the exact name
    Ogre::SceneNode* node1 = mgr->addSceneNode("Cube");
    ASSERT_NE(node1, nullptr);
    EXPECT_TRUE(mgr->hasSceneNode("Cube"));

    // Second node with the same name should get auto-numbered (Cube1)
    Ogre::SceneNode* node2 = mgr->addSceneNode("Cube");
    ASSERT_NE(node2, nullptr);
    EXPECT_TRUE(mgr->hasSceneNode("Cube1"));

    // Third node with the same name should get Cube2
    Ogre::SceneNode* node3 = mgr->addSceneNode("Cube");
    ASSERT_NE(node3, nullptr);
    EXPECT_TRUE(mgr->hasSceneNode("Cube2"));

    // All three should be distinct nodes
    EXPECT_NE(node1, node2);
    EXPECT_NE(node2, node3);
    EXPECT_NE(node1, node3);
}

TEST_F(ManagerHeadlessTest, DestroySceneNode)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    mgr->addSceneNode("NodeToDestroy");
    ASSERT_TRUE(mgr->hasSceneNode("NodeToDestroy"));

    mgr->destroySceneNode("NodeToDestroy");
    EXPECT_FALSE(mgr->hasSceneNode("NodeToDestroy"));
}

TEST_F(ManagerHeadlessTest, HasSceneNode)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    // Non-existent node should return false
    EXPECT_FALSE(mgr->hasSceneNode("NonExistentNode"));
    EXPECT_FALSE(mgr->hasSceneNode("AnotherMissing"));
    EXPECT_FALSE(mgr->hasSceneNode(""));

    // After adding, should return true
    mgr->addSceneNode("ExistingNode");
    EXPECT_TRUE(mgr->hasSceneNode("ExistingNode"));

    // Other names should still return false
    EXPECT_FALSE(mgr->hasSceneNode("StillMissing"));
}

TEST_F(ManagerHeadlessTest, GetSceneNodes)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    // Initially no user scene nodes (forbidden names are filtered out)
    QList<Ogre::SceneNode*>& nodes = mgr->getSceneNodes();
    int initialCount = nodes.count();

    // Add some user nodes
    mgr->addSceneNode("UserNode1");
    mgr->addSceneNode("UserNode2");
    mgr->addSceneNode("UserNode3");

    QList<Ogre::SceneNode*>& updatedNodes = mgr->getSceneNodes();
    EXPECT_EQ(updatedNodes.count(), initialCount + 3);

    // Verify the nodes are in the list by checking their names
    bool foundNode1 = false, foundNode2 = false, foundNode3 = false;
    for (Ogre::SceneNode* sn : updatedNodes)
    {
        QString name = sn->getName().c_str();
        if (name == "UserNode1") foundNode1 = true;
        if (name == "UserNode2") foundNode2 = true;
        if (name == "UserNode3") foundNode3 = true;

        // Verify that no forbidden names appear in the list
        EXPECT_FALSE(mgr->isForbiddenNodeName(name));
    }
    EXPECT_TRUE(foundNode1);
    EXPECT_TRUE(foundNode2);
    EXPECT_TRUE(foundNode3);
}

TEST_F(ManagerHeadlessTest, IsForbiddenNodeName)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    // Forbidden names
    EXPECT_TRUE(mgr->isForbiddenNodeName("TPCameraChildSceneNode"));
    EXPECT_TRUE(mgr->isForbiddenNodeName("GridLine_node"));
    EXPECT_TRUE(mgr->isForbiddenNodeName(SELECTIONBOX_OBJECT_NAME));
    EXPECT_TRUE(mgr->isForbiddenNodeName(TRANSFORM_OBJECT_NAME));
    EXPECT_TRUE(mgr->isForbiddenNodeName("Unnamed_"));
    EXPECT_TRUE(mgr->isForbiddenNodeName("Unnamed_0"));
    EXPECT_TRUE(mgr->isForbiddenNodeName("Unnamed_camera"));

    // Non-forbidden names
    EXPECT_FALSE(mgr->isForbiddenNodeName("Cube"));
    EXPECT_FALSE(mgr->isForbiddenNodeName("Sphere"));
    EXPECT_FALSE(mgr->isForbiddenNodeName("MyObject"));
    EXPECT_FALSE(mgr->isForbiddenNodeName("TPCameraChildSceneNode_0"));
    EXPECT_FALSE(mgr->isForbiddenNodeName(""));
}

TEST_F(ManagerHeadlessTest, IsValidFileExtention)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    // Valid extensions
    QString meshFile = "model.mesh";
    EXPECT_TRUE(mgr->isValidFileExtention(meshFile));

    QString fbxFile = "character.fbx";
    EXPECT_TRUE(mgr->isValidFileExtention(fbxFile));

    QString objFile = "scene.obj";
    EXPECT_TRUE(mgr->isValidFileExtention(objFile));

    QString daeFile = "animation.dae";
    EXPECT_TRUE(mgr->isValidFileExtention(daeFile));

    QString gltfFile = "model.gltf";
    EXPECT_TRUE(mgr->isValidFileExtention(gltfFile));

    QString glbFile = "model.glb";
    EXPECT_TRUE(mgr->isValidFileExtention(glbFile));

    QString stlFile = "print.stl";
    EXPECT_TRUE(mgr->isValidFileExtention(stlFile));

    // Invalid extensions
    QString docFile = "readme.doc";
    EXPECT_FALSE(mgr->isValidFileExtention(docFile));

    QString pngFile = "texture.png";
    EXPECT_FALSE(mgr->isValidFileExtention(pngFile));

    QString cppFile = "source.cpp";
    EXPECT_FALSE(mgr->isValidFileExtention(cppFile));

    QString emptyFile = "";
    EXPECT_FALSE(mgr->isValidFileExtention(emptyFile));

    // getValidFileExtention should return a non-empty string
    QString validExts = mgr->getValidFileExtention();
    EXPECT_FALSE(validExts.isEmpty());
    EXPECT_TRUE(validExts.contains(".mesh"));
    EXPECT_TRUE(validExts.contains(".fbx"));
}

TEST_F(ManagerHeadlessTest, CreateEmptyScene)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    // Scene manager should already exist from headless init
    ASSERT_NE(mgr->getSceneMgr(), nullptr);
    ASSERT_NE(mgr->getRoot(), nullptr);

    // CreateEmptyScene adds a light and a viewport grid
    mgr->CreateEmptyScene();

    // After creating the scene, the viewport grid should be initialized
    EXPECT_NE(mgr->getViewportGrid(), nullptr);

    // Scene manager should still be valid
    EXPECT_NE(mgr->getSceneMgr(), nullptr);
}

TEST_F(ManagerHeadlessTest, GetEntities)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    // Initially no entities (no user-created objects exist)
    QList<Ogre::Entity*>& entities = mgr->getEntities();
    EXPECT_EQ(entities.count(), 0);
}

TEST_F(ManagerHeadlessTest, CreateEntity)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    // Create a primitive (cube), which internally creates a scene node and entity
    Ogre::SceneNode* cubeNode = PrimitiveObject::createCube("TestCube");
    ASSERT_NE(cubeNode, nullptr);
    EXPECT_TRUE(mgr->hasSceneNode("TestCube"));

    // After creating a primitive, there should be at least one entity
    QList<Ogre::Entity*>& entities = mgr->getEntities();
    EXPECT_GT(entities.count(), 0);

    // The scene nodes list should also include the cube node
    QList<Ogre::SceneNode*>& nodes = mgr->getSceneNodes();
    bool foundCube = false;
    for (Ogre::SceneNode* sn : nodes)
    {
        if (QString(sn->getName().c_str()) == "TestCube")
        {
            foundCube = true;
            break;
        }
    }
    EXPECT_TRUE(foundCube);
}

TEST_F(ManagerHeadlessTest, GetSceneNode_ExistingNode)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);
    Ogre::SceneNode* addedNode = mgr->addSceneNode("GetTestNode");
    ASSERT_NE(addedNode, nullptr);
    Ogre::SceneNode* retrievedNode = mgr->getSceneNode("GetTestNode");
    EXPECT_EQ(retrievedNode, addedNode);
}

TEST_F(ManagerHeadlessTest, GetSceneNode_NonExistentNode)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);
    Ogre::SceneNode* node = mgr->getSceneNode("NonExistentNode");
    EXPECT_EQ(node, nullptr);
}

TEST_F(ManagerHeadlessTest, DestroyAllAttachedMovableObjects)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);
    Ogre::SceneNode* cubeNode = PrimitiveObject::createCube("CubeForDestroy");
    ASSERT_NE(cubeNode, nullptr);
    EXPECT_GT(cubeNode->numAttachedObjects(), 0u);
    mgr->destroyAllAttachedMovableObjects(cubeNode);
    EXPECT_EQ(cubeNode->numAttachedObjects(), 0u);
}

TEST_F(ManagerHeadlessTest, AddSceneNode_WithOgreAny)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);
    Ogre::Any userData = Ogre::Any(std::string("TestUserData"));
    Ogre::SceneNode* node = mgr->addSceneNode("NodeWithAny", userData);
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(mgr->hasSceneNode("NodeWithAny"));
    EXPECT_TRUE(node->getUserObjectBindings().getUserAny().has_value());
}

TEST_F(ManagerHeadlessTest, DestroySceneNode_ByPointer)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);
    Ogre::SceneNode* node = mgr->addSceneNode("PointerDestroyNode");
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(mgr->hasSceneNode("PointerDestroyNode"));
    mgr->destroySceneNode(node);
    EXPECT_FALSE(mgr->hasSceneNode("PointerDestroyNode"));
}

TEST_F(ManagerHeadlessTest, SceneNodeCreated_Signal)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);
    QSignalSpy spy(mgr, &Manager::sceneNodeCreated);
    Ogre::SceneNode* node = mgr->addSceneNode("SignalTestNode");
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(ManagerHeadlessTest, SceneNodeDestroyed_Signal_ByName)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);
    Ogre::SceneNode* node = mgr->addSceneNode("DestroySignalNode");
    ASSERT_NE(node, nullptr);
    QSignalSpy spy(mgr, &Manager::sceneNodeDestroyed);
    mgr->destroySceneNode("DestroySignalNode");
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(ManagerHeadlessTest, SceneNodeDestroyed_Signal_ByPointer)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);
    Ogre::SceneNode* node = mgr->addSceneNode("DestroyPtrSignalNode");
    ASSERT_NE(node, nullptr);
    QSignalSpy spy(mgr, &Manager::sceneNodeDestroyed);
    mgr->destroySceneNode(node);
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(ManagerHeadlessTest, EntityCreated_Signal)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);
    QSignalSpy spy(mgr, &Manager::entityCreated);
    Ogre::SceneNode* cubeNode = PrimitiveObject::createCube("SignalEntityCube");
    ASSERT_NE(cubeNode, nullptr);
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(ManagerHeadlessTest, GetRoot_ReturnsNonNull)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);
    EXPECT_NE(mgr->getRoot(), nullptr);
}

TEST_F(ManagerHeadlessTest, GetSceneMgr_ReturnsNonNull)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);
    EXPECT_NE(mgr->getSceneMgr(), nullptr);
}

// ==========================================================================
// NEW: destroySceneNode error paths
// ==========================================================================

TEST_F(ManagerHeadlessTest, DestroySceneNodeByNameNotFound)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);
    // Destroying a non-existent node should not crash (catch path)
    mgr->destroySceneNode("NonExistentNode_XYZ_99999");
    // No assertion needed — just verify no crash
}

TEST_F(ManagerHeadlessTest, DestroySceneNodeNull)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);
    // Passing nullptr should be a no-op
    Ogre::SceneNode* nullNode = nullptr;
    mgr->destroySceneNode(nullNode);
    // No crash is the test
}

TEST_F(ManagerHeadlessTest, DestroySceneNodeForbiddenName)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);
    // Trying to destroy a forbidden node name should be a no-op
    mgr->destroySceneNode("GridLine_node");
    // No crash, and the forbidden node (if it existed) would remain
}

TEST_F(ManagerHeadlessTest, DestroySceneNodePrimitive)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    auto cubeNode = PrimitiveObject::createCube("PrimitiveDestroy");
    ASSERT_NE(cubeNode, nullptr);
    EXPECT_TRUE(PrimitiveObject::isPrimitive(cubeNode));
    EXPECT_TRUE(mgr->hasSceneNode("PrimitiveDestroy"));

    // Destroying a primitive should also clean up the PrimitiveObject
    mgr->destroySceneNode(cubeNode);
    EXPECT_FALSE(mgr->hasSceneNode("PrimitiveDestroy"));
}

// ==========================================================================
// NEW: destroyAllAttachedMovableObjects with null
// ==========================================================================

TEST_F(ManagerHeadlessTest, DestroyAllAttachedMovableObjectsNull)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);
    // Passing nullptr should be a no-op
    mgr->destroyAllAttachedMovableObjects(nullptr);
    // No crash is the test
}

// ==========================================================================
// NEW: addSceneNode auto-selects (not during scene init)
// ==========================================================================

TEST_F(ManagerHeadlessTest, AddSceneNodeAutoSelects)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    SelectionSet::getSingleton()->clear();
    EXPECT_TRUE(SelectionSet::getSingleton()->isEmpty());

    Ogre::SceneNode* node = mgr->addSceneNode("AutoSelectNode");
    ASSERT_NE(node, nullptr);

    // addSceneNode calls selectOne when not initializing scene
    EXPECT_TRUE(SelectionSet::getSingleton()->contains(node));
    EXPECT_EQ(SelectionSet::getSingleton()->getNodesCount(), 1);

    SelectionSet::getSingleton()->clear();
}

// ==========================================================================
// NEW: getMainWindow returns nullptr in headless mode
// ==========================================================================

TEST_F(ManagerHeadlessTest, GetMainWindowReturnsNull)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);
    // In headless test mode, MainWindow is not set
    EXPECT_EQ(mgr->getMainWindow(), nullptr);
}

// ==========================================================================
// NEW: getViewportGrid returns nullptr before CreateEmptyScene
// ==========================================================================

TEST_F(ManagerHeadlessTest, GetViewportGridNullBeforeCreateScene)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);
    // Before CreateEmptyScene, viewport grid should be null
    EXPECT_EQ(mgr->getViewportGrid(), nullptr);
}

// ==========================================================================
// NEW: destroySceneNode by name string — verify destroy and signal
// ==========================================================================

TEST_F(ManagerHeadlessTest, DestroySceneNodeByNameWithSignal)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    mgr->addSceneNode("SignalDestroyByName");
    ASSERT_TRUE(mgr->hasSceneNode("SignalDestroyByName"));

    QSignalSpy spy(mgr, &Manager::sceneNodeDestroyed);
    mgr->destroySceneNode("SignalDestroyByName");
    EXPECT_FALSE(mgr->hasSceneNode("SignalDestroyByName"));
    EXPECT_EQ(spy.count(), 1);
}

// ==========================================================================
// NEW: hasAnimationName with non-skeletal entity
// ==========================================================================

TEST_F(ManagerHeadlessTest, HasAnimationNameNoSkeleton)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    auto cubeNode = PrimitiveObject::createCube("AnimNameCube");
    ASSERT_FALSE(mgr->getEntities().isEmpty());
    // Filter by movable type to avoid casting ManualObjects to Entity
    Ogre::Entity* entity = nullptr;
    for (auto* obj : mgr->getEntities()) {
        if (obj->getMovableType() == "Entity") {
            entity = static_cast<Ogre::Entity*>(obj);
        }
    }
    ASSERT_NE(entity, nullptr);

    // Primitives have no skeleton, so hasAnimationName should return false
    EXPECT_FALSE(mgr->hasAnimationName(entity, "Walk"));
    EXPECT_FALSE(mgr->hasAnimationName(entity, ""));

    mgr->destroySceneNode(cubeNode);
}

// --- In-memory entity tests ---

TEST_F(ManagerHeadlessTest, CreateInMemoryEntity)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    auto mesh = createInMemoryTriangleMesh("MgrInMemTriangle");
    auto* sceneMgr = mgr->getSceneMgr();
    auto* node = mgr->addSceneNode("MgrInMemNode");
    auto* entity = sceneMgr->createEntity("MgrInMemEntity", mesh);
    node->attachObject(entity);

    EXPECT_TRUE(mgr->hasSceneNode("MgrInMemNode"));
    EXPECT_FALSE(mgr->getEntities().isEmpty());
}

TEST_F(ManagerHeadlessTest, CreateInMemorySkeletonEntity)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    auto mesh = createInMemorySkeletonMesh("MgrSkelMesh");
    auto* sceneMgr = mgr->getSceneMgr();
    auto* node = mgr->addSceneNode("MgrSkelNode");
    auto* entity = sceneMgr->createEntity("MgrSkelEntity", mesh);
    node->attachObject(entity);

    EXPECT_TRUE(entity->hasSkeleton());
    EXPECT_EQ(entity->getSkeleton()->getNumBones(), 2u);
}

TEST_F(ManagerHeadlessTest, CreateAnimatedEntity)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    auto* entity = createAnimatedTestEntity("MgrAnimEntity");
    ASSERT_NE(entity, nullptr);

    EXPECT_TRUE(entity->hasSkeleton());
    EXPECT_TRUE(entity->hasAnimationState("TestAnim"));

    auto* state = entity->getAnimationState("TestAnim");
    EXPECT_NEAR(state->getLength(), 1.0f, 0.01f);
}

TEST_F(ManagerHeadlessTest, HasAnimationNameWithSkeleton)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    auto* entity = createAnimatedTestEntity("MgrAnimNameEntity");
    ASSERT_NE(entity, nullptr);

    EXPECT_TRUE(mgr->hasAnimationName(entity, "TestAnim"));
    EXPECT_FALSE(mgr->hasAnimationName(entity, "NonExistentAnim"));
}

TEST_F(ManagerHeadlessTest, DestroyInMemoryEntity)
{
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: entity creation not supported without render window"; }
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    auto mesh = createInMemoryTriangleMesh("MgrDestroyTriangle");
    auto* sceneMgr = mgr->getSceneMgr();
    auto* node = mgr->addSceneNode("MgrDestroyNode");
    auto* entity = sceneMgr->createEntity("MgrDestroyEntity", mesh);
    node->attachObject(entity);

    ASSERT_TRUE(mgr->hasSceneNode("MgrDestroyNode"));
    mgr->destroySceneNode("MgrDestroyNode");
    EXPECT_FALSE(mgr->hasSceneNode("MgrDestroyNode"));
}

TEST_F(ManagerHeadlessTest, SceneNodeParenting)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    auto* parent = mgr->addSceneNode("ParentNode");
    ASSERT_NE(parent, nullptr);

    auto* child = mgr->getSceneMgr()->getRootSceneNode()->createChildSceneNode("ChildNode");
    ASSERT_NE(child, nullptr);

    // Move child under parent
    mgr->getSceneMgr()->getRootSceneNode()->removeChild(child);
    parent->addChild(child);

    EXPECT_EQ(child->getParentSceneNode(), parent);
    EXPECT_EQ(parent->numChildren(), 1u);
}
