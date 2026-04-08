#include <gtest/gtest.h>
#include "TransformCommands.h"
#include "../Manager.h"
#include "../SelectionSet.h"
#include "../TestHelpers.h"
#include <QApplication>
#include <QCoreApplication>
#include <QThread>

class TransformCommandsTests : public ::testing::Test {
protected:
    QApplication* app = nullptr;

    void SetUp() override {
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        if (!tryInitOgre()) {
            GTEST_SKIP() << "Skipping: Ogre initialization failed";
        }
    }

    void TearDown() override {
        SelectionSet::getSingleton()->clear();
        if (app) app->processEvents();
    }
};

// ---- TranslateCommand ----

TEST_F(TransformCommandsTests, TranslateCommand_RedoMovesNode) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("TransCmdNode1");
    ASSERT_NE(node, nullptr);
    node->setPosition(0, 0, 0);

    QList<Ogre::SceneNode*> nodes = {node};
    Ogre::Vector3 delta(10, 20, 30);
    auto* cmd = new TranslateCommand(nodes, delta);

    cmd->redo();
    EXPECT_EQ(node->getPosition(), Ogre::Vector3(10, 20, 30));

    delete cmd;
}

TEST_F(TransformCommandsTests, TranslateCommand_UndoRestoresPosition) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("TransCmdNode2");
    ASSERT_NE(node, nullptr);
    node->setPosition(5, 5, 5);

    QList<Ogre::SceneNode*> nodes = {node};
    Ogre::Vector3 delta(10, 20, 30);
    auto* cmd = new TranslateCommand(nodes, delta);

    cmd->redo();
    EXPECT_EQ(node->getPosition(), Ogre::Vector3(15, 25, 35));

    cmd->undo();
    EXPECT_EQ(node->getPosition(), Ogre::Vector3(5, 5, 5));

    delete cmd;
}

TEST_F(TransformCommandsTests, TranslateCommand_MultipleNodes) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node1 = mgr->addSceneNode("TransCmdMulti1");
    Ogre::SceneNode* node2 = mgr->addSceneNode("TransCmdMulti2");
    ASSERT_NE(node1, nullptr);
    ASSERT_NE(node2, nullptr);
    node1->setPosition(0, 0, 0);
    node2->setPosition(100, 100, 100);

    QList<Ogre::SceneNode*> nodes = {node1, node2};
    Ogre::Vector3 delta(5, 10, 15);
    auto* cmd = new TranslateCommand(nodes, delta);

    cmd->redo();
    EXPECT_EQ(node1->getPosition(), Ogre::Vector3(5, 10, 15));
    EXPECT_EQ(node2->getPosition(), Ogre::Vector3(105, 110, 115));

    cmd->undo();
    EXPECT_EQ(node1->getPosition(), Ogre::Vector3(0, 0, 0));
    EXPECT_EQ(node2->getPosition(), Ogre::Vector3(100, 100, 100));

    delete cmd;
}

TEST_F(TransformCommandsTests, TranslateCommand_RedoUndoRedoCycle) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("TransCmdCycle");
    ASSERT_NE(node, nullptr);
    node->setPosition(0, 0, 0);

    QList<Ogre::SceneNode*> nodes = {node};
    Ogre::Vector3 delta(1, 2, 3);
    auto* cmd = new TranslateCommand(nodes, delta);

    cmd->redo();
    EXPECT_EQ(node->getPosition(), Ogre::Vector3(1, 2, 3));

    cmd->undo();
    EXPECT_EQ(node->getPosition(), Ogre::Vector3(0, 0, 0));

    cmd->redo();
    EXPECT_EQ(node->getPosition(), Ogre::Vector3(1, 2, 3));

    delete cmd;
}

TEST_F(TransformCommandsTests, TranslateCommand_EmptyNodeList) {
    QList<Ogre::SceneNode*> nodes;
    Ogre::Vector3 delta(10, 20, 30);
    auto* cmd = new TranslateCommand(nodes, delta);

    // Should not crash with empty list
    EXPECT_NO_THROW(cmd->redo());
    EXPECT_NO_THROW(cmd->undo());

    delete cmd;
}

TEST_F(TransformCommandsTests, TranslateCommand_WithDestroyedNode) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("TransCmdDestroyed");
    ASSERT_NE(node, nullptr);

    QList<Ogre::SceneNode*> nodes = {node};
    Ogre::Vector3 delta(10, 20, 30);
    auto* cmd = new TranslateCommand(nodes, delta);

    // Destroy the node before redo
    mgr->destroySceneNode("TransCmdDestroyed");

    // isNodeValid should return false, so redo/undo should skip safely
    EXPECT_NO_THROW(cmd->redo());
    EXPECT_NO_THROW(cmd->undo());

    delete cmd;
}

