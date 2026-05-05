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

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
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

    // Empty/unnamed are forbidden
    EXPECT_TRUE(mgr->isForbiddenNodeName(""));

    // Non-forbidden names
    EXPECT_FALSE(mgr->isForbiddenNodeName("Cube"));
    EXPECT_FALSE(mgr->isForbiddenNodeName("Sphere"));
    EXPECT_FALSE(mgr->isForbiddenNodeName("MyObject"));
    EXPECT_FALSE(mgr->isForbiddenNodeName("TPCameraChildSceneNode_0"));
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

    QString vrmFile = "avatar.vrm";
    EXPECT_TRUE(mgr->isValidFileExtention(vrmFile));

    QString stlFile = "print.stl";
    EXPECT_TRUE(mgr->isValidFileExtention(stlFile));

    QString tmdFile = "model.tmd";
    EXPECT_TRUE(mgr->isValidFileExtention(tmdFile));
    EXPECT_TRUE(mgr->isValidFileExtention(QStringLiteral("CAR.TMD")));

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
    EXPECT_TRUE(validExts.contains(".vrm"));
    EXPECT_TRUE(validExts.contains(".tmd"));
}

TEST_F(ManagerHeadlessTest, CreateEmptyScene)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
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
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
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
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
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
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
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
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
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
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
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
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
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
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
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
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
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
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    auto* entity = createAnimatedTestEntity("MgrAnimNameEntity");
    ASSERT_NE(entity, nullptr);

    EXPECT_TRUE(mgr->hasAnimationName(entity, "TestAnim"));
    EXPECT_FALSE(mgr->hasAnimationName(entity, "NonExistentAnim"));
}

TEST_F(ManagerHeadlessTest, DestroyInMemoryEntity)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
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

// ==========================================================================
// NEW BATCH: Additional coverage tests
// ==========================================================================

// Test clearScene-like behavior: destroy all user nodes and verify cleanup
TEST_F(ManagerHeadlessTest, DestroyAllUserNodes_ClearsScene)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    // Create several nodes with entities
    auto mesh1 = createInMemoryTriangleMesh("ClearSceneMesh1");
    auto mesh2 = createInMemoryTriangleMesh("ClearSceneMesh2");
    auto* sceneMgr = mgr->getSceneMgr();

    auto* node1 = mgr->addSceneNode("ClearNode1");
    auto* ent1 = sceneMgr->createEntity("ClearEntity1", mesh1);
    node1->attachObject(ent1);

    auto* node2 = mgr->addSceneNode("ClearNode2");
    auto* ent2 = sceneMgr->createEntity("ClearEntity2", mesh2);
    node2->attachObject(ent2);

    // Also add a plain node with no entity
    mgr->addSceneNode("ClearNodeEmpty");

    ASSERT_TRUE(mgr->hasSceneNode("ClearNode1"));
    ASSERT_TRUE(mgr->hasSceneNode("ClearNode2"));
    ASSERT_TRUE(mgr->hasSceneNode("ClearNodeEmpty"));
    ASSERT_FALSE(mgr->getEntities().isEmpty());

    // Destroy all user nodes one-by-one (mimics clearScene for user content)
    QList<Ogre::SceneNode*> nodesToDestroy;
    for (auto* sn : mgr->getSceneNodes()) {
        nodesToDestroy.append(sn);
    }
    for (auto* sn : nodesToDestroy) {
        mgr->destroySceneNode(sn);
    }

    // Verify everything is cleaned up
    EXPECT_EQ(mgr->getSceneNodes().count(), 0);
    EXPECT_EQ(mgr->getEntities().count(), 0);
    EXPECT_FALSE(mgr->hasSceneNode("ClearNode1"));
    EXPECT_FALSE(mgr->hasSceneNode("ClearNode2"));
    EXPECT_FALSE(mgr->hasSceneNode("ClearNodeEmpty"));
}

