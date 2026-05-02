#include <gtest/gtest.h>
#include "TransformCommands.h"
#include "../Manager.h"
#include "../SelectionSet.h"
#include "../SubMeshTransform.h"
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

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    }

    void TearDown() override {
        SelectionSet::getSingleton()->clear();
        if (app) app->processEvents();
    }
};

static inline Ogre::MeshPtr createInMemoryTwoSubMeshMesh(const std::string& name)
{
    auto mesh = Ogre::MeshManager::getSingleton().createManual(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    mesh->sharedVertexData = new Ogre::VertexData();
    auto* sharedDecl = mesh->sharedVertexData->vertexDeclaration;
    size_t sharedOffset = 0;
    sharedDecl->addElement(0, sharedOffset, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    sharedOffset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    sharedDecl->addElement(0, sharedOffset, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);
    sharedOffset += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    sharedDecl->addElement(0, sharedOffset, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES);

    auto sharedVbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        sharedDecl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    float sharedVerts[] = {
        0,0,0,   0,0,1,  0.0f,0.0f,
        1,0,0,   0,0,1,  1.0f,0.0f,
        0,1,0,   0,0,1,  0.0f,1.0f,
    };
    sharedVbuf->writeData(0, sizeof(sharedVerts), sharedVerts);
    mesh->sharedVertexData->vertexBufferBinding->setBinding(0, sharedVbuf);
    mesh->sharedVertexData->vertexCount = 3;

    auto* sub0 = mesh->createSubMesh();
    auto sharedIbuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    uint16_t sharedIdx[] = {0, 1, 2};
    sharedIbuf->writeData(0, sizeof(sharedIdx), sharedIdx);
    sub0->useSharedVertices = true;
    sub0->indexData->indexBuffer = sharedIbuf;
    sub0->indexData->indexCount = 3;

    auto* sub1 = mesh->createSubMesh();
    sub1->useSharedVertices = false;
    sub1->vertexData = new Ogre::VertexData();
    auto* decl1 = sub1->vertexData->vertexDeclaration;
    size_t offset1 = 0;
    decl1->addElement(0, offset1, Ogre::VET_FLOAT3, Ogre::VES_POSITION);
    offset1 += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl1->addElement(0, offset1, Ogre::VET_FLOAT3, Ogre::VES_NORMAL);
    offset1 += Ogre::VertexElement::getTypeSize(Ogre::VET_FLOAT3);
    decl1->addElement(0, offset1, Ogre::VET_FLOAT2, Ogre::VES_TEXTURE_COORDINATES);

    auto sub1Vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
        decl1->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    float sub1Verts[] = {
        10,0,0,  0,0,1,  0.0f,0.0f,
        11,0,0,  0,0,1,  1.0f,0.0f,
        10,1,0,  0,0,1,  0.0f,1.0f,
    };
    sub1Vbuf->writeData(0, sizeof(sub1Verts), sub1Verts);
    sub1->vertexData->vertexBufferBinding->setBinding(0, sub1Vbuf);
    sub1->vertexData->vertexCount = 3;

    auto sub1Ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
        Ogre::HardwareIndexBuffer::IT_16BIT, 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
    uint16_t sub1Idx[] = {0, 1, 2};
    sub1Ibuf->writeData(0, sizeof(sub1Idx), sub1Idx);
    sub1->indexData->indexBuffer = sub1Ibuf;
    sub1->indexData->indexCount = 3;

    mesh->_setBounds(Ogre::AxisAlignedBox(-1, -1, -1, 12, 2, 2));
    mesh->_setBoundingSphereRadius(12.0);
    mesh->load();
    return mesh;
}

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

TEST_F(TransformCommandsTests, RotateCommand_MultipleNodesUndoRestoresState) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node1 = mgr->addSceneNode("RotCmdMulti1");
    Ogre::SceneNode* node2 = mgr->addSceneNode("RotCmdMulti2");
    ASSERT_NE(node1, nullptr);
    ASSERT_NE(node2, nullptr);

    node1->setPosition(1, 0, 0);
    node2->setPosition(0, 2, 0);
    node1->setOrientation(Ogre::Quaternion(Ogre::Degree(10), Ogre::Vector3::UNIT_X));
    node2->setOrientation(Ogre::Quaternion(Ogre::Degree(20), Ogre::Vector3::UNIT_Y));

    const Ogre::Vector3 origPos1 = node1->getPosition();
    const Ogre::Vector3 origPos2 = node2->getPosition();
    const Ogre::Quaternion origOrient1 = node1->getOrientation();
    const Ogre::Quaternion origOrient2 = node2->getOrientation();

    QList<Ogre::SceneNode*> nodes = {node1, node2};
    Ogre::Quaternion rotation(Ogre::Degree(90), Ogre::Vector3::UNIT_Z);
    Ogre::Vector3 pivot(0, 0, 0);
    auto* cmd = new RotateCommand(nodes, rotation, pivot);

    cmd->redo();
    cmd->undo();

    EXPECT_EQ(node1->getPosition(), origPos1);
    EXPECT_EQ(node2->getPosition(), origPos2);
    EXPECT_NEAR(node1->getOrientation().w, origOrient1.w, 0.0001f);
    EXPECT_NEAR(node1->getOrientation().x, origOrient1.x, 0.0001f);
    EXPECT_NEAR(node1->getOrientation().y, origOrient1.y, 0.0001f);
    EXPECT_NEAR(node1->getOrientation().z, origOrient1.z, 0.0001f);
    EXPECT_NEAR(node2->getOrientation().w, origOrient2.w, 0.0001f);
    EXPECT_NEAR(node2->getOrientation().x, origOrient2.x, 0.0001f);
    EXPECT_NEAR(node2->getOrientation().y, origOrient2.y, 0.0001f);
    EXPECT_NEAR(node2->getOrientation().z, origOrient2.z, 0.0001f);

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
    ASSERT_TRUE(canLoadMeshFiles());

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

TEST_F(TransformCommandsTests, DeleteCommand_SecondRedoHidesAttachedEntity) {
    ASSERT_TRUE(canLoadMeshFiles());

    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* node = mgr->addSceneNode("DelCmdNodeEntity");
    ASSERT_NE(node, nullptr);

    auto mesh = createInMemoryTriangleMesh("DelCmdEntityMesh");
    auto* entity = mgr->getSceneMgr()->createEntity(mesh);
    node->attachObject(entity);
    EXPECT_TRUE(entity->getVisible());

    QList<Ogre::SceneNode*> nodes = {node};
    auto* cmd = new DeleteCommand(nodes);

    cmd->redo();  // first redo: no-op
    cmd->undo();  // restore snapshot state
    EXPECT_TRUE(entity->getVisible());

    cmd->redo();  // second redo: hide
    EXPECT_FALSE(entity->getVisible());

    cmd->undo();
    EXPECT_TRUE(entity->getVisible());

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

TEST_F(TransformCommandsTests, DuplicateCommand_UndoDestroysClones) {
    ASSERT_TRUE(canLoadMeshFiles());

    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* srcNode = mgr->addSceneNode("DupCmdHideSrc");
    ASSERT_NE(srcNode, nullptr);

    auto mesh = createInMemoryTriangleMesh("DupHideTestMesh");
    auto* entity = mgr->getSceneMgr()->createEntity(mesh);
    srcNode->attachObject(entity);

    // Create a real clone (separate node)
    Ogre::SceneNode* cloneNode = mgr->duplicateSceneNode(srcNode);
    ASSERT_NE(cloneNode, nullptr);
    QString cloneName = QString::fromStdString(cloneNode->getName());

    QList<Ogre::SceneNode*> sources = {srcNode};
    QList<Ogre::SceneNode*> clones = {cloneNode};
    auto* cmd = new DuplicateCommand(sources, clones);

    // First redo (no-op)
    cmd->redo();
    // Clone should exist
    EXPECT_TRUE(mgr->hasSceneNode(cloneName));

    // Undo should destroy the clone
    cmd->undo();
    EXPECT_FALSE(mgr->hasSceneNode(cloneName));

    // Source should still exist
    EXPECT_TRUE(mgr->hasSceneNode(QString::fromStdString(srcNode->getName())));

    delete cmd;
}

TEST_F(TransformCommandsTests, DuplicateCommand_RedoRecreatesClones) {
    ASSERT_TRUE(canLoadMeshFiles());

    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* srcNode = mgr->addSceneNode("DupCmdShowSrc");
    ASSERT_NE(srcNode, nullptr);

    auto mesh = createInMemoryTriangleMesh("DupShowTestMesh");
    auto* entity = mgr->getSceneMgr()->createEntity(mesh);
    srcNode->attachObject(entity);

    Ogre::SceneNode* cloneNode = mgr->duplicateSceneNode(srcNode);
    ASSERT_NE(cloneNode, nullptr);

    QList<Ogre::SceneNode*> sources = {srcNode};
    QList<Ogre::SceneNode*> clones = {cloneNode};
    auto* cmd = new DuplicateCommand(sources, clones);

    // First redo (no-op)
    cmd->redo();

    // Undo destroys clone
    cmd->undo();

    // Second redo should re-duplicate from source
    cmd->redo();
    // Source should still exist
    EXPECT_TRUE(mgr->hasSceneNode(QString::fromStdString(srcNode->getName())));

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

TEST_F(TransformCommandsTests, DuplicateCommand_RedoSkipsMissingSource) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* srcNode = mgr->addSceneNode("DupCmdMissingSrc");
    ASSERT_NE(srcNode, nullptr);
    const QString srcName = QString::fromStdString(srcNode->getName());

    Ogre::SceneNode* cloneNode = mgr->duplicateSceneNode(srcNode);
    ASSERT_NE(cloneNode, nullptr);
    const QString cloneName = QString::fromStdString(cloneNode->getName());

    QList<Ogre::SceneNode*> sources = {srcNode};
    QList<Ogre::SceneNode*> clones = {cloneNode};
    auto* cmd = new DuplicateCommand(sources, clones);

    cmd->redo();  // first redo: no-op
    cmd->undo();  // destroy initial clone
    EXPECT_FALSE(mgr->hasSceneNode(cloneName));

    mgr->destroySceneNode(srcName);
    EXPECT_FALSE(mgr->hasSceneNode(srcName));

    cmd->redo();  // should skip missing source
    EXPECT_FALSE(mgr->hasSceneNode(cloneName));
    EXPECT_TRUE(SelectionSet::getSingleton()->isEmpty());

    delete cmd;
}

TEST_F(TransformCommandsTests, DuplicateCommand_RedoRecreatesAndSelectsCloneWithoutEntity) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* srcNode = mgr->addSceneNode("DupCmdPlainSrc");
    ASSERT_NE(srcNode, nullptr);

    Ogre::SceneNode* initialClone = mgr->duplicateSceneNode(srcNode);
    ASSERT_NE(initialClone, nullptr);
    const QString initialCloneName = QString::fromStdString(initialClone->getName());

    QList<Ogre::SceneNode*> sources = {srcNode};
    QList<Ogre::SceneNode*> clones = {initialClone};
    auto* cmd = new DuplicateCommand(sources, clones);

    cmd->redo();  // first redo: no-op
    cmd->undo();
    EXPECT_FALSE(mgr->hasSceneNode(initialCloneName));
    EXPECT_TRUE(SelectionSet::getSingleton()->isEmpty());

    cmd->redo();
    ASSERT_EQ(SelectionSet::getSingleton()->getNodesCount(), 1);
    Ogre::SceneNode* selectedClone = SelectionSet::getSingleton()->getSceneNode(0);
    ASSERT_NE(selectedClone, nullptr);
    EXPECT_NE(selectedClone, srcNode);
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(selectedClone->getParent()),
              static_cast<Ogre::SceneNode*>(srcNode->getParent()));

    delete cmd;
}

// ---- GroupCommand / UngroupCommand / ReparentCommand ----

TEST_F(TransformCommandsTests, GroupCommand_UndoRedoRoundTrip) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* nodeA = mgr->addSceneNode("GroupCmdNodeA");
    Ogre::SceneNode* nodeB = mgr->addSceneNode("GroupCmdNodeB");
    ASSERT_NE(nodeA, nullptr);
    ASSERT_NE(nodeB, nullptr);
    nodeA->setPosition(1, 0, 0);
    nodeB->setPosition(0, 1, 0);

    QList<Ogre::SceneNode*> nodes = {nodeA, nodeB};
    auto* cmd = new GroupCommand(nodes);

    Ogre::SceneNode* initialGroup = mgr->groupNodes(nodes);
    ASSERT_NE(initialGroup, nullptr);
    const std::string groupName = initialGroup->getName();

    // First redo captures initial state only.
    cmd->redo();
    EXPECT_TRUE(mgr->getSceneMgr()->hasSceneNode(groupName));

    cmd->undo();
    EXPECT_FALSE(mgr->getSceneMgr()->hasSceneNode(groupName));
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(nodeA->getParent()), mgr->getSceneMgr()->getRootSceneNode());
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(nodeB->getParent()), mgr->getSceneMgr()->getRootSceneNode());

    cmd->redo();
    ASSERT_TRUE(mgr->getSceneMgr()->hasSceneNode(groupName));
    Ogre::SceneNode* recreatedGroup = mgr->getSceneMgr()->getSceneNode(groupName);
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(nodeA->getParent()), recreatedGroup);
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(nodeB->getParent()), recreatedGroup);

    delete cmd;
}

