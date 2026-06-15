#include <gtest/gtest.h>
#include "SceneTreeModel.h"
#include "Manager.h"
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include <OgreSceneManager.h>
#include <OgreSceneNode.h>
#include "TestHelpers.h"

// Tests for the reparent API of SceneTreeModel:
//   bool canReparent(const QString& nodeName, const QString& newParentName) const
//   bool reparentNode(const QString& nodeName, const QString& newParentName)
//
// Both resolve names against the live Ogre scene graph and call
// Manager::reparentNode, so the whole suite is gated behind tryInitOgre()
// (macOS/headless without GL skips gracefully).
class SceneTreeModelReparentTests : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    SceneTreeModel* model = nullptr;

    void SetUp() override {
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        // This suite operates on the live Ogre scene graph, so Ogre must be
        // available. Fail loudly rather than GTEST_SKIP — a skip would silently
        // hide a broken CI/runtime environment (per project convention; the CI
        // harness also treats skip-only suites as failures).
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed — invalid CI/runtime environment";

        createStandardOgreMaterials();
        model = new SceneTreeModel();
    }

    void TearDown() override {
        delete model;
        model = nullptr;
        if (app) app->processEvents();
    }

    Ogre::SceneManager* sceneMgr() const {
        return Manager::getSingleton()->getSceneMgr();
    }
};

// ---- canReparent: pure rejection branches --------------------------------

TEST_F(SceneTreeModelReparentTests, CanReparentFalseForUnknownNode) {
    // Node does not exist in the scene at all.
    EXPECT_FALSE(model->canReparent("NoSuchNode", "root"));
}

TEST_F(SceneTreeModelReparentTests, CanReparentFalseForUnknownParent) {
    Ogre::SceneNode* node = Manager::getSingleton()->addSceneNode("RP_NodeA");
    ASSERT_NE(node, nullptr);

    // Node exists but parent name does not (and is not empty/"root").
    EXPECT_FALSE(model->canReparent("RP_NodeA", "NoSuchParent"));
}

TEST_F(SceneTreeModelReparentTests, CanReparentFalseForSelfParent) {
    Ogre::SceneNode* node = Manager::getSingleton()->addSceneNode("RP_SelfNode");
    ASSERT_NE(node, nullptr);

    // node == newParent  -> rejected.
    EXPECT_FALSE(model->canReparent("RP_SelfNode", "RP_SelfNode"));
}

TEST_F(SceneTreeModelReparentTests, CanReparentFalseWhenAlreadyChildOfRoot) {
    // addSceneNode attaches under the root scene node, so reparenting it
    // to root again is a no-op and must be rejected.
    Ogre::SceneNode* node = Manager::getSingleton()->addSceneNode("RP_RootChild");
    ASSERT_NE(node, nullptr);
    ASSERT_EQ(node->getParent(), sceneMgr()->getRootSceneNode());

    EXPECT_FALSE(model->canReparent("RP_RootChild", "root"));
    EXPECT_FALSE(model->canReparent("RP_RootChild", QString()));
}

TEST_F(SceneTreeModelReparentTests, CanReparentFalseForCycleDescendantParent) {
    // Build parent -> child relationship, then try to reparent the parent
    // under its own descendant (would create a cycle).
    Ogre::SceneNode* parent = Manager::getSingleton()->addSceneNode("RP_CycleParent");
    Ogre::SceneNode* child  = Manager::getSingleton()->addSceneNode("RP_CycleChild");
    ASSERT_NE(parent, nullptr);
    ASSERT_NE(child, nullptr);

    ASSERT_TRUE(Manager::getSingleton()->reparentNode(child, parent));
    ASSERT_EQ(child->getParent(), parent);

    // parent is an ancestor of child; reparenting parent under child is illegal.
    EXPECT_FALSE(model->canReparent("RP_CycleParent", "RP_CycleChild"));
}

// ---- canReparent: success branch -----------------------------------------

