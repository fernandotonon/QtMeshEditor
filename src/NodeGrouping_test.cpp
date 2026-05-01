#include <gtest/gtest.h>
#include "Manager.h"
#include "SelectionSet.h"
#include "UndoManager.h"
#include "commands/TransformCommands.h"
#include "TestHelpers.h"
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <Ogre.h>

class NodeGroupingTest : public ::testing::Test {
protected:
    void SetUp() override {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
    }

    void TearDown() override {
        if (app)
            app->processEvents();
    }

    QApplication* app = nullptr;
};

TEST_F(NodeGroupingTest, GroupNodes_Basic)
{
    ASSERT_TRUE(canLoadMeshFiles());

    auto* mgr = Manager::getSingleton();

    // Create two scene nodes with entities
    auto mesh1 = createInMemoryTriangleMesh("grouptest_mesh1");
    auto* node1 = mgr->addSceneNode("GroupTestNode1");
    mgr->createEntity(node1, mesh1);
    node1->setPosition(10, 0, 0);

    auto mesh2 = createInMemoryTriangleMesh("grouptest_mesh2");
    auto* node2 = mgr->addSceneNode("GroupTestNode2");
    mgr->createEntity(node2, mesh2);
    node2->setPosition(-10, 0, 0);

    QList<Ogre::SceneNode*> nodes = {node1, node2};
    Ogre::SceneNode* groupNode = mgr->groupNodes(nodes);

    ASSERT_NE(groupNode, nullptr);

    // Group node should be parent of both nodes
    EXPECT_EQ(node1->getParent(), groupNode);
    EXPECT_EQ(node2->getParent(), groupNode);

    // Group node should have 2 children
    EXPECT_EQ(groupNode->numChildren(), 2u);

    // Group node should have no attached objects
    EXPECT_EQ(groupNode->numAttachedObjects(), 0u);

    // isGroupNode should return true
    EXPECT_TRUE(mgr->isGroupNode(groupNode));

    // World positions should be preserved (approximately)
    Ogre::Vector3 world1 = node1->_getDerivedPosition();
    Ogre::Vector3 world2 = node2->_getDerivedPosition();
    EXPECT_NEAR(world1.x, 10.0, 0.001);
    EXPECT_NEAR(world2.x, -10.0, 0.001);
}

TEST_F(NodeGroupingTest, UngroupNode)
{
    ASSERT_TRUE(canLoadMeshFiles());

    auto* mgr = Manager::getSingleton();

    auto mesh1 = createInMemoryTriangleMesh("ungroup_mesh1");
    auto* node1 = mgr->addSceneNode("UngroupNode1");
    mgr->createEntity(node1, mesh1);
    node1->setPosition(5, 5, 0);

    auto mesh2 = createInMemoryTriangleMesh("ungroup_mesh2");
    auto* node2 = mgr->addSceneNode("UngroupNode2");
    mgr->createEntity(node2, mesh2);
    node2->setPosition(-5, -5, 0);

    // Group them
    QList<Ogre::SceneNode*> nodes = {node1, node2};
    Ogre::SceneNode* groupNode = mgr->groupNodes(nodes);
    ASSERT_NE(groupNode, nullptr);

    // Now ungroup
    mgr->ungroupNode(groupNode);

    // Nodes should be back under root scene node
    auto* rootNode = mgr->getSceneMgr()->getRootSceneNode();
    EXPECT_EQ(node1->getParent(), rootNode);
    EXPECT_EQ(node2->getParent(), rootNode);

    // World positions should be preserved
    Ogre::Vector3 world1 = node1->_getDerivedPosition();
    Ogre::Vector3 world2 = node2->_getDerivedPosition();
    EXPECT_NEAR(world1.x, 5.0, 0.001);
    EXPECT_NEAR(world1.y, 5.0, 0.001);
    EXPECT_NEAR(world2.x, -5.0, 0.001);
    EXPECT_NEAR(world2.y, -5.0, 0.001);
}

TEST_F(NodeGroupingTest, IsGroupNode)
{
    auto* mgr = Manager::getSingleton();

    // A node with no children and no objects is not a group
    auto* emptyNode = mgr->addSceneNode("NotAGroup");
    EXPECT_FALSE(mgr->isGroupNode(emptyNode));

    // nullptr is not a group
    EXPECT_FALSE(mgr->isGroupNode(nullptr));
}

TEST_F(NodeGroupingTest, GroupTransformPropagation)
{
    ASSERT_TRUE(canLoadMeshFiles());

    auto* mgr = Manager::getSingleton();

    auto mesh = createInMemoryTriangleMesh("propagation_mesh");
    auto* node1 = mgr->addSceneNode("PropNode1");
    mgr->createEntity(node1, mesh);
    node1->setPosition(0, 0, 0);

    auto mesh2 = createInMemoryTriangleMesh("propagation_mesh2");
    auto* node2 = mgr->addSceneNode("PropNode2");
    mgr->createEntity(node2, mesh2);
    node2->setPosition(10, 0, 0);

    QList<Ogre::SceneNode*> nodes = {node1, node2};
    Ogre::SceneNode* groupNode = mgr->groupNodes(nodes);
    ASSERT_NE(groupNode, nullptr);

    // Move the group
    groupNode->translate(100, 0, 0);

    // Children should have moved as well (world positions)
    Ogre::Vector3 world1 = node1->_getDerivedPosition();
    Ogre::Vector3 world2 = node2->_getDerivedPosition();
    EXPECT_NEAR(world1.x, 100.0, 0.001);
    EXPECT_NEAR(world2.x, 110.0, 0.001);
}