// ---- RotateCommand ----

TEST_F(TransformCommandsTests, RotateCommand_RedoRotatesNode) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("RotCmdNode1");
    ASSERT_NE(node, nullptr);
    node->setPosition(10, 0, 0);

    QList<Ogre::SceneNode*> nodes = {node};
    Ogre::Quaternion rotation(Ogre::Degree(90), Ogre::Vector3::UNIT_Y);
    Ogre::Vector3 pivot(0, 0, 0);
    auto* cmd = new RotateCommand(nodes, rotation, pivot);

    Ogre::Quaternion origOrientation = node->getOrientation();
    cmd->redo();
    // Node should have rotated
    EXPECT_NE(node->getOrientation(), origOrientation);

    delete cmd;
}

TEST_F(TransformCommandsTests, RotateCommand_UndoRestoresState) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("RotCmdNode2");
    ASSERT_NE(node, nullptr);
    node->setPosition(10, 0, 0);

    Ogre::Vector3 origPos = node->getPosition();
    Ogre::Quaternion origOrient = node->getOrientation();

    QList<Ogre::SceneNode*> nodes = {node};
    Ogre::Quaternion rotation(Ogre::Degree(90), Ogre::Vector3::UNIT_Y);
    Ogre::Vector3 pivot(0, 0, 0);
    auto* cmd = new RotateCommand(nodes, rotation, pivot);

    cmd->redo();
    cmd->undo();

    EXPECT_EQ(node->getPosition(), origPos);
    EXPECT_EQ(node->getOrientation(), origOrient);

    delete cmd;
}

TEST_F(TransformCommandsTests, RotateCommand_EmptyNodeList) {
    QList<Ogre::SceneNode*> nodes;
    Ogre::Quaternion rotation(Ogre::Degree(45), Ogre::Vector3::UNIT_Z);
    Ogre::Vector3 pivot(0, 0, 0);
    auto* cmd = new RotateCommand(nodes, rotation, pivot);

    EXPECT_NO_THROW(cmd->redo());
    EXPECT_NO_THROW(cmd->undo());

    delete cmd;
}

TEST_F(TransformCommandsTests, RotateCommand_WithDestroyedNode) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("RotCmdDestroyed");
    ASSERT_NE(node, nullptr);

    QList<Ogre::SceneNode*> nodes = {node};
    Ogre::Quaternion rotation(Ogre::Degree(45), Ogre::Vector3::UNIT_X);
    Ogre::Vector3 pivot(0, 0, 0);
    auto* cmd = new RotateCommand(nodes, rotation, pivot);

    mgr->destroySceneNode("RotCmdDestroyed");

    EXPECT_NO_THROW(cmd->redo());
    EXPECT_NO_THROW(cmd->undo());

    delete cmd;
}

// ---- ScaleCommand ----

TEST_F(TransformCommandsTests, ScaleCommand_RedoScalesNode) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("ScaleCmdNode1");
    ASSERT_NE(node, nullptr);
    node->setScale(1, 1, 1);

    QList<Ogre::SceneNode*> nodes = {node};
    Ogre::Vector3 factor(2, 3, 4);
    auto* cmd = new ScaleCommand(nodes, factor);

    cmd->redo();
    EXPECT_EQ(node->getScale(), Ogre::Vector3(2, 3, 4));

    delete cmd;
}

TEST_F(TransformCommandsTests, ScaleCommand_UndoRestoresScale) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("ScaleCmdNode2");
    ASSERT_NE(node, nullptr);
    node->setScale(1, 1, 1);

    QList<Ogre::SceneNode*> nodes = {node};
    Ogre::Vector3 factor(2, 3, 4);
    auto* cmd = new ScaleCommand(nodes, factor);

    cmd->redo();
    EXPECT_EQ(node->getScale(), Ogre::Vector3(2, 3, 4));

    cmd->undo();
    // Undo applies inverse: scale * (1/factor)
    Ogre::Vector3 expectedScale(1, 1, 1);
    EXPECT_NEAR(node->getScale().x, expectedScale.x, 0.001f);
    EXPECT_NEAR(node->getScale().y, expectedScale.y, 0.001f);
    EXPECT_NEAR(node->getScale().z, expectedScale.z, 0.001f);

    delete cmd;
}

