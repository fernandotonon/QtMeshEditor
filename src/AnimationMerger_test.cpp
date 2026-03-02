#include <gtest/gtest.h>
#include "AnimationMerger.h"
#include "Manager.h"
#include "TestHelpers.h"
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include <OgreSkeleton.h>
#include <OgreSkeletonManager.h>
#include <OgreMeshManager.h>

class AnimationMergerTest : public ::testing::Test {
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
            app->processEvents();
    }

    QApplication* app = nullptr;

    // Helper: create a skeleton with given bone names and animations
    Ogre::SkeletonPtr createTestSkeleton(const std::string& name,
                                          const std::vector<std::string>& boneNames,
                                          const std::vector<std::string>& animNames)
    {
        auto skel = Ogre::SkeletonManager::getSingleton().create(
            name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

        for (size_t i = 0; i < boneNames.size(); ++i)
        {
            auto* bone = skel->createBone(boneNames[i], static_cast<unsigned short>(i));
            if (i > 0)
                skel->getBone(0)->addChild(bone);
        }
        skel->setBindingPose();

        for (const auto& animName : animNames)
        {
            auto* anim = skel->createAnimation(animName, 1.0f);
            auto* track = anim->createNodeTrack(0);
            track->setAssociatedNode(skel->getBone(0));
            auto* kf = track->createNodeKeyFrame(0.0f);
            kf->setTranslate(Ogre::Vector3::ZERO);
            kf = track->createNodeKeyFrame(1.0f);
            kf->setTranslate(Ogre::Vector3(1, 0, 0));
        }

        return skel;
    }
};

TEST_F(AnimationMergerTest, CompatibleSkeletons)
{
    auto skelA = createTestSkeleton("compat_a", {"root", "spine", "head"}, {"idle"});
    auto skelB = createTestSkeleton("compat_b", {"root", "spine", "head"}, {"walk"});

    EXPECT_TRUE(AnimationMerger::areSkeletonsCompatible(skelA, skelB));

    Ogre::SkeletonManager::getSingleton().remove(skelA);
    Ogre::SkeletonManager::getSingleton().remove(skelB);
}

TEST_F(AnimationMergerTest, IncompatibleSkeletons)
{
    auto skelA = createTestSkeleton("incompat_a", {"root", "spine", "head"}, {"idle"});
    auto skelB = createTestSkeleton("incompat_b", {"root", "arm", "hand"}, {"wave"});

    EXPECT_FALSE(AnimationMerger::areSkeletonsCompatible(skelA, skelB));

    Ogre::SkeletonManager::getSingleton().remove(skelA);
    Ogre::SkeletonManager::getSingleton().remove(skelB);
}

TEST_F(AnimationMergerTest, NullSkeletons)
{
    Ogre::SkeletonPtr null;
    auto skelA = createTestSkeleton("null_test", {"root"}, {"idle"});

    EXPECT_FALSE(AnimationMerger::areSkeletonsCompatible(null, skelA));
    EXPECT_FALSE(AnimationMerger::areSkeletonsCompatible(skelA, null));

    Ogre::SkeletonManager::getSingleton().remove(skelA);
}