// Test getEntities with ManualObjects mixed in -- verifies the type-filtering pitfall
// Manager::getEntities() does static_cast<Entity*> without checking movableType,
// so attaching a ManualObject to a user node would cause issues. This test
// verifies that ManualObjects attached to forbidden-name nodes are filtered out
// by the isForbiddenNodeName check in getEntities().
TEST_F(ManagerHeadlessTest, GetEntitiesWithManualObjectOnForbiddenNode)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);
    auto* sceneMgr = mgr->getSceneMgr();

    // Create a ManualObject on the root (but under a forbidden-name node).
    // getEntities filters out forbidden nodes, so this ManualObject should not appear.
    auto* manualObj = sceneMgr->createManualObject("TestManual_Unnamed");
    // Attach to a node that starts with "Unnamed_" (forbidden)
    auto* forbiddenNode = sceneMgr->getRootSceneNode()->createChildSceneNode("Unnamed_manual_test");
    forbiddenNode->attachObject(manualObj);

    // getEntities should not crash and should return 0 entities
    // (the forbidden node is filtered out, so the ManualObject cast never happens)
    QList<Ogre::Entity*>& entities = mgr->getEntities();
    EXPECT_EQ(entities.count(), 0);

    // Cleanup
    forbiddenNode->detachAllObjects();
    sceneMgr->destroyManualObject(manualObj);
    sceneMgr->destroySceneNode(forbiddenNode);
}

// Test duplicate node name auto-numbering with many collisions
TEST_F(ManagerHeadlessTest, AddSceneNode_AutoNumbering_ManyDuplicates)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    // Create 5 nodes with the same base name "Sphere"
    Ogre::SceneNode* nodes[5];
    nodes[0] = mgr->addSceneNode("Sphere");
    nodes[1] = mgr->addSceneNode("Sphere");
    nodes[2] = mgr->addSceneNode("Sphere");
    nodes[3] = mgr->addSceneNode("Sphere");
    nodes[4] = mgr->addSceneNode("Sphere");

    // First gets exact name, subsequent get Sphere1..Sphere4
    EXPECT_TRUE(mgr->hasSceneNode("Sphere"));
    EXPECT_TRUE(mgr->hasSceneNode("Sphere1"));
    EXPECT_TRUE(mgr->hasSceneNode("Sphere2"));
    EXPECT_TRUE(mgr->hasSceneNode("Sphere3"));
    EXPECT_TRUE(mgr->hasSceneNode("Sphere4"));

    // All nodes must be distinct
    for (int i = 0; i < 5; ++i) {
        ASSERT_NE(nodes[i], nullptr);
        for (int j = i + 1; j < 5; ++j) {
            EXPECT_NE(nodes[i], nodes[j]) << "Nodes at index " << i << " and " << j << " should differ";
        }
    }
}

// Test auto-numbering when middle names are missing (gap filling)
TEST_F(ManagerHeadlessTest, AddSceneNode_AutoNumbering_WithGap)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    // Create "Plane" and "Plane1", then destroy "Plane"
    auto* n0 = mgr->addSceneNode("Plane");  // "Plane"
    auto* n1 = mgr->addSceneNode("Plane");  // "Plane1"
    ASSERT_TRUE(mgr->hasSceneNode("Plane"));
    ASSERT_TRUE(mgr->hasSceneNode("Plane1"));

    mgr->destroySceneNode("Plane");
    EXPECT_FALSE(mgr->hasSceneNode("Plane"));
    EXPECT_TRUE(mgr->hasSceneNode("Plane1"));

    // Adding "Plane" again should reuse the name "Plane" (the gap)
    auto* n2 = mgr->addSceneNode("Plane");
    ASSERT_NE(n2, nullptr);
    EXPECT_TRUE(mgr->hasSceneNode("Plane"));
    EXPECT_TRUE(mgr->hasSceneNode("Plane1"));
}

TEST_F(ManagerHeadlessTest, DuplicateSceneNode_NullSourceReturnsNull)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    EXPECT_EQ(mgr->duplicateSceneNode(nullptr), nullptr);
}