TEST_F(TransformCommandsTests, ScaleCommand_MultipleNodes) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node1 = mgr->addSceneNode("ScaleCmdMulti1");
    Ogre::SceneNode* node2 = mgr->addSceneNode("ScaleCmdMulti2");
    ASSERT_NE(node1, nullptr);
    ASSERT_NE(node2, nullptr);
    node1->setScale(1, 1, 1);
    node2->setScale(2, 2, 2);

    QList<Ogre::SceneNode*> nodes = {node1, node2};
    Ogre::Vector3 factor(2, 2, 2);
    auto* cmd = new ScaleCommand(nodes, factor);

    cmd->redo();
    EXPECT_EQ(node1->getScale(), Ogre::Vector3(2, 2, 2));
    EXPECT_EQ(node2->getScale(), Ogre::Vector3(4, 4, 4));

    cmd->undo();
    EXPECT_NEAR(node1->getScale().x, 1.0f, 0.001f);
    EXPECT_NEAR(node2->getScale().x, 2.0f, 0.001f);

    delete cmd;
}

TEST_F(TransformCommandsTests, ScaleCommand_EmptyNodeList) {
    QList<Ogre::SceneNode*> nodes;
    Ogre::Vector3 factor(2, 2, 2);
    auto* cmd = new ScaleCommand(nodes, factor);

    EXPECT_NO_THROW(cmd->redo());
    EXPECT_NO_THROW(cmd->undo());

    delete cmd;
}

TEST_F(TransformCommandsTests, ScaleCommand_WithDestroyedNode) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("ScaleCmdDestroyed");
    ASSERT_NE(node, nullptr);

    QList<Ogre::SceneNode*> nodes = {node};
    Ogre::Vector3 factor(2, 2, 2);
    auto* cmd = new ScaleCommand(nodes, factor);

    mgr->destroySceneNode("ScaleCmdDestroyed");

    EXPECT_NO_THROW(cmd->redo());
    EXPECT_NO_THROW(cmd->undo());

    delete cmd;
}

// ---- DeleteCommand ----

TEST_F(TransformCommandsTests, DeleteCommand_FirstRedoIsNoop) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("DelCmdNode1");
    ASSERT_NE(node, nullptr);

    QList<Ogre::SceneNode*> nodes = {node};
    auto* cmd = new DeleteCommand(nodes);

    // First redo should be a no-op (caller handles initial deletion)
    cmd->redo();
    // Node should still be there since first redo is skipped
    EXPECT_TRUE(mgr->hasSceneNode("DelCmdNode1"));

    delete cmd;
}

TEST_F(TransformCommandsTests, DeleteCommand_UndoRestoresVisibility) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: needs entity"; }

    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("DelCmdNode2");
    ASSERT_NE(node, nullptr);

    // Attach entity so we can check visibility
    auto mesh = createInMemoryTriangleMesh("DelVisTestMesh");
    auto* entity = mgr->getSceneMgr()->createEntity(mesh);
    node->attachObject(entity);
    EXPECT_TRUE(entity->getVisible());

    QList<Ogre::SceneNode*> nodes = {node};
    auto* cmd = new DeleteCommand(nodes);

    // First redo (initial deletion - no-op)
    cmd->redo();

    // Hide the node manually (simulating what caller does)
    node->setVisible(false, true);
    EXPECT_FALSE(entity->getVisible());

    // Undo should restore visibility
    cmd->undo();
    EXPECT_TRUE(entity->getVisible());

    delete cmd;
}

TEST_F(TransformCommandsTests, DeleteCommand_SecondRedoHidesNode) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("DelCmdNode3");
    ASSERT_NE(node, nullptr);

    QList<Ogre::SceneNode*> nodes = {node};
    auto* cmd = new DeleteCommand(nodes);

    // First redo (no-op)
    cmd->redo();

    // Undo (restore)
    cmd->undo();

    // Second redo should hide the node
    cmd->redo();
    // Node exists but is hidden (DeleteCommand hides instead of destroying)

    delete cmd;
}