TEST_F(AnimationMergerTest, MergeAnimationsBasic)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "Skipping: cannot load mesh files in this environment";

    // Create two meshes sharing compatible skeletons
    auto skelA = createTestSkeleton("merge_skel_a", {"root", "spine"}, {"idle"});
    auto skelB = createTestSkeleton("merge_skel_b", {"root", "spine"}, {"walk"});

    auto meshA = Ogre::MeshManager::getSingleton().create("merge_mesh_a",
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    meshA->_notifySkeleton(skelA);

    auto meshB = Ogre::MeshManager::getSingleton().create("merge_mesh_b",
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    meshB->_notifySkeleton(skelB);

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* nodeA = sceneMgr->getRootSceneNode()->createChildSceneNode("baseNode");
    auto* entityA = sceneMgr->createEntity("baseEntity", meshA);
    nodeA->attachObject(entityA);

    auto* nodeB = sceneMgr->getRootSceneNode()->createChildSceneNode("walkNode");
    auto* entityB = sceneMgr->createEntity("walkEntity", meshB);
    nodeB->attachObject(entityB);

    QList<Ogre::Entity*> sources = {entityA, entityB};
    QString err;
    auto* merged = AnimationMerger::mergeAnimations(entityA, sources, err);

    ASSERT_NE(merged, nullptr) << err.toStdString();
    EXPECT_EQ(merged, entityA);

    // Base should still have its "idle" animation
    EXPECT_TRUE(merged->getMesh()->getSkeleton()->hasAnimation("idle"));
    // walkNode's "walk" animation should be prepended with slugified node name
    EXPECT_TRUE(merged->getMesh()->getSkeleton()->hasAnimation("walknode_walk"));

    // Cleanup
    nodeA->detachAllObjects();
    nodeB->detachAllObjects();
    sceneMgr->destroyEntity(entityA);
    sceneMgr->destroyEntity(entityB);
    sceneMgr->destroySceneNode(nodeA);
    sceneMgr->destroySceneNode(nodeB);
    Ogre::MeshManager::getSingleton().remove(meshA);
    Ogre::MeshManager::getSingleton().remove(meshB);
    Ogre::SkeletonManager::getSingleton().remove(skelA);
    Ogre::SkeletonManager::getSingleton().remove(skelB);
}

TEST_F(AnimationMergerTest, MergeAnimationsNameCollision)
{
    if (!canLoadMeshFiles())
        GTEST_SKIP() << "Skipping: cannot load mesh files in this environment";

    // Both skeletons have an animation that would result in the same name
    auto skelA = createTestSkeleton("collision_skel_a", {"root"}, {"idle"});
    auto skelB = createTestSkeleton("collision_skel_b", {"root"}, {"idle"});

    auto meshA = Ogre::MeshManager::getSingleton().create("collision_mesh_a",
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    meshA->_notifySkeleton(skelA);

    auto meshB = Ogre::MeshManager::getSingleton().create("collision_mesh_b",
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    meshB->_notifySkeleton(skelB);

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    // Name the node "idle" so it collides with base's "idle" animation
    auto* nodeA = sceneMgr->getRootSceneNode()->createChildSceneNode("collBaseNode");
    auto* entityA = sceneMgr->createEntity("collBaseEntity", meshA);
    nodeA->attachObject(entityA);

    auto* nodeB = sceneMgr->getRootSceneNode()->createChildSceneNode("idle");
    auto* entityB = sceneMgr->createEntity("collWalkEntity", meshB);
    nodeB->attachObject(entityB);

    QList<Ogre::Entity*> sources = {entityA, entityB};
    QString err;
    auto* merged = AnimationMerger::mergeAnimations(entityA, sources, err);

    ASSERT_NE(merged, nullptr) << err.toStdString();
    // Base keeps "idle". Source node is "idle", anim is "idle", so slug is "idle_idle".
    // No collision with base's "idle", so no _2 suffix needed.
    EXPECT_TRUE(merged->getMesh()->getSkeleton()->hasAnimation("idle"));
    EXPECT_TRUE(merged->getMesh()->getSkeleton()->hasAnimation("idle_idle"));

    // Cleanup
    nodeA->detachAllObjects();
    nodeB->detachAllObjects();
    sceneMgr->destroyEntity(entityA);
    sceneMgr->destroyEntity(entityB);
    sceneMgr->destroySceneNode(nodeA);
    sceneMgr->destroySceneNode(nodeB);
    Ogre::MeshManager::getSingleton().remove(meshA);
    Ogre::MeshManager::getSingleton().remove(meshB);
    Ogre::SkeletonManager::getSingleton().remove(skelA);
    Ogre::SkeletonManager::getSingleton().remove(skelB);
}

// Standalone test that doesn't need Ogre initialization
TEST(AnimationMergerStandaloneTest, MergeNoSkeletonError)
{
    QString err;
    auto* result = AnimationMerger::mergeAnimations(nullptr, {}, err);
    EXPECT_EQ(result, nullptr);
    EXPECT_FALSE(err.isEmpty());
}

TEST(AnimationMergerStandaloneTest, NullSkeletonCompatibility)
{
    Ogre::SkeletonPtr null;
    EXPECT_FALSE(AnimationMerger::areSkeletonsCompatible(null, null));
}