TEST_F(NodeGroupingTest, NestedGroups)
{
    ASSERT_TRUE(canLoadMeshFiles());

    auto* mgr = Manager::getSingleton();

    auto mesh1 = createInMemoryTriangleMesh("nested_mesh1");
    auto* node1 = mgr->addSceneNode("NestedNode1");
    mgr->createEntity(node1, mesh1);
    node1->setPosition(0, 0, 0);

    auto mesh2 = createInMemoryTriangleMesh("nested_mesh2");
    auto* node2 = mgr->addSceneNode("NestedNode2");
    mgr->createEntity(node2, mesh2);
    node2->setPosition(10, 0, 0);

    auto mesh3 = createInMemoryTriangleMesh("nested_mesh3");
    auto* node3 = mgr->addSceneNode("NestedNode3");
    mgr->createEntity(node3, mesh3);
    node3->setPosition(20, 0, 0);

    // Group node1 and node2
    QList<Ogre::SceneNode*> innerNodes = {node1, node2};
    Ogre::SceneNode* innerGroup = mgr->groupNodes(innerNodes);
    ASSERT_NE(innerGroup, nullptr);

    // Group the inner group with node3
    QList<Ogre::SceneNode*> outerNodes = {innerGroup, node3};
    Ogre::SceneNode* outerGroup = mgr->groupNodes(outerNodes);
    ASSERT_NE(outerGroup, nullptr);

    // Verify nesting
    EXPECT_EQ(innerGroup->getParent(), outerGroup);
    EXPECT_EQ(node3->getParent(), outerGroup);
    EXPECT_EQ(node1->getParent(), innerGroup);
    EXPECT_EQ(node2->getParent(), innerGroup);

    // Both levels should be recognized as groups
    EXPECT_TRUE(mgr->isGroupNode(innerGroup));
    EXPECT_TRUE(mgr->isGroupNode(outerGroup));
}

TEST_F(NodeGroupingTest, GetSceneNodesIncludesGroupChildren)
{
    ASSERT_TRUE(canLoadMeshFiles());

    auto* mgr = Manager::getSingleton();

    auto mesh1 = createInMemoryTriangleMesh("scenenodes_mesh1");
    auto* node1 = mgr->addSceneNode("SceneNodesNode1");
    mgr->createEntity(node1, mesh1);

    auto mesh2 = createInMemoryTriangleMesh("scenenodes_mesh2");
    auto* node2 = mgr->addSceneNode("SceneNodesNode2");
    mgr->createEntity(node2, mesh2);

    QList<Ogre::SceneNode*> nodes = {node1, node2};
    Ogre::SceneNode* groupNode = mgr->groupNodes(nodes);
    ASSERT_NE(groupNode, nullptr);

    // getSceneNodes should include the group AND its children
    auto& allNodes = mgr->getSceneNodes();
    bool foundGroup = false, foundNode1 = false, foundNode2 = false;
    for (auto* n : allNodes) {
        if (n == groupNode) foundGroup = true;
        if (n == node1) foundNode1 = true;
        if (n == node2) foundNode2 = true;
    }
    EXPECT_TRUE(foundGroup);
    EXPECT_TRUE(foundNode1);
    EXPECT_TRUE(foundNode2);
}

TEST_F(NodeGroupingTest, GetEntitiesIncludesGroupedEntities)
{
    ASSERT_TRUE(canLoadMeshFiles());

    auto* mgr = Manager::getSingleton();

    auto mesh1 = createInMemoryTriangleMesh("entities_mesh1");
    auto* node1 = mgr->addSceneNode("EntitiesNode1");
    auto* ent1 = mgr->createEntity(node1, mesh1);

    auto mesh2 = createInMemoryTriangleMesh("entities_mesh2");
    auto* node2 = mgr->addSceneNode("EntitiesNode2");
    auto* ent2 = mgr->createEntity(node2, mesh2);

    QList<Ogre::SceneNode*> nodes = {node1, node2};
    mgr->groupNodes(nodes);

    // getEntities should include entities inside groups
    auto& allEntities = mgr->getEntities();
    bool foundEnt1 = false, foundEnt2 = false;
    for (auto* e : allEntities) {
        if (e == ent1) foundEnt1 = true;
        if (e == ent2) foundEnt2 = true;
    }
    EXPECT_TRUE(foundEnt1);
    EXPECT_TRUE(foundEnt2);
}

TEST_F(NodeGroupingTest, HasSceneNodeFindsNestedNodes)
{
    ASSERT_TRUE(canLoadMeshFiles());

    auto* mgr = Manager::getSingleton();

    auto mesh = createInMemoryTriangleMesh("hasnested_mesh");
    auto* node = mgr->addSceneNode("HasNestedNode");
    mgr->createEntity(node, mesh);

    auto mesh2 = createInMemoryTriangleMesh("hasnested_mesh2");
    auto* node2 = mgr->addSceneNode("HasNestedNode2");
    mgr->createEntity(node2, mesh2);

    QList<Ogre::SceneNode*> nodes = {node, node2};
    mgr->groupNodes(nodes);

    // hasSceneNode should find nodes inside groups
    EXPECT_TRUE(mgr->hasSceneNode("HasNestedNode"));
    EXPECT_TRUE(mgr->hasSceneNode("HasNestedNode2"));
}