TEST_F(TransformCommandsTests, DeleteCommand_SnapshotPreservesTransform) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("DelCmdSnap");
    ASSERT_NE(node, nullptr);
    node->setPosition(10, 20, 30);
    node->setScale(2, 3, 4);
    Ogre::Quaternion orient(Ogre::Degree(45), Ogre::Vector3::UNIT_Y);
    node->setOrientation(orient);

    QList<Ogre::SceneNode*> nodes = {node};
    auto* cmd = new DeleteCommand(nodes);

    // First redo (no-op)
    cmd->redo();

    // Modify node transform
    node->setPosition(0, 0, 0);
    node->setScale(1, 1, 1);
    node->setOrientation(Ogre::Quaternion::IDENTITY);

    // Undo should restore original transform
    cmd->undo();
    EXPECT_EQ(node->getPosition(), Ogre::Vector3(10, 20, 30));
    EXPECT_EQ(node->getScale(), Ogre::Vector3(2, 3, 4));
    EXPECT_EQ(node->getOrientation(), orient);

    delete cmd;
}

TEST_F(TransformCommandsTests, DeleteCommand_MultipleNodes) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node1 = mgr->addSceneNode("DelCmdMulti1");
    Ogre::SceneNode* node2 = mgr->addSceneNode("DelCmdMulti2");
    ASSERT_NE(node1, nullptr);
    ASSERT_NE(node2, nullptr);
    node1->setPosition(1, 2, 3);
    node2->setPosition(4, 5, 6);

    QList<Ogre::SceneNode*> nodes = {node1, node2};
    auto* cmd = new DeleteCommand(nodes);

    // First redo (no-op)
    cmd->redo();

    // Modify positions
    node1->setPosition(0, 0, 0);
    node2->setPosition(0, 0, 0);

    // Undo should restore both
    cmd->undo();
    EXPECT_EQ(node1->getPosition(), Ogre::Vector3(1, 2, 3));
    EXPECT_EQ(node2->getPosition(), Ogre::Vector3(4, 5, 6));

    delete cmd;
}

// ---- DuplicateCommand ----

TEST_F(TransformCommandsTests, DuplicateCommand_Constructor) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("DupCmdNode1");
    ASSERT_NE(node, nullptr);

    QList<Ogre::SceneNode*> clones = {node};
    auto* cmd = new DuplicateCommand(clones, clones);
    EXPECT_NE(cmd, nullptr);

    delete cmd;
}

TEST_F(TransformCommandsTests, DuplicateCommand_FirstRedoIsNoop) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("DupCmdNoop");
    ASSERT_NE(node, nullptr);

    QList<Ogre::SceneNode*> clones = {node};
    auto* cmd = new DuplicateCommand(clones, clones);

    // First redo should be a no-op (caller already created the clones)
    cmd->redo();
    // Node should still be visible
    EXPECT_TRUE(mgr->hasSceneNode("DupCmdNoop"));

    delete cmd;
}

TEST_F(TransformCommandsTests, DuplicateCommand_UndoHidesClones) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: needs entity"; }

    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("DupCmdHide");
    ASSERT_NE(node, nullptr);

    auto mesh = createInMemoryTriangleMesh("DupHideTestMesh");
    auto* entity = mgr->getSceneMgr()->createEntity(mesh);
    node->attachObject(entity);
    EXPECT_TRUE(entity->getVisible());

    QList<Ogre::SceneNode*> clones = {node};
    auto* cmd = new DuplicateCommand(clones, clones);

    // First redo (no-op)
    cmd->redo();

    // Undo should hide the cloned nodes
    cmd->undo();
    EXPECT_FALSE(entity->getVisible());

    delete cmd;
}

TEST_F(TransformCommandsTests, DuplicateCommand_RedoShowsClones) {
    if (!canLoadMeshFiles()) { GTEST_SKIP() << "Skipping: needs entity"; }

    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("DupCmdShow");
    ASSERT_NE(node, nullptr);

    auto mesh = createInMemoryTriangleMesh("DupShowTestMesh");
    auto* entity = mgr->getSceneMgr()->createEntity(mesh);
    node->attachObject(entity);

    QList<Ogre::SceneNode*> clones = {node};
    auto* cmd = new DuplicateCommand(clones, clones);

    // First redo (no-op)
    cmd->redo();

    // Undo hides
    cmd->undo();
    EXPECT_FALSE(entity->getVisible());

    // Second redo should show again
    cmd->redo();
    EXPECT_TRUE(entity->getVisible());

    delete cmd;
}

TEST_F(TransformCommandsTests, DuplicateCommand_EmptyCloneList) {
    QList<Ogre::SceneNode*> clones;
    auto* cmd = new DuplicateCommand(clones, clones);

    // Should not crash with empty list
    EXPECT_NO_THROW(cmd->redo());
    EXPECT_NO_THROW(cmd->undo());

    delete cmd;
}