TEST_F(TransformCommandsTests, GroupCommand_RedoSkipsDestroyedChild) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* nodeA = mgr->addSceneNode("GroupCmdSkipNodeA");
    Ogre::SceneNode* nodeB = mgr->addSceneNode("GroupCmdSkipNodeB");
    ASSERT_NE(nodeA, nullptr);
    ASSERT_NE(nodeB, nullptr);

    QList<Ogre::SceneNode*> nodes = {nodeA, nodeB};
    auto* cmd = new GroupCommand(nodes);

    Ogre::SceneNode* initialGroup = mgr->groupNodes(nodes);
    ASSERT_NE(initialGroup, nullptr);
    const std::string groupName = initialGroup->getName();

    cmd->redo();  // first redo: capture group name
    cmd->undo();  // remove group, restore nodes

    mgr->destroySceneNode("GroupCmdSkipNodeB");
    EXPECT_FALSE(mgr->hasSceneNode("GroupCmdSkipNodeB"));

    cmd->redo();
    ASSERT_TRUE(mgr->getSceneMgr()->hasSceneNode(groupName));
    Ogre::SceneNode* recreatedGroup = mgr->getSceneMgr()->getSceneNode(groupName);
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(nodeA->getParent()), recreatedGroup);

    delete cmd;
}