TEST_F(ManagerHeadlessTest, DuplicateSceneNode_ClonesAnimatedEntityWithIndependentSkeleton)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    Ogre::Entity* sourceEntity = createAnimatedTestEntity("MgrDupAnim");
    ASSERT_NE(sourceEntity, nullptr);
    ASSERT_TRUE(sourceEntity->hasSkeleton());
    Ogre::SceneNode* sourceNode = sourceEntity->getParentSceneNode();
    ASSERT_NE(sourceNode, nullptr);

    sourceNode->setPosition(3.0f, 4.0f, 5.0f);
    sourceNode->setScale(1.5f, 2.0f, 2.5f);
    sourceNode->setOrientation(Ogre::Quaternion(Ogre::Degree(20.0f), Ogre::Vector3::UNIT_Y));

    Ogre::SceneNode* duplicateNode = mgr->duplicateSceneNode(sourceNode);
    ASSERT_NE(duplicateNode, nullptr);
    EXPECT_NE(duplicateNode, sourceNode);
    EXPECT_NE(QString::fromStdString(duplicateNode->getName()), QString::fromStdString(sourceNode->getName()));

    EXPECT_FLOAT_EQ(duplicateNode->getPosition().x, sourceNode->getPosition().x);
    EXPECT_FLOAT_EQ(duplicateNode->getPosition().y, sourceNode->getPosition().y);
    EXPECT_FLOAT_EQ(duplicateNode->getPosition().z, sourceNode->getPosition().z);
    EXPECT_FLOAT_EQ(duplicateNode->getScale().x, sourceNode->getScale().x);
    EXPECT_FLOAT_EQ(duplicateNode->getScale().y, sourceNode->getScale().y);
    EXPECT_FLOAT_EQ(duplicateNode->getScale().z, sourceNode->getScale().z);
    EXPECT_NEAR(duplicateNode->getOrientation().w, sourceNode->getOrientation().w, 1e-5f);
    EXPECT_NEAR(duplicateNode->getOrientation().x, sourceNode->getOrientation().x, 1e-5f);
    EXPECT_NEAR(duplicateNode->getOrientation().y, sourceNode->getOrientation().y, 1e-5f);
    EXPECT_NEAR(duplicateNode->getOrientation().z, sourceNode->getOrientation().z, 1e-5f);

    ASSERT_EQ(duplicateNode->numAttachedObjects(), 1u);
    Ogre::MovableObject* attached = duplicateNode->getAttachedObject(0);
    ASSERT_NE(attached, nullptr);
    ASSERT_EQ(attached->getMovableType(), "Entity");
    Ogre::Entity* duplicateEntity = static_cast<Ogre::Entity*>(attached);
    ASSERT_NE(duplicateEntity, nullptr);

    EXPECT_NE(duplicateEntity->getMesh()->getName(), sourceEntity->getMesh()->getName());
    ASSERT_TRUE(sourceEntity->getMesh()->hasSkeleton());
    ASSERT_TRUE(duplicateEntity->getMesh()->hasSkeleton());
    EXPECT_NE(duplicateEntity->getMesh()->getSkeleton()->getName(),
              sourceEntity->getMesh()->getSkeleton()->getName());
    EXPECT_TRUE(duplicateEntity->getAllAnimationStates()->hasAnimationState("TestAnim"));

    Ogre::AnimationState* sourceState = sourceEntity->getAnimationState("TestAnim");
    Ogre::AnimationState* duplicateState = duplicateEntity->getAnimationState("TestAnim");
    ASSERT_NE(sourceState, nullptr);
    ASSERT_NE(duplicateState, nullptr);
    EXPECT_FALSE(sourceState->getEnabled());
    EXPECT_FALSE(duplicateState->getEnabled());

    sourceState->setEnabled(true);
    EXPECT_TRUE(sourceState->getEnabled());
    EXPECT_FALSE(duplicateState->getEnabled());

    Ogre::Animation* sourceAnim = sourceEntity->getMesh()->getSkeleton()->getAnimation("TestAnim");
    Ogre::Animation* duplicateAnim = duplicateEntity->getMesh()->getSkeleton()->getAnimation("TestAnim");
    ASSERT_NE(sourceAnim, nullptr);
    ASSERT_NE(duplicateAnim, nullptr);
    EXPECT_FLOAT_EQ(sourceAnim->getLength(), duplicateAnim->getLength());
    EXPECT_EQ(sourceAnim->getNumNodeTracks(), duplicateAnim->getNumNodeTracks());
}

// Test hasAnimationName with createAnimatedTestEntity - multiple animation name checks
TEST_F(ManagerHeadlessTest, HasAnimationName_MultipleChecks)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    auto* entity = createAnimatedTestEntity("AnimMultiCheck");
    ASSERT_NE(entity, nullptr);
    ASSERT_TRUE(entity->hasSkeleton());

    // The helper creates a "TestAnim" animation
    EXPECT_TRUE(mgr->hasAnimationName(entity, "TestAnim"));

    // Non-existent animation names
    EXPECT_FALSE(mgr->hasAnimationName(entity, "Walk"));
    EXPECT_FALSE(mgr->hasAnimationName(entity, "Run"));
    EXPECT_FALSE(mgr->hasAnimationName(entity, "Idle"));
    EXPECT_FALSE(mgr->hasAnimationName(entity, ""));
    EXPECT_FALSE(mgr->hasAnimationName(entity, "testAnim")); // case-sensitive
}

