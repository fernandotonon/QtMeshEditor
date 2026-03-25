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
#include <OgreHardwareBufferManager.h>

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

    // Helper: create an in-memory mesh with a minimal triangle so Ogre
    // doesn't try to load the resource from disk when creating an Entity.
    Ogre::MeshPtr createInMemoryMesh(const std::string& name,
                                      const Ogre::SkeletonPtr& skel)
    {
        auto mesh = Ogre::MeshManager::getSingleton().createManual(
            name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

        auto* sub = mesh->createSubMesh();
        mesh->sharedVertexData = new Ogre::VertexData();
        auto* decl = mesh->sharedVertexData->vertexDeclaration;
        decl->addElement(0, 0, Ogre::VET_FLOAT3, Ogre::VES_POSITION);

        auto vbuf = Ogre::HardwareBufferManager::getSingleton().createVertexBuffer(
            decl->getVertexSize(0), 3, Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        float verts[] = {0,0,0, 1,0,0, 0,1,0};
        vbuf->writeData(0, sizeof(verts), verts);
        mesh->sharedVertexData->vertexBufferBinding->setBinding(0, vbuf);
        mesh->sharedVertexData->vertexCount = 3;

        auto ibuf = Ogre::HardwareBufferManager::getSingleton().createIndexBuffer(
            Ogre::HardwareIndexBuffer::IT_16BIT, 3,
            Ogre::HardwareBuffer::HBU_STATIC_WRITE_ONLY);
        uint16_t idx[] = {0, 1, 2};
        ibuf->writeData(0, sizeof(idx), idx);
        sub->useSharedVertices = true;
        sub->indexData->indexBuffer = ibuf;
        sub->indexData->indexCount = 3;

        mesh->_notifySkeleton(skel);
        mesh->_setBounds(Ogre::AxisAlignedBox(-1,-1,-1,1,1,1));
        mesh->_setBoundingSphereRadius(1.0);
        mesh->load();

        return mesh;
    }

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
    // Create two meshes sharing compatible skeletons
    auto skelA = createTestSkeleton("merge_skel_a", {"root", "spine"}, {"idle"});
    auto skelB = createTestSkeleton("merge_skel_b", {"root", "spine"}, {"walk"});

    auto meshA = createInMemoryMesh("merge_mesh_a", skelA);
    auto meshB = createInMemoryMesh("merge_mesh_b", skelB);

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

    // Base "idle" is now prefixed: "basenode_idle"
    EXPECT_TRUE(merged->getMesh()->getSkeleton()->hasAnimation("basenode_idle"));
    // walkNode's "walk" → "walknode_walk"
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
    // Both skeletons have an animation that would result in the same name
    auto skelA = createTestSkeleton("collision_skel_a", {"root"}, {"idle"});
    auto skelB = createTestSkeleton("collision_skel_b", {"root"}, {"idle"});

    auto meshA = createInMemoryMesh("collision_mesh_a", skelA);
    auto meshB = createInMemoryMesh("collision_mesh_b", skelB);

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
    // Base node "collBaseNode" + anim "idle" → "collbasenode_idle"
    // Source node "idle" + anim "idle" → "idle" (dedup: same slug, no duplication)
    // But "idle" would collide if another had it, so it gets _2 if needed
    EXPECT_TRUE(merged->getMesh()->getSkeleton()->hasAnimation("collbasenode_idle"));
    // Source node "idle" + anim "idle" → buildAnimName returns "idle" (no duplication)
    EXPECT_TRUE(merged->getMesh()->getSkeleton()->hasAnimation("idle"));

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

TEST_F(AnimationMergerTest, MergeAnimationsMixamoCleanup)
{
    // Animations with "mixamo.com" in their names should be cleaned
    auto skelA = createTestSkeleton("mixamo_skel_a", {"root", "spine"},
                                     {"Armature|mixamo.com|Layer0"});
    auto skelB = createTestSkeleton("mixamo_skel_b", {"root", "spine"},
                                     {"mixamo.com|walk"});

    auto meshA = createInMemoryMesh("mixamo_mesh_a", skelA);
    auto meshB = createInMemoryMesh("mixamo_mesh_b", skelB);

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* nodeA = sceneMgr->getRootSceneNode()->createChildSceneNode("character");
    auto* entityA = sceneMgr->createEntity("mixamoBaseEntity", meshA);
    nodeA->attachObject(entityA);

    auto* nodeB = sceneMgr->getRootSceneNode()->createChildSceneNode("walkAnim");
    auto* entityB = sceneMgr->createEntity("mixamoWalkEntity", meshB);
    nodeB->attachObject(entityB);

    QList<Ogre::Entity*> sources = {entityA, entityB};
    QString err;
    auto* merged = AnimationMerger::mergeAnimations(entityA, sources, err);

    ASSERT_NE(merged, nullptr) << err.toStdString();
    auto* skel = merged->getMesh()->getSkeleton().get();

    // "Armature|mixamo.com|Layer0" → cleaned → "Armature|Layer0" → "character_armature_layer0"
    EXPECT_TRUE(skel->hasAnimation("character_armature_layer0"))
        << "Expected cleaned base animation";
    // "mixamo.com|walk" → cleaned → "walk" → "walkanim_walk"
    EXPECT_TRUE(skel->hasAnimation("walkanim_walk"))
        << "Expected cleaned source animation";

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

TEST_F(AnimationMergerTest, MergeAnimationsDeduplication)
{
    // When prefix == anim name, avoid "idle_idle" — just use "idle".
    // When two sources produce the same final name, append _2.
    auto skelA = createTestSkeleton("dedup_skel_a", {"root"}, {"idle"});
    auto skelB = createTestSkeleton("dedup_skel_b", {"root"}, {"idle"});
    auto skelC = createTestSkeleton("dedup_skel_c", {"root"}, {"idle"});

    auto meshA = createInMemoryMesh("dedup_mesh_a", skelA);
    auto meshB = createInMemoryMesh("dedup_mesh_b", skelB);
    auto meshC = createInMemoryMesh("dedup_mesh_c", skelC);

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* nodeA = sceneMgr->getRootSceneNode()->createChildSceneNode("idle");
    auto* entityA = sceneMgr->createEntity("dedupBaseEntity", meshA);
    nodeA->attachObject(entityA);

    auto* nodeB = sceneMgr->getRootSceneNode()->createChildSceneNode("idle2");
    auto* entityB = sceneMgr->createEntity("dedupSrc1Entity", meshB);
    nodeB->attachObject(entityB);

    auto* nodeC = sceneMgr->getRootSceneNode()->createChildSceneNode("idle3");
    auto* entityC = sceneMgr->createEntity("dedupSrc2Entity", meshC);
    nodeC->attachObject(entityC);

    QList<Ogre::Entity*> sources = {entityA, entityB, entityC};
    QString err;
    auto* merged = AnimationMerger::mergeAnimations(entityA, sources, err);

    ASSERT_NE(merged, nullptr) << err.toStdString();
    auto* skel = merged->getMesh()->getSkeleton().get();

    // Base: node="idle" + anim="idle" → "idle"
    EXPECT_TRUE(skel->hasAnimation("idle"));
    // Source 1: node="idle2" + anim="idle" → "idle2" (prefix != anim, but anim is subprefix)
    // Actually: buildAnimName("idle2", "idle") → slugPrefix="idle2", slugAnim="idle" → "idle2_idle"
    EXPECT_TRUE(skel->hasAnimation("idle2_idle"));
    // Source 2: node="idle3" + anim="idle" → "idle3_idle"
    EXPECT_TRUE(skel->hasAnimation("idle3_idle"));

    nodeA->detachAllObjects();
    nodeB->detachAllObjects();
    nodeC->detachAllObjects();
    sceneMgr->destroyEntity(entityA);
    sceneMgr->destroyEntity(entityB);
    sceneMgr->destroyEntity(entityC);
    sceneMgr->destroySceneNode(nodeA);
    sceneMgr->destroySceneNode(nodeB);
    sceneMgr->destroySceneNode(nodeC);
    Ogre::MeshManager::getSingleton().remove(meshA);
    Ogre::MeshManager::getSingleton().remove(meshB);
    Ogre::MeshManager::getSingleton().remove(meshC);
    Ogre::SkeletonManager::getSingleton().remove(skelA);
    Ogre::SkeletonManager::getSingleton().remove(skelB);
    Ogre::SkeletonManager::getSingleton().remove(skelC);
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