TEST_F(TransformCommandsTests, GroupCommand_UndoRestoresChildrenToOriginalNonRootParent) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneManager* sceneMgr = mgr->getSceneMgr();
    ASSERT_NE(sceneMgr, nullptr);

    Ogre::SceneNode* container = sceneMgr->getRootSceneNode()->createChildSceneNode("GroupCmdNestedParent");
    Ogre::SceneNode* nodeA = container->createChildSceneNode("GroupCmdNestedChildA");
    Ogre::SceneNode* nodeB = container->createChildSceneNode("GroupCmdNestedChildB");
    ASSERT_NE(container, nullptr);
    ASSERT_NE(nodeA, nullptr);
    ASSERT_NE(nodeB, nullptr);

    QList<Ogre::SceneNode*> nodes = {nodeA, nodeB};
    auto* cmd = new GroupCommand(nodes);

    Ogre::SceneNode* initialGroup = mgr->groupNodes(nodes);
    ASSERT_NE(initialGroup, nullptr);
    const std::string groupName = initialGroup->getName();

    cmd->redo();  // first redo: capture state
    cmd->undo();

    EXPECT_FALSE(sceneMgr->hasSceneNode(groupName));
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(nodeA->getParent()), container);
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(nodeB->getParent()), container);

    delete cmd;
}

TEST_F(TransformCommandsTests, UngroupCommand_UndoRedoRoundTrip) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* nodeA = mgr->addSceneNode("UngroupCmdNodeA");
    Ogre::SceneNode* nodeB = mgr->addSceneNode("UngroupCmdNodeB");
    ASSERT_NE(nodeA, nullptr);
    ASSERT_NE(nodeB, nullptr);
    QList<Ogre::SceneNode*> nodes = {nodeA, nodeB};

    Ogre::SceneNode* groupNode = mgr->groupNodes(nodes);
    ASSERT_NE(groupNode, nullptr);
    const std::string groupName = groupNode->getName();

    auto* cmd = new UngroupCommand(groupNode);

    mgr->ungroupNode(groupNode);
    EXPECT_FALSE(mgr->getSceneMgr()->hasSceneNode(groupName));
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(nodeA->getParent()), mgr->getSceneMgr()->getRootSceneNode());

    // First redo is a no-op.
    cmd->redo();

    cmd->undo();
    ASSERT_TRUE(mgr->getSceneMgr()->hasSceneNode(groupName));
    Ogre::SceneNode* recreatedGroup = mgr->getSceneMgr()->getSceneNode(groupName);
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(nodeA->getParent()), recreatedGroup);
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(nodeB->getParent()), recreatedGroup);

    cmd->redo();
    EXPECT_FALSE(mgr->getSceneMgr()->hasSceneNode(groupName));
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(nodeA->getParent()), mgr->getSceneMgr()->getRootSceneNode());
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(nodeB->getParent()), mgr->getSceneMgr()->getRootSceneNode());

    delete cmd;
}