// Test scene node parent-child chain after multiple addSceneNode calls
TEST_F(ManagerHeadlessTest, SceneNodeParentChain_MultipleDepth)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);
    auto* sceneMgr = mgr->getSceneMgr();
    auto* root = sceneMgr->getRootSceneNode();

    // addSceneNode always creates children of root
    auto* a = mgr->addSceneNode("ChainA");
    auto* b = mgr->addSceneNode("ChainB");
    auto* c = mgr->addSceneNode("ChainC");

    // All are children of root by default
    EXPECT_EQ(a->getParentSceneNode(), root);
    EXPECT_EQ(b->getParentSceneNode(), root);
    EXPECT_EQ(c->getParentSceneNode(), root);

    // Now reparent: A -> B -> C
    root->removeChild(b);
    a->addChild(b);
    root->removeChild(c);
    b->addChild(c);

    // Verify the chain
    EXPECT_EQ(c->getParentSceneNode(), b);
    EXPECT_EQ(b->getParentSceneNode(), a);
    EXPECT_EQ(a->getParentSceneNode(), root);

    // C's derived position should accumulate the parent chain
    a->setPosition(10, 0, 0);
    b->setPosition(0, 20, 0);
    c->setPosition(0, 0, 30);

    // Force updates
    a->_update(true, false);

    Ogre::Vector3 derivedPos = c->_getDerivedPosition();
    EXPECT_NEAR(derivedPos.x, 10.0f, 0.01f);
    EXPECT_NEAR(derivedPos.y, 20.0f, 0.01f);
    EXPECT_NEAR(derivedPos.z, 30.0f, 0.01f);
}

// Test createEntity via Manager::createEntity helper (not manual attach)
TEST_F(ManagerHeadlessTest, CreateEntityViaManagerHelper)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    auto mesh = createInMemoryTriangleMesh("MgrHelperMesh");
    auto* node = mgr->addSceneNode("MgrHelperNode");
    ASSERT_NE(node, nullptr);

    QSignalSpy spy(mgr, &Manager::entityCreated);
    auto* entity = mgr->createEntity(node, mesh);
    ASSERT_NE(entity, nullptr);
    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(node->numAttachedObjects(), 1u);
    EXPECT_FALSE(mgr->getEntities().isEmpty());
}

// Test destroying a node with children recursively
TEST_F(ManagerHeadlessTest, DestroySceneNode_WithChildren)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);
    auto* sceneMgr = mgr->getSceneMgr();

    auto* parent = mgr->addSceneNode("DestroyParent");
    ASSERT_NE(parent, nullptr);

    // Create child nodes directly under parent
    auto* child1 = parent->createChildSceneNode("DestroyChild1");
    auto* child2 = parent->createChildSceneNode("DestroyChild2");
    ASSERT_NE(child1, nullptr);
    ASSERT_NE(child2, nullptr);

    EXPECT_EQ(parent->numChildren(), 2u);

    // Destroying parent should also destroy children (removeAndDestroyAllChildren)
    mgr->destroySceneNode(parent);
    EXPECT_FALSE(mgr->hasSceneNode("DestroyParent"));
}

// Test getSceneNodes does not include forbidden-name nodes
TEST_F(ManagerHeadlessTest, GetSceneNodes_ExcludesForbiddenNames)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    mgr->addSceneNode("VisibleNode");

    QList<Ogre::SceneNode*>& nodes = mgr->getSceneNodes();
    for (auto* sn : nodes) {
        QString name = sn->getName().c_str();
        EXPECT_FALSE(name.startsWith("Unnamed_")) << "Found forbidden node: " << name.toStdString();
        EXPECT_NE(name, QString("TPCameraChildSceneNode"));
        EXPECT_NE(name, QString("GridLine_node"));
        EXPECT_NE(name, QString(SELECTIONBOX_OBJECT_NAME));
        EXPECT_NE(name, QString(TRANSFORM_OBJECT_NAME));
    }
}

// Test hasSceneNode with empty and whitespace names
TEST_F(ManagerHeadlessTest, HasSceneNode_EdgeCaseNames)
{
    auto* mgr = Manager::getSingletonPtr();
    ASSERT_NE(mgr, nullptr);

    EXPECT_FALSE(mgr->hasSceneNode(""));
    EXPECT_FALSE(mgr->hasSceneNode("   "));
    EXPECT_FALSE(mgr->hasSceneNode("nonexistent_12345"));

    // A node with spaces in the name should work
    auto* node = mgr->addSceneNode("Node With Spaces");
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(mgr->hasSceneNode("Node With Spaces"));
}