TEST_F(SceneTreeModelReparentTests, CanReparentTrueForLegalMove) {
    Manager::getSingleton()->addSceneNode("RP_LegalA");
    Manager::getSingleton()->addSceneNode("RP_LegalB");

    // Both currently children of root; moving A under B is legal.
    EXPECT_TRUE(model->canReparent("RP_LegalA", "RP_LegalB"));
}

// ---- reparentNode: failure path (delegates to canReparent) ---------------

TEST_F(SceneTreeModelReparentTests, ReparentNodeFalseWhenCanReparentFails) {
    // Unknown node -> canReparent returns false -> reparentNode returns false.
    EXPECT_FALSE(model->reparentNode("NoSuchNode", "root"));

    Manager::getSingleton()->addSceneNode("RP_FailSelf");
    // Self-parent -> canReparent fails.
    EXPECT_FALSE(model->reparentNode("RP_FailSelf", "RP_FailSelf"));
}

// ---- reparentNode: success path ------------------------------------------

TEST_F(SceneTreeModelReparentTests, ReparentNodeSuccessMovesNodeUnderNewParent) {
    Ogre::SceneNode* a = Manager::getSingleton()->addSceneNode("RP_MoveA");
    Ogre::SceneNode* b = Manager::getSingleton()->addSceneNode("RP_MoveB");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_EQ(a->getParent(), sceneMgr()->getRootSceneNode());

    EXPECT_TRUE(model->reparentNode("RP_MoveA", "RP_MoveB"));

    // After a successful reparent the node's parent is the new parent.
    EXPECT_EQ(a->getParent(), b);
}

TEST_F(SceneTreeModelReparentTests, ReparentNodeSuccessPreservesWorldTransform) {
    Ogre::SceneNode* a = Manager::getSingleton()->addSceneNode("RP_WorldA");
    Ogre::SceneNode* b = Manager::getSingleton()->addSceneNode("RP_WorldB");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    // Give the soon-to-be parent a non-trivial world transform so the
    // local-transform recapture path (SceneTreeModel.cpp:424-427) does work.
    b->setPosition(10.0f, 5.0f, -3.0f);
    a->setPosition(2.0f, 0.0f, 1.0f);

    const Ogre::Vector3 worldBefore = a->_getDerivedPosition();

    EXPECT_TRUE(model->reparentNode("RP_WorldA", "RP_WorldB"));
    EXPECT_EQ(a->getParent(), b);

    // Manager::reparentNode preserves world transform; recaptured local
    // transform should differ from the original local pos (parent moved).
    sceneMgr()->getRootSceneNode()->_update(true, false);
    const Ogre::Vector3 worldAfter = a->_getDerivedPosition();

    EXPECT_NEAR(worldBefore.x, worldAfter.x, 1e-3f);
    EXPECT_NEAR(worldBefore.y, worldAfter.y, 1e-3f);
    EXPECT_NEAR(worldBefore.z, worldAfter.z, 1e-3f);

    // Local position should have been rebased relative to the new parent.
    EXPECT_NEAR(a->getPosition().x, worldBefore.x - 10.0f, 1e-3f);
    EXPECT_NEAR(a->getPosition().y, worldBefore.y - 5.0f, 1e-3f);
    EXPECT_NEAR(a->getPosition().z, worldBefore.z - (-3.0f), 1e-3f);
}

TEST_F(SceneTreeModelReparentTests, ReparentNodeSuccessThenBackToRoot) {
    Ogre::SceneNode* a = Manager::getSingleton()->addSceneNode("RP_BackA");
    Ogre::SceneNode* b = Manager::getSingleton()->addSceneNode("RP_BackB");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);

    ASSERT_TRUE(model->reparentNode("RP_BackA", "RP_BackB"));
    ASSERT_EQ(a->getParent(), b);

    // Now move it back to root via the empty/"root" parent name path.
    EXPECT_TRUE(model->canReparent("RP_BackA", "root"));
    EXPECT_TRUE(model->reparentNode("RP_BackA", "root"));
    EXPECT_EQ(a->getParent(), sceneMgr()->getRootSceneNode());
}