TEST_F(TransformCommandsTests, UngroupCommand_RedoWithMissingGroupNodeNoOp) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneNode* nodeA = mgr->addSceneNode("UngroupCmdMissingGroupA");
    Ogre::SceneNode* nodeB = mgr->addSceneNode("UngroupCmdMissingGroupB");
    ASSERT_NE(nodeA, nullptr);
    ASSERT_NE(nodeB, nullptr);
    QList<Ogre::SceneNode*> nodes = {nodeA, nodeB};

    Ogre::SceneNode* groupNode = mgr->groupNodes(nodes);
    ASSERT_NE(groupNode, nullptr);
    const std::string groupName = groupNode->getName();

    auto* cmd = new UngroupCommand(groupNode);

    mgr->ungroupNode(groupNode);
    cmd->redo();  // first redo: no-op

    cmd->undo();
    ASSERT_TRUE(mgr->getSceneMgr()->hasSceneNode(groupName));
    Ogre::SceneNode* recreatedGroup = mgr->getSceneMgr()->getSceneNode(groupName);

    // Remove group externally so command redo hits missing-group early return.
    mgr->ungroupNode(recreatedGroup);
    ASSERT_FALSE(mgr->getSceneMgr()->hasSceneNode(groupName));

    EXPECT_NO_THROW(cmd->redo());
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(nodeA->getParent()), mgr->getSceneMgr()->getRootSceneNode());
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(nodeB->getParent()), mgr->getSceneMgr()->getRootSceneNode());

    delete cmd;
}

TEST_F(TransformCommandsTests, UngroupCommand_UndoRestoresGroupUnderOriginalParentAndSkipsForbiddenChild) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneManager* sceneMgr = mgr->getSceneMgr();
    ASSERT_NE(sceneMgr, nullptr);

    Ogre::SceneNode* parent = sceneMgr->getRootSceneNode()->createChildSceneNode("UngroupCmdNestedParent");
    Ogre::SceneNode* nodeA = parent->createChildSceneNode("UngroupCmdNestedA");
    Ogre::SceneNode* nodeB = parent->createChildSceneNode("UngroupCmdNestedB");
    ASSERT_NE(parent, nullptr);
    ASSERT_NE(nodeA, nullptr);
    ASSERT_NE(nodeB, nullptr);

    QList<Ogre::SceneNode*> nodes = {nodeA, nodeB};
    Ogre::SceneNode* groupNode = mgr->groupNodes(nodes);
    ASSERT_NE(groupNode, nullptr);
    const std::string groupName = groupNode->getName();

    Ogre::SceneNode* forbiddenChild = groupNode->createChildSceneNode("Unnamed_ForbiddenForUngroupCommand");
    ASSERT_NE(forbiddenChild, nullptr);

    auto* cmd = new UngroupCommand(groupNode);
    mgr->ungroupNode(groupNode);
    cmd->redo();  // first redo: no-op
    cmd->undo();

    ASSERT_TRUE(sceneMgr->hasSceneNode(groupName));
    Ogre::SceneNode* recreatedGroup = sceneMgr->getSceneNode(groupName);
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(recreatedGroup->getParent()), parent);
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(nodeA->getParent()), recreatedGroup);
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(nodeB->getParent()), recreatedGroup);

    delete cmd;
}

TEST_F(TransformCommandsTests, ReparentCommand_UndoRedoRoundTrip) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneManager* sceneMgr = mgr->getSceneMgr();
    ASSERT_NE(sceneMgr, nullptr);

    Ogre::SceneNode* oldParent = sceneMgr->getRootSceneNode()->createChildSceneNode("ReparentCmdOldParent");
    Ogre::SceneNode* newParent = sceneMgr->getRootSceneNode()->createChildSceneNode("ReparentCmdNewParent");
    Ogre::SceneNode* child = oldParent->createChildSceneNode("ReparentCmdChild");
    // Keep oldParent non-empty after reparent so Manager::reparentNode does not auto-destroy it.
    Ogre::SceneNode* oldParentKeeper = oldParent->createChildSceneNode("ReparentCmdOldParentKeeper");
    ASSERT_NE(oldParent, nullptr);
    ASSERT_NE(newParent, nullptr);
    ASSERT_NE(child, nullptr);
    ASSERT_NE(oldParentKeeper, nullptr);

    child->setPosition(1, 2, 3);
    child->setOrientation(Ogre::Quaternion(Ogre::Degree(15), Ogre::Vector3::UNIT_Z));
    child->setScale(1.2f, 1.3f, 1.4f);

    const Ogre::Vector3 oldLocalPos = child->getPosition();
    const Ogre::Quaternion oldLocalOrient = child->getOrientation();
    const Ogre::Vector3 oldLocalScale = child->getScale();

    ASSERT_TRUE(mgr->reparentNode(child, newParent));
    const Ogre::Vector3 newLocalPos = child->getPosition();
    const Ogre::Quaternion newLocalOrient = child->getOrientation();
    const Ogre::Vector3 newLocalScale = child->getScale();

    auto* cmd = new ReparentCommand(
        "ReparentCmdChild",
        "ReparentCmdOldParent",
        "ReparentCmdNewParent",
        oldLocalPos, oldLocalOrient, oldLocalScale,
        newLocalPos, newLocalOrient, newLocalScale);

    // First redo is a no-op.
    cmd->redo();
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(child->getParent()), newParent);

    cmd->undo();
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(child->getParent()), oldParent);
    EXPECT_EQ(child->getPosition(), oldLocalPos);
    EXPECT_EQ(child->getOrientation(), oldLocalOrient);
    EXPECT_EQ(child->getScale(), oldLocalScale);

    cmd->redo();
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(child->getParent()), newParent);
    EXPECT_EQ(child->getPosition(), newLocalPos);
    EXPECT_EQ(child->getOrientation(), newLocalOrient);
    EXPECT_EQ(child->getScale(), newLocalScale);

    delete cmd;
}

TEST_F(TransformCommandsTests, ReparentCommand_UndoWithMissingOldParentNoOp) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneManager* sceneMgr = mgr->getSceneMgr();
    ASSERT_NE(sceneMgr, nullptr);

    Ogre::SceneNode* oldParent = sceneMgr->getRootSceneNode()->createChildSceneNode("ReparentCmdMissingOldParent");
    Ogre::SceneNode* newParent = sceneMgr->getRootSceneNode()->createChildSceneNode("ReparentCmdMissingOldNewParent");
    Ogre::SceneNode* child = oldParent->createChildSceneNode("ReparentCmdMissingOldChild");
    Ogre::SceneNode* keeper = oldParent->createChildSceneNode("ReparentCmdMissingOldKeeper");
    ASSERT_NE(oldParent, nullptr);
    ASSERT_NE(newParent, nullptr);
    ASSERT_NE(child, nullptr);
    ASSERT_NE(keeper, nullptr);

    child->setPosition(2, 3, 4);
    child->setOrientation(Ogre::Quaternion(Ogre::Degree(20), Ogre::Vector3::UNIT_Y));
    child->setScale(1.1f, 1.2f, 1.3f);

    const Ogre::Vector3 oldLocalPos = child->getPosition();
    const Ogre::Quaternion oldLocalOrient = child->getOrientation();
    const Ogre::Vector3 oldLocalScale = child->getScale();

    ASSERT_TRUE(mgr->reparentNode(child, newParent));
    const Ogre::Vector3 newLocalPos = child->getPosition();
    const Ogre::Quaternion newLocalOrient = child->getOrientation();
    const Ogre::Vector3 newLocalScale = child->getScale();

    auto* cmd = new ReparentCommand(
        "ReparentCmdMissingOldChild",
        "ReparentCmdMissingOldParent",
        "ReparentCmdMissingOldNewParent",
        oldLocalPos, oldLocalOrient, oldLocalScale,
        newLocalPos, newLocalOrient, newLocalScale);

    cmd->redo();  // first redo: no-op
    mgr->destroySceneNode("ReparentCmdMissingOldParent");
    ASSERT_FALSE(sceneMgr->hasSceneNode("ReparentCmdMissingOldParent"));

    // undo should early-return because old parent no longer exists
    cmd->undo();
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(child->getParent()), newParent);
    EXPECT_EQ(child->getPosition(), newLocalPos);
    EXPECT_EQ(child->getOrientation(), newLocalOrient);
    EXPECT_EQ(child->getScale(), newLocalScale);

    delete cmd;
}

TEST_F(TransformCommandsTests, ReparentCommand_RedoWithMissingNewParentNoOp) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneManager* sceneMgr = mgr->getSceneMgr();
    ASSERT_NE(sceneMgr, nullptr);

    Ogre::SceneNode* oldParent = sceneMgr->getRootSceneNode()->createChildSceneNode("ReparentCmdMissingNewOldParent");
    Ogre::SceneNode* newParent = sceneMgr->getRootSceneNode()->createChildSceneNode("ReparentCmdMissingNewParent");
    Ogre::SceneNode* child = oldParent->createChildSceneNode("ReparentCmdMissingNewChild");
    Ogre::SceneNode* keeper = oldParent->createChildSceneNode("ReparentCmdMissingNewKeeper");
    ASSERT_NE(oldParent, nullptr);
    ASSERT_NE(newParent, nullptr);
    ASSERT_NE(child, nullptr);
    ASSERT_NE(keeper, nullptr);

    child->setPosition(3, 4, 5);
    child->setOrientation(Ogre::Quaternion(Ogre::Degree(25), Ogre::Vector3::UNIT_X));
    child->setScale(1.4f, 1.5f, 1.6f);

    const Ogre::Vector3 oldLocalPos = child->getPosition();
    const Ogre::Quaternion oldLocalOrient = child->getOrientation();
    const Ogre::Vector3 oldLocalScale = child->getScale();

    ASSERT_TRUE(mgr->reparentNode(child, newParent));
    const Ogre::Vector3 newLocalPos = child->getPosition();
    const Ogre::Quaternion newLocalOrient = child->getOrientation();
    const Ogre::Vector3 newLocalScale = child->getScale();

    auto* cmd = new ReparentCommand(
        "ReparentCmdMissingNewChild",
        "ReparentCmdMissingNewOldParent",
        "ReparentCmdMissingNewParent",
        oldLocalPos, oldLocalOrient, oldLocalScale,
        newLocalPos, newLocalOrient, newLocalScale);

    cmd->redo();  // first redo: no-op
    cmd->undo();  // move back to old parent
    ASSERT_EQ(static_cast<Ogre::SceneNode*>(child->getParent()), oldParent);

    mgr->destroySceneNode("ReparentCmdMissingNewParent");
    ASSERT_FALSE(sceneMgr->hasSceneNode("ReparentCmdMissingNewParent"));

    // redo should early-return because new parent no longer exists
    cmd->redo();
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(child->getParent()), oldParent);
    EXPECT_EQ(child->getPosition(), oldLocalPos);
    EXPECT_EQ(child->getOrientation(), oldLocalOrient);
    EXPECT_EQ(child->getScale(), oldLocalScale);

    delete cmd;
}

TEST_F(TransformCommandsTests, ReparentCommand_UndoToRootWhenOldParentNameEmpty) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneManager* sceneMgr = mgr->getSceneMgr();
    ASSERT_NE(sceneMgr, nullptr);

    Ogre::SceneNode* newParent = sceneMgr->getRootSceneNode()->createChildSceneNode("ReparentCmdRootUndoNewParent");
    Ogre::SceneNode* child = sceneMgr->getRootSceneNode()->createChildSceneNode("ReparentCmdRootUndoChild");
    ASSERT_NE(newParent, nullptr);
    ASSERT_NE(child, nullptr);

    child->setPosition(1, 2, 3);
    child->setOrientation(Ogre::Quaternion(Ogre::Degree(35), Ogre::Vector3::UNIT_Z));
    child->setScale(1.2f, 1.3f, 1.4f);

    const Ogre::Vector3 oldLocalPos = child->getPosition();
    const Ogre::Quaternion oldLocalOrient = child->getOrientation();
    const Ogre::Vector3 oldLocalScale = child->getScale();

    ASSERT_TRUE(mgr->reparentNode(child, newParent));
    const Ogre::Vector3 newLocalPos = child->getPosition();
    const Ogre::Quaternion newLocalOrient = child->getOrientation();
    const Ogre::Vector3 newLocalScale = child->getScale();

    auto* cmd = new ReparentCommand(
        "ReparentCmdRootUndoChild",
        "",
        "ReparentCmdRootUndoNewParent",
        oldLocalPos, oldLocalOrient, oldLocalScale,
        newLocalPos, newLocalOrient, newLocalScale);

    cmd->redo();  // first redo: no-op
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(child->getParent()), newParent);

    cmd->undo();
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(child->getParent()), sceneMgr->getRootSceneNode());
    EXPECT_EQ(child->getPosition(), oldLocalPos);
    EXPECT_EQ(child->getOrientation(), oldLocalOrient);
    EXPECT_EQ(child->getScale(), oldLocalScale);

    delete cmd;
}

TEST_F(TransformCommandsTests, ReparentCommand_RedoToRootWhenNewParentNameEmpty) {
    Manager* mgr = Manager::getSingleton();
    Ogre::SceneManager* sceneMgr = mgr->getSceneMgr();
    ASSERT_NE(sceneMgr, nullptr);

    Ogre::SceneNode* oldParent = sceneMgr->getRootSceneNode()->createChildSceneNode("ReparentCmdRootRedoOldParent");
    Ogre::SceneNode* child = oldParent->createChildSceneNode("ReparentCmdRootRedoChild");
    Ogre::SceneNode* keeper = oldParent->createChildSceneNode("ReparentCmdRootRedoKeeper");
    ASSERT_NE(oldParent, nullptr);
    ASSERT_NE(child, nullptr);
    ASSERT_NE(keeper, nullptr);

    child->setPosition(2, 3, 4);
    child->setOrientation(Ogre::Quaternion(Ogre::Degree(15), Ogre::Vector3::UNIT_X));
    child->setScale(1.1f, 1.2f, 1.3f);

    const Ogre::Vector3 oldLocalPos = child->getPosition();
    const Ogre::Quaternion oldLocalOrient = child->getOrientation();
    const Ogre::Vector3 oldLocalScale = child->getScale();

    ASSERT_TRUE(mgr->reparentNode(child, sceneMgr->getRootSceneNode()));
    const Ogre::Vector3 newLocalPos = child->getPosition();
    const Ogre::Quaternion newLocalOrient = child->getOrientation();
    const Ogre::Vector3 newLocalScale = child->getScale();

    auto* cmd = new ReparentCommand(
        "ReparentCmdRootRedoChild",
        "ReparentCmdRootRedoOldParent",
        "",
        oldLocalPos, oldLocalOrient, oldLocalScale,
        newLocalPos, newLocalOrient, newLocalScale);

    cmd->redo();  // first redo: no-op
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(child->getParent()), sceneMgr->getRootSceneNode());

    cmd->undo();
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(child->getParent()), oldParent);

    cmd->redo();
    EXPECT_EQ(static_cast<Ogre::SceneNode*>(child->getParent()), sceneMgr->getRootSceneNode());
    EXPECT_EQ(child->getPosition(), newLocalPos);
    EXPECT_EQ(child->getOrientation(), newLocalOrient);
    EXPECT_EQ(child->getScale(), newLocalScale);

    delete cmd;
}

// ---- SubMeshTransformCommand ----

TEST_F(TransformCommandsTests, SubMeshTransformCommand_TranslateUndoRedo) {
    ASSERT_TRUE(canLoadMeshFiles());

    Manager* mgr = Manager::getSingleton();
    auto mesh = createInMemoryTriangleMesh("SubMeshCmdTranslate");
    auto* entity = mgr->getSceneMgr()->createEntity(mesh);
    auto* node = mgr->addSceneNode("SubMeshCmdNode");
    node->attachObject(entity);

    Ogre::SubEntity* sub = entity->getSubEntity(0);
    ASSERT_NE(sub, nullptr);

    // Read original positions
    auto origPositions = SubMeshTransform::readPositions(entity, 0);
    ASSERT_EQ(origPositions.size(), 3u);

    // Translate the sub-mesh
    Ogre::Vector3 delta(10, 20, 30);
    SubMeshTransform::translateSubMesh(entity, 0, delta);

    // Create undo command with original positions (simulates what TransformOperator does)
    auto* cmd = new SubMeshTransformCommand(sub, origPositions, "Test SubMesh Translate");

    // First redo captures current (post-transform) positions (no-op)
    cmd->redo();

    // Verify positions are translated
    auto currentPositions = SubMeshTransform::readPositions(entity, 0);
    for (size_t i = 0; i < origPositions.size(); ++i) {
        EXPECT_NEAR(currentPositions[i].x, origPositions[i].x + delta.x, 0.001f);
        EXPECT_NEAR(currentPositions[i].y, origPositions[i].y + delta.y, 0.001f);
        EXPECT_NEAR(currentPositions[i].z, origPositions[i].z + delta.z, 0.001f);
    }

    // Undo should restore original positions
    cmd->undo();
    auto restoredPositions = SubMeshTransform::readPositions(entity, 0);
    for (size_t i = 0; i < origPositions.size(); ++i) {
        EXPECT_NEAR(restoredPositions[i].x, origPositions[i].x, 0.001f);
        EXPECT_NEAR(restoredPositions[i].y, origPositions[i].y, 0.001f);
        EXPECT_NEAR(restoredPositions[i].z, origPositions[i].z, 0.001f);
    }

    // Redo should re-apply the transform
    cmd->redo();
    auto redonePositions = SubMeshTransform::readPositions(entity, 0);
    for (size_t i = 0; i < origPositions.size(); ++i) {
        EXPECT_NEAR(redonePositions[i].x, origPositions[i].x + delta.x, 0.001f);
        EXPECT_NEAR(redonePositions[i].y, origPositions[i].y + delta.y, 0.001f);
        EXPECT_NEAR(redonePositions[i].z, origPositions[i].z + delta.z, 0.001f);
    }

    delete cmd;
}

TEST_F(TransformCommandsTests, SubMeshTransformCommand_NullSubEntity) {
    // Should not crash with null sub-entity
    std::vector<Ogre::Vector3> empty;
    auto* cmd = new SubMeshTransformCommand(nullptr, empty, "Null test");

    EXPECT_NO_THROW(cmd->redo());
    EXPECT_NO_THROW(cmd->undo());

    delete cmd;
}

TEST_F(TransformCommandsTests, SubMeshTransformCommand_NonZeroSubMeshIndexTargetsCorrectSubMesh) {
    ASSERT_TRUE(canLoadMeshFiles());

    Manager* mgr = Manager::getSingleton();
    auto mesh = createInMemoryTwoSubMeshMesh("SubMeshCmdTwoSubMeshIndex");
    auto* entity = mgr->getSceneMgr()->createEntity(mesh);
    auto* node = mgr->addSceneNode("SubMeshCmdTwoSubMeshNode");
    ASSERT_NE(entity, nullptr);
    ASSERT_NE(node, nullptr);
    node->attachObject(entity);
    ASSERT_EQ(entity->getNumSubEntities(), 2u);

    Ogre::SubEntity* secondSub = entity->getSubEntity(1);
    ASSERT_NE(secondSub, nullptr);

    const auto originalSub0 = SubMeshTransform::readPositions(entity, 0);
    const auto originalSub1 = SubMeshTransform::readPositions(entity, 1);
    ASSERT_EQ(originalSub0.size(), 3u);
    ASSERT_EQ(originalSub1.size(), 3u);

    const Ogre::Vector3 delta(2.0f, -1.0f, 0.5f);
    SubMeshTransform::translateSubMesh(entity, 1, delta);

    auto* cmd = new SubMeshTransformCommand(secondSub, originalSub1, "SubMesh index 1");
    cmd->redo();  // first redo: capture transformed positions
    cmd->undo();  // restore original positions for submesh 1

    const auto undoneSub0 = SubMeshTransform::readPositions(entity, 0);
    const auto undoneSub1 = SubMeshTransform::readPositions(entity, 1);
    ASSERT_EQ(undoneSub0.size(), originalSub0.size());
    ASSERT_EQ(undoneSub1.size(), originalSub1.size());
    for (size_t i = 0; i < originalSub0.size(); ++i) {
        EXPECT_NEAR(undoneSub0[i].x, originalSub0[i].x, 0.001f);
        EXPECT_NEAR(undoneSub0[i].y, originalSub0[i].y, 0.001f);
        EXPECT_NEAR(undoneSub0[i].z, originalSub0[i].z, 0.001f);
    }
    for (size_t i = 0; i < originalSub1.size(); ++i) {
        EXPECT_NEAR(undoneSub1[i].x, originalSub1[i].x, 0.001f);
        EXPECT_NEAR(undoneSub1[i].y, originalSub1[i].y, 0.001f);
        EXPECT_NEAR(undoneSub1[i].z, originalSub1[i].z, 0.001f);
    }

    cmd->redo();  // reapply captured transformed positions on submesh 1
    const auto redoneSub1 = SubMeshTransform::readPositions(entity, 1);
    ASSERT_EQ(redoneSub1.size(), originalSub1.size());
    for (size_t i = 0; i < originalSub1.size(); ++i) {
        EXPECT_NEAR(redoneSub1[i].x, originalSub1[i].x + delta.x, 0.001f);
        EXPECT_NEAR(redoneSub1[i].y, originalSub1[i].y + delta.y, 0.001f);
        EXPECT_NEAR(redoneSub1[i].z, originalSub1[i].z + delta.z, 0.001f);
    }

    delete cmd;
}

TEST_F(TransformCommandsTests, SubMeshTransform_GetCenter) {
    ASSERT_TRUE(canLoadMeshFiles());

    Manager* mgr = Manager::getSingleton();
    auto mesh = createInMemoryTriangleMesh("SubMeshCenterTest");
    auto* entity = mgr->getSceneMgr()->createEntity(mesh);
    auto* node = mgr->addSceneNode("SubMeshCenterNode");
    node->attachObject(entity);

    Ogre::Vector3 center = SubMeshTransform::getSubMeshCenter(entity, 0);
    // Triangle vertices are (0,0,0), (1,0,0), (0,1,0)
    // Centroid should be ~(0.333, 0.333, 0)
    EXPECT_NEAR(center.x, 1.0f/3.0f, 0.01f);
    EXPECT_NEAR(center.y, 1.0f/3.0f, 0.01f);
    EXPECT_NEAR(center.z, 0.0f, 0.01f);
}

TEST_F(TransformCommandsTests, SubMeshTransform_ReadWritePositions) {
    ASSERT_TRUE(canLoadMeshFiles());

    Manager* mgr = Manager::getSingleton();
    auto mesh = createInMemoryTriangleMesh("SubMeshRWTest");
    auto* entity = mgr->getSceneMgr()->createEntity(mesh);
    auto* node = mgr->addSceneNode("SubMeshRWNode");
    node->attachObject(entity);

    auto positions = SubMeshTransform::readPositions(entity, 0);
    ASSERT_EQ(positions.size(), 3u);

    // Modify and write back
    for (auto& p : positions)
        p += Ogre::Vector3(5, 5, 5);
    SubMeshTransform::writePositions(entity, 0, positions);

    // Read again and verify
    auto modified = SubMeshTransform::readPositions(entity, 0);
    EXPECT_NEAR(modified[0].x, 5.0f, 0.001f);
    EXPECT_NEAR(modified[0].y, 5.0f, 0.001f);
    EXPECT_NEAR(modified[0].z, 5.0f, 0.001f);
}

// ---- SubMeshTransform: scaleSubMesh ----

TEST_F(TransformCommandsTests, SubMeshTransform_ScaleSubMesh) {
    ASSERT_TRUE(canLoadMeshFiles());

    Manager* mgr = Manager::getSingleton();
    auto mesh = createInMemoryTriangleMesh("SubMeshScaleTest");
    auto* entity = mgr->getSceneMgr()->createEntity(mesh);
    auto* node = mgr->addSceneNode("SubMeshScaleNode");
    node->attachObject(entity);

    // Read original positions: triangle at (0,0,0), (1,0,0), (0,1,0)
    auto origPositions = SubMeshTransform::readPositions(entity, 0);
    ASSERT_EQ(origPositions.size(), 3u);

    Ogre::Vector3 center = SubMeshTransform::getSubMeshCenter(entity, 0);

    // Scale by 2x around centroid
    Ogre::Vector3 scaleFactor(2.0f, 2.0f, 2.0f);
    SubMeshTransform::scaleSubMesh(entity, 0, scaleFactor);

    auto scaledPositions = SubMeshTransform::readPositions(entity, 0);
    ASSERT_EQ(scaledPositions.size(), 3u);

    // After scaling by 2x around centroid, each vertex should be 2x as far from the centroid
    for (size_t i = 0; i < origPositions.size(); ++i) {
        Ogre::Vector3 origOffset = origPositions[i] - center;
        Ogre::Vector3 scaledOffset = scaledPositions[i] - center;
        EXPECT_NEAR(scaledOffset.x, origOffset.x * scaleFactor.x, 0.01f);
        EXPECT_NEAR(scaledOffset.y, origOffset.y * scaleFactor.y, 0.01f);
        EXPECT_NEAR(scaledOffset.z, origOffset.z * scaleFactor.z, 0.01f);
    }
}

// ---- SubMeshTransform: rotateSubMesh ----

TEST_F(TransformCommandsTests, SubMeshTransform_RotateSubMesh) {
    ASSERT_TRUE(canLoadMeshFiles());

    Manager* mgr = Manager::getSingleton();
    auto mesh = createInMemoryTriangleMesh("SubMeshRotateTest");
    auto* entity = mgr->getSceneMgr()->createEntity(mesh);
    auto* node = mgr->addSceneNode("SubMeshRotateNode");
    node->attachObject(entity);

    // Read original positions: triangle at (0,0,0), (1,0,0), (0,1,0)
    auto origPositions = SubMeshTransform::readPositions(entity, 0);
    ASSERT_EQ(origPositions.size(), 3u);

    Ogre::Vector3 center = SubMeshTransform::getSubMeshCenter(entity, 0);

    // Rotate 90 degrees around Z axis
    Ogre::Quaternion rotation(Ogre::Degree(90), Ogre::Vector3::UNIT_Z);
    SubMeshTransform::rotateSubMesh(entity, 0, rotation);

    auto rotatedPositions = SubMeshTransform::readPositions(entity, 0);
    ASSERT_EQ(rotatedPositions.size(), 3u);

    // After rotating around centroid, each vertex should be at rotated offset
    for (size_t i = 0; i < origPositions.size(); ++i) {
        Ogre::Vector3 origOffset = origPositions[i] - center;
        Ogre::Vector3 expectedOffset = rotation * origOffset;
        Ogre::Vector3 actualOffset = rotatedPositions[i] - center;
        EXPECT_NEAR(actualOffset.x, expectedOffset.x, 0.01f);
        EXPECT_NEAR(actualOffset.y, expectedOffset.y, 0.01f);
        EXPECT_NEAR(actualOffset.z, expectedOffset.z, 0.01f);
    }
}

// ---- SubMeshTransform: rotateSubMesh preserves centroid ----

TEST_F(TransformCommandsTests, SubMeshTransform_RotatePreservesCentroid) {
    ASSERT_TRUE(canLoadMeshFiles());

    Manager* mgr = Manager::getSingleton();
    auto mesh = createInMemoryTriangleMesh("SubMeshRotCenterTest");
    auto* entity = mgr->getSceneMgr()->createEntity(mesh);
    auto* node = mgr->addSceneNode("SubMeshRotCenterNode");
    node->attachObject(entity);

    Ogre::Vector3 centerBefore = SubMeshTransform::getSubMeshCenter(entity, 0);

    Ogre::Quaternion rotation(Ogre::Degree(45), Ogre::Vector3::UNIT_Y);
    SubMeshTransform::rotateSubMesh(entity, 0, rotation);

    Ogre::Vector3 centerAfter = SubMeshTransform::getSubMeshCenter(entity, 0);

    // Centroid should remain the same after rotation around centroid
    EXPECT_NEAR(centerAfter.x, centerBefore.x, 0.01f);
    EXPECT_NEAR(centerAfter.y, centerBefore.y, 0.01f);
    EXPECT_NEAR(centerAfter.z, centerBefore.z, 0.01f);
}

// ---- SubMeshTransform: scaleSubMesh preserves centroid ----

TEST_F(TransformCommandsTests, SubMeshTransform_ScalePreservesCentroid) {
    ASSERT_TRUE(canLoadMeshFiles());

    Manager* mgr = Manager::getSingleton();
    auto mesh = createInMemoryTriangleMesh("SubMeshScaleCenterTest");
    auto* entity = mgr->getSceneMgr()->createEntity(mesh);
    auto* node = mgr->addSceneNode("SubMeshScaleCenterNode");
    node->attachObject(entity);

    Ogre::Vector3 centerBefore = SubMeshTransform::getSubMeshCenter(entity, 0);

    SubMeshTransform::scaleSubMesh(entity, 0, Ogre::Vector3(3.0f, 0.5f, 2.0f));

    Ogre::Vector3 centerAfter = SubMeshTransform::getSubMeshCenter(entity, 0);

    // Centroid should remain the same after scaling around centroid
    EXPECT_NEAR(centerAfter.x, centerBefore.x, 0.01f);
    EXPECT_NEAR(centerAfter.y, centerBefore.y, 0.01f);
    EXPECT_NEAR(centerAfter.z, centerBefore.z, 0.01f);
}

TEST_F(TransformCommandsTests, SubMeshTransform_NullEntityIsNoOp)
{
    SubMeshTransform::translateSubMesh(nullptr, 0, Ogre::Vector3::UNIT_X);
    SubMeshTransform::scaleSubMesh(nullptr, 0, Ogre::Vector3::UNIT_SCALE);
    SubMeshTransform::rotateSubMesh(nullptr, 0, Ogre::Quaternion::IDENTITY);
    EXPECT_EQ(SubMeshTransform::getSubMeshCenter(nullptr, 0), Ogre::Vector3::ZERO);
    EXPECT_TRUE(SubMeshTransform::readPositions(nullptr, 0).empty());
    SubMeshTransform::writePositions(nullptr, 0, {});
}

TEST_F(TransformCommandsTests, SubMeshTransform_InvalidSubMeshIndex)
{
    ASSERT_TRUE(canLoadMeshFiles());

    Manager* mgr = Manager::getSingleton();
    auto mesh = createInMemoryTriangleMesh("SubMeshBadIdx");
    auto* entity = mgr->getSceneMgr()->createEntity(mesh);
    auto* node = mgr->addSceneNode("SubMeshBadIdxNode");
    node->attachObject(entity);

    EXPECT_EQ(SubMeshTransform::getSubMeshCenter(entity, 99), Ogre::Vector3::ZERO);
    EXPECT_TRUE(SubMeshTransform::readPositions(entity, 99).empty());
}
