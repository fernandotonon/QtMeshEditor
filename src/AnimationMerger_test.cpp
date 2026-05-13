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
#include <OgreKeyFrame.h>

class AnimationMergerTest : public ::testing::Test {
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
    ASSERT_TRUE(canLoadMeshFiles()) << "GL/hardware buffers required (Xvfb in CI)";

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

    // Meaningful names kept as-is (no prefix)
    EXPECT_TRUE(merged->getMesh()->getSkeleton()->hasAnimation("idle"));
    EXPECT_TRUE(merged->getMesh()->getSkeleton()->hasAnimation("walk"));

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
    ASSERT_TRUE(canLoadMeshFiles()) << "GL/hardware buffers required (Xvfb in CI)";

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
    // Both have meaningful name "idle" → base keeps "idle", source gets "idle_2"
    EXPECT_TRUE(merged->getMesh()->getSkeleton()->hasAnimation("idle"));
    EXPECT_TRUE(merged->getMesh()->getSkeleton()->hasAnimation("idle_2"));

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
    ASSERT_TRUE(canLoadMeshFiles()) << "GL/hardware buffers required (Xvfb in CI)";

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

    // "Armature|mixamo.com|Layer0" → cleaned → "Armature|Layer0" → meaningful → "armature_layer0"
    EXPECT_TRUE(skel->hasAnimation("armature_layer0"))
        << "Expected cleaned base animation";
    // "mixamo.com|walk" → cleaned → "walk" → meaningful → "walk"
    EXPECT_TRUE(skel->hasAnimation("walk"))
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
    ASSERT_TRUE(canLoadMeshFiles()) << "GL/hardware buffers required (Xvfb in CI)";

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

    // All three have meaningful name "idle" → dedup: "idle", "idle_2", "idle_3"
    EXPECT_TRUE(skel->hasAnimation("idle"));
    EXPECT_TRUE(skel->hasAnimation("idle_2"));
    EXPECT_TRUE(skel->hasAnimation("idle_3"));

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

TEST_F(AnimationMergerTest, MergeAnimationsUnrealTakeCleanup)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "GL/hardware buffers required (Xvfb in CI)";

    // Animation-only FBX from Unreal Engine retarget: animation is named "Unreal Take"
    // (with a space). After cleanup it should fall back to the skeleton/file name.
    auto skelBase = createTestSkeleton("ue_base_skel", {"root", "spine"}, {"idle"});
    auto skelAnim = createTestSkeleton("mm_attack_03.skeleton", {"root", "spine"}, {"Unreal Take"});

    auto meshBase = createInMemoryMesh("ue_base_mesh", skelBase);

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* nodeBase = sceneMgr->getRootSceneNode()->createChildSceneNode("ueBaseNode");
    auto* entityBase = sceneMgr->createEntity("ueBaseEntity", meshBase);
    nodeBase->attachObject(entityBase);

    QString err;
    auto* merged = AnimationMerger::mergeAnimations(entityBase, {}, {skelAnim}, err);

    ASSERT_NE(merged, nullptr) << err.toStdString();
    auto* skel = merged->getMesh()->getSkeleton().get();

    // "Unreal Take" cleaned to "" → falls back to skeleton name "mm_attack_03"
    EXPECT_TRUE(skel->hasAnimation("mm_attack_03"))
        << "Expected animation named after source file, not 'Unreal Take' or empty";
    EXPECT_FALSE(skel->hasAnimation("unreal_take"))
        << "Noise token should have been removed";
    EXPECT_FALSE(skel->hasAnimation(""))
        << "Animation name must not be empty";

    nodeBase->detachAllObjects();
    sceneMgr->destroyEntity(entityBase);
    sceneMgr->destroySceneNode(nodeBase);
    Ogre::MeshManager::getSingleton().remove(meshBase);
    Ogre::SkeletonManager::getSingleton().remove(skelBase);
    Ogre::SkeletonManager::getSingleton().remove(skelAnim);
}

TEST_F(AnimationMergerTest, MergeAnimationsNumericSuffixPreserved)
{
    ASSERT_TRUE(canLoadMeshFiles()) << "GL/hardware buffers required (Xvfb in CI)";

    // Intentional numeric suffix in file name must not be stripped by deduplication.
    // "mm_attack_03.skeleton" → animation should be "mm_attack_03", not "mm_attack".
    auto skelBase = createTestSkeleton("numsfx_base_skel", {"root"}, {"idle"});
    auto skelAnim = createTestSkeleton("mm_attack_03.skeleton", {"root"}, {"Unreal Take"});

    auto meshBase = createInMemoryMesh("numsfx_base_mesh", skelBase);

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* nodeBase = sceneMgr->getRootSceneNode()->createChildSceneNode("numsfxBaseNode");
    auto* entityBase = sceneMgr->createEntity("numsfxBaseEntity", meshBase);
    nodeBase->attachObject(entityBase);

    QString err;
    auto* merged = AnimationMerger::mergeAnimations(entityBase, {}, {skelAnim}, err);

    ASSERT_NE(merged, nullptr) << err.toStdString();
    auto* skel = merged->getMesh()->getSkeleton().get();

    EXPECT_TRUE(skel->hasAnimation("mm_attack_03"))
        << "Numeric suffix _03 should be preserved (not stripped to mm_attack)";
    EXPECT_FALSE(skel->hasAnimation("mm_attack"))
        << "Deduplication must not preemptively strip intentional _N suffixes";

    nodeBase->detachAllObjects();
    sceneMgr->destroyEntity(entityBase);
    sceneMgr->destroySceneNode(nodeBase);
    Ogre::MeshManager::getSingleton().remove(meshBase);
    Ogre::SkeletonManager::getSingleton().remove(skelBase);
    Ogre::SkeletonManager::getSingleton().remove(skelAnim);
}

TEST_F(AnimationMergerTest, ResampleAnimationBasic)
{
    // Create a skeleton with an animation that has 10 keyframes
    auto skel = Ogre::SkeletonManager::getSingleton().create(
        "resample_skel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* bone = skel->createBone("root", 0);
    auto* bone2 = skel->createBone("spine", 1);
    bone->addChild(bone2);
    skel->setBindingPose();

    auto* anim = skel->createAnimation("walk", 1.0f);

    // Track for root bone with 10 keyframes (linear translation)
    auto* track = anim->createNodeTrack(0);
    track->setAssociatedNode(bone);
    for (int i = 0; i < 10; ++i) {
        float t = i / 9.0f;
        auto* kf = track->createNodeKeyFrame(t);
        kf->setTranslate(Ogre::Vector3(t, 0, 0));
        kf->setRotation(Ogre::Quaternion::IDENTITY);
        kf->setScale(Ogre::Vector3::UNIT_SCALE);
    }

    // Track for spine bone with 10 keyframes
    auto* track2 = anim->createNodeTrack(1);
    track2->setAssociatedNode(bone2);
    for (int i = 0; i < 10; ++i) {
        float t = i / 9.0f;
        auto* kf = track2->createNodeKeyFrame(t);
        kf->setTranslate(Ogre::Vector3(0, t, 0));
        kf->setRotation(Ogre::Quaternion::IDENTITY);
        kf->setScale(Ogre::Vector3::UNIT_SCALE);
    }

    // Resample to 5 keyframes
    int removed = AnimationMerger::resampleAnimation(skel.get(), "walk", 5);
    EXPECT_EQ(removed, 5); // 10 original - 5 target = 5 removed

    // Verify the animation still exists and has 5 keyframes per track
    EXPECT_TRUE(skel->hasAnimation("walk"));
    auto* newAnim = skel->getAnimation("walk");
    EXPECT_FLOAT_EQ(newAnim->getLength(), 1.0f);

    for (const auto& [handle, newTrack] : newAnim->_getNodeTrackList()) {
        EXPECT_EQ(newTrack->getNumKeyFrames(), 5u);
        // First keyframe at t=0
        EXPECT_FLOAT_EQ(newTrack->getNodeKeyFrame(0)->getTime(), 0.0f);
        // Last keyframe at t=1.0
        EXPECT_FLOAT_EQ(newTrack->getNodeKeyFrame(4)->getTime(), 1.0f);
    }

    Ogre::SkeletonManager::getSingleton().remove(skel);
}

TEST_F(AnimationMergerTest, ResampleAnimationPreservesInterpolation)
{
    // The resampled keyframes should match interpolated values from the original
    auto skel = Ogre::SkeletonManager::getSingleton().create(
        "resample_interp_skel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* bone = skel->createBone("root", 0);
    skel->setBindingPose();

    auto* anim = skel->createAnimation("move", 1.0f);
    auto* track = anim->createNodeTrack(0);
    track->setAssociatedNode(bone);

    // Create keyframes with known positions: linear from (0,0,0) to (1,0,0)
    for (int i = 0; i < 11; ++i) {
        float t = i / 10.0f;
        auto* kf = track->createNodeKeyFrame(t);
        kf->setTranslate(Ogre::Vector3(t, 0, 0));
        kf->setRotation(Ogre::Quaternion::IDENTITY);
        kf->setScale(Ogre::Vector3::UNIT_SCALE);
    }

    // Resample to 3 keyframes: t=0, t=0.5, t=1.0
    AnimationMerger::resampleAnimation(skel.get(), "move", 3);

    auto* newAnim = skel->getAnimation("move");
    auto& trackList = newAnim->_getNodeTrackList();
    ASSERT_EQ(trackList.size(), 1u);

    auto* newTrack = trackList.begin()->second;
    ASSERT_EQ(newTrack->getNumKeyFrames(), 3u);

    // Check midpoint is interpolated correctly
    auto* midKf = newTrack->getNodeKeyFrame(1);
    EXPECT_NEAR(midKf->getTranslate().x, 0.5f, 0.01f);

    Ogre::SkeletonManager::getSingleton().remove(skel);
}

TEST_F(AnimationMergerTest, DecimateAnimationBasic)
{
    auto skel = Ogre::SkeletonManager::getSingleton().create(
        "decimate_skel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* bone = skel->createBone("root", 0);
    skel->setBindingPose();

    auto* anim = skel->createAnimation("run", 1.0f);
    auto* track = anim->createNodeTrack(0);
    track->setAssociatedNode(bone);

    // Create 10 keyframes
    for (int i = 0; i < 10; ++i) {
        float t = i / 9.0f;
        auto* kf = track->createNodeKeyFrame(t);
        kf->setTranslate(Ogre::Vector3(static_cast<float>(i), 0, 0));
        kf->setRotation(Ogre::Quaternion::IDENTITY);
        kf->setScale(Ogre::Vector3::UNIT_SCALE);
    }

    // Decimate with step=3: keep indices 0, 3, 6, 9 (last)
    int removed = AnimationMerger::decimateAnimation(skel.get(), "run", 3);
    EXPECT_EQ(removed, 6); // 10 - 4 = 6

    auto* newAnim = skel->getAnimation("run");
    auto& trackList = newAnim->_getNodeTrackList();
    ASSERT_EQ(trackList.size(), 1u);

    auto* newTrack = trackList.begin()->second;
    EXPECT_EQ(newTrack->getNumKeyFrames(), 4u);

    // Verify kept keyframe values (original translate.x was the index)
    EXPECT_FLOAT_EQ(newTrack->getNodeKeyFrame(0)->getTranslate().x, 0.0f); // index 0
    EXPECT_FLOAT_EQ(newTrack->getNodeKeyFrame(1)->getTranslate().x, 3.0f); // index 3
    EXPECT_FLOAT_EQ(newTrack->getNodeKeyFrame(2)->getTranslate().x, 6.0f); // index 6
    EXPECT_FLOAT_EQ(newTrack->getNodeKeyFrame(3)->getTranslate().x, 9.0f); // index 9 (last)

    Ogre::SkeletonManager::getSingleton().remove(skel);
}

TEST_F(AnimationMergerTest, DecimateAlwaysKeepsLast)
{
    auto skel = Ogre::SkeletonManager::getSingleton().create(
        "decimate_last_skel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);

    auto* bone = skel->createBone("root", 0);
    skel->setBindingPose();

    auto* anim = skel->createAnimation("jump", 1.0f);
    auto* track = anim->createNodeTrack(0);
    track->setAssociatedNode(bone);

    // 7 keyframes: keep 0, 5, 6 (last) with step=5
    for (int i = 0; i < 7; ++i) {
        float t = i / 6.0f;
        auto* kf = track->createNodeKeyFrame(t);
        kf->setTranslate(Ogre::Vector3(static_cast<float>(i), 0, 0));
        kf->setRotation(Ogre::Quaternion::IDENTITY);
        kf->setScale(Ogre::Vector3::UNIT_SCALE);
    }

    AnimationMerger::decimateAnimation(skel.get(), "jump", 5);

    auto* newTrack = skel->getAnimation("jump")->_getNodeTrackList().begin()->second;
    EXPECT_EQ(newTrack->getNumKeyFrames(), 3u); // 0, 5, 6(last)

    // Verify last keyframe is preserved
    auto* lastKf = newTrack->getNodeKeyFrame(newTrack->getNumKeyFrames() - 1);
    EXPECT_FLOAT_EQ(lastKf->getTranslate().x, 6.0f);

    Ogre::SkeletonManager::getSingleton().remove(skel);
}

TEST_F(AnimationMergerTest, ResampleNullSkeleton)
{
    // Edge case: null skeleton should return 0
    EXPECT_EQ(AnimationMerger::resampleAnimation(nullptr, "test", 5), 0);
}

TEST_F(AnimationMergerTest, DecimateNullSkeleton)
{
    EXPECT_EQ(AnimationMerger::decimateAnimation(nullptr, "test", 3), 0);
}

TEST_F(AnimationMergerTest, ResampleMissingAnimation)
{
    auto skel = Ogre::SkeletonManager::getSingleton().create(
        "resample_missing_skel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* bone = skel->createBone("root", 0);
    skel->setBindingPose();

    // No animations on this skeleton
    EXPECT_EQ(AnimationMerger::resampleAnimation(skel.get(), "nonexistent", 5), 0);

    Ogre::SkeletonManager::getSingleton().remove(skel);
}

TEST_F(AnimationMergerTest, DecimateStepTooSmall)
{
    auto skel = Ogre::SkeletonManager::getSingleton().create(
        "decimate_small_skel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* bone = skel->createBone("root", 0);
    skel->setBindingPose();
    skel->createAnimation("idle", 1.0f);

    // step < 2 should be a no-op
    EXPECT_EQ(AnimationMerger::decimateAnimation(skel.get(), "idle", 1), 0);
    EXPECT_EQ(AnimationMerger::decimateAnimation(skel.get(), "idle", 0), 0);

    Ogre::SkeletonManager::getSingleton().remove(skel);
}

TEST_F(AnimationMergerTest, SimplifyAnimationLinearMotionCollapses)
{
    // Linear translation across 11 evenly-spaced keys should collapse to 2.
    auto skel = Ogre::SkeletonManager::getSingleton().create(
        "simplify_linear_skel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* bone = skel->createBone("root", 0);
    skel->setBindingPose();

    auto* anim = skel->createAnimation("walk", 1.0f);
    auto* track = anim->createNodeTrack(0);
    track->setAssociatedNode(bone);
    for (int i = 0; i < 11; ++i) {
        float t = i / 10.0f;
        auto* kf = track->createNodeKeyFrame(t);
        kf->setTranslate(Ogre::Vector3(t, 0, 0));
        kf->setRotation(Ogre::Quaternion::IDENTITY);
        kf->setScale(Ogre::Vector3::UNIT_SCALE);
    }

    int removed = AnimationMerger::simplifyAnimation(skel.get(), "walk");
    EXPECT_EQ(removed, 9);

    auto* newTrack = skel->getAnimation("walk")->_getNodeTrackList().begin()->second;
    EXPECT_EQ(newTrack->getNumKeyFrames(), 2u);
    EXPECT_FLOAT_EQ(newTrack->getNodeKeyFrame(0)->getTime(), 0.0f);
    EXPECT_FLOAT_EQ(newTrack->getNodeKeyFrame(1)->getTime(), 1.0f);

    Ogre::SkeletonManager::getSingleton().remove(skel);
}

TEST_F(AnimationMergerTest, SimplifyAnimationKeepsNonLinearKeys)
{
    // V-shape (motion reverses at midpoint) cannot be collapsed past the apex.
    auto skel = Ogre::SkeletonManager::getSingleton().create(
        "simplify_vshape_skel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* bone = skel->createBone("root", 0);
    skel->setBindingPose();

    auto* anim = skel->createAnimation("bob", 1.0f);
    auto* track = anim->createNodeTrack(0);
    track->setAssociatedNode(bone);
    // Up at t=0, peak at t=0.5, back down at t=1.0
    for (int i = 0; i < 11; ++i) {
        float t = i / 10.0f;
        float y = (t < 0.5f) ? t : (1.0f - t); // tent function
        auto* kf = track->createNodeKeyFrame(t);
        kf->setTranslate(Ogre::Vector3(0, y, 0));
        kf->setRotation(Ogre::Quaternion::IDENTITY);
        kf->setScale(Ogre::Vector3::UNIT_SCALE);
    }

    AnimationMerger::simplifyAnimation(skel.get(), "bob");

    auto* newTrack = skel->getAnimation("bob")->_getNodeTrackList().begin()->second;
    // Should keep the apex — first/peak/last == 3 keys.
    EXPECT_EQ(newTrack->getNumKeyFrames(), 3u);
    EXPECT_FLOAT_EQ(newTrack->getNodeKeyFrame(1)->getTime(), 0.5f);
    EXPECT_NEAR(newTrack->getNodeKeyFrame(1)->getTranslate().y, 0.5f, 1e-5f);

    Ogre::SkeletonManager::getSingleton().remove(skel);
}

TEST_F(AnimationMergerTest, SimplifyAnimationToleranceControlsAggressiveness)
{
    // Slight noise added to a linear curve. Tight tolerance keeps everything;
    // loose tolerance collapses it.
    auto skel = Ogre::SkeletonManager::getSingleton().create(
        "simplify_tol_skel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* bone = skel->createBone("root", 0);
    skel->setBindingPose();

    auto* anim = skel->createAnimation("noisy", 1.0f);
    auto* track = anim->createNodeTrack(0);
    track->setAssociatedNode(bone);
    for (int i = 0; i < 11; ++i) {
        float t = i / 10.0f;
        // 5mm wobble around the linear path
        float jitter = (i % 2 == 0) ? 0.005f : -0.005f;
        auto* kf = track->createNodeKeyFrame(t);
        kf->setTranslate(Ogre::Vector3(t, jitter, 0));
        kf->setRotation(Ogre::Quaternion::IDENTITY);
        kf->setScale(Ogre::Vector3::UNIT_SCALE);
    }

    AnimationMerger::SimplifyTolerances tightTol;
    tightTol.translation = 1e-6f;
    int total = 0, redundantTight = 0;
    AnimationMerger::analyzeRedundantKeyframes(skel->getAnimation("noisy"),
                                               tightTol, &total, &redundantTight);
    EXPECT_EQ(redundantTight, 0);

    AnimationMerger::SimplifyTolerances looseTol;
    looseTol.translation = 0.1f;
    int totalLoose = 0, redundantLoose = 0;
    AnimationMerger::analyzeRedundantKeyframes(skel->getAnimation("noisy"),
                                               looseTol, &totalLoose, &redundantLoose);
    EXPECT_GT(redundantLoose, 5);

    Ogre::SkeletonManager::getSingleton().remove(skel);
}

TEST_F(AnimationMergerTest, SimplifyAnimationPreservesEndpoints)
{
    auto skel = Ogre::SkeletonManager::getSingleton().create(
        "simplify_endpoints_skel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* bone = skel->createBone("root", 0);
    skel->setBindingPose();

    auto* anim = skel->createAnimation("hold", 1.0f);
    auto* track = anim->createNodeTrack(0);
    track->setAssociatedNode(bone);
    // 5 identical keyframes — all middle ones are trivially redundant.
    for (int i = 0; i < 5; ++i) {
        auto* kf = track->createNodeKeyFrame(i / 4.0f);
        kf->setTranslate(Ogre::Vector3(0, 0, 0));
        kf->setRotation(Ogre::Quaternion::IDENTITY);
        kf->setScale(Ogre::Vector3::UNIT_SCALE);
    }

    AnimationMerger::simplifyAnimation(skel.get(), "hold");
    auto* newTrack = skel->getAnimation("hold")->_getNodeTrackList().begin()->second;
    EXPECT_EQ(newTrack->getNumKeyFrames(), 2u);
    EXPECT_FLOAT_EQ(newTrack->getNodeKeyFrame(0)->getTime(), 0.0f);
    EXPECT_FLOAT_EQ(newTrack->getNodeKeyFrame(1)->getTime(), 1.0f);

    Ogre::SkeletonManager::getSingleton().remove(skel);
}

TEST_F(AnimationMergerTest, SimplifyAnimationNullAndMissing)
{
    EXPECT_EQ(AnimationMerger::simplifyAnimation(nullptr, "x"), 0);

    auto skel = Ogre::SkeletonManager::getSingleton().create(
        "simplify_missing_skel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    skel->createBone("root", 0);
    skel->setBindingPose();
    EXPECT_EQ(AnimationMerger::simplifyAnimation(skel.get(), "nonexistent"), 0);

    Ogre::SkeletonManager::getSingleton().remove(skel);
}

TEST_F(AnimationMergerTest, SimplifyAnimationAntipodalRotation)
{
    // Quaternions q and -q represent the same rotation. The simplifier's
    // hemisphere alignment must treat them as equivalent so it doesn't
    // pretend a stationary rotation is "moving" via the long way around.
    auto skel = Ogre::SkeletonManager::getSingleton().create(
        "simplify_antipodal_skel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* bone = skel->createBone("root", 0);
    skel->setBindingPose();

    auto* anim = skel->createAnimation("turn", 1.0f);
    auto* track = anim->createNodeTrack(0);
    track->setAssociatedNode(bone);

    // Five keys whose rotations alternate q / -q (same orientation, flipped
    // representation). Translation/scale are constant, so the only way the
    // simplifier could keep middle keys is if it failed to align hemispheres.
    const Ogre::Quaternion q = Ogre::Quaternion(Ogre::Degree(45), Ogre::Vector3::UNIT_Y);
    for (int i = 0; i < 5; ++i) {
        float t = i / 4.0f;
        auto* kf = track->createNodeKeyFrame(t);
        kf->setTranslate(Ogre::Vector3::ZERO);
        kf->setScale(Ogre::Vector3::UNIT_SCALE);
        kf->setRotation((i % 2 == 0)
            ? q
            : Ogre::Quaternion(-q.w, -q.x, -q.y, -q.z));
    }

    AnimationMerger::simplifyAnimation(skel.get(), "turn");
    auto* newTrack = skel->getAnimation("turn")->_getNodeTrackList().begin()->second;
    EXPECT_EQ(newTrack->getNumKeyFrames(), 2u);

    Ogre::SkeletonManager::getSingleton().remove(skel);
}

TEST_F(AnimationMergerTest, SimplifyAnimationScaleOnlyTrack)
{
    // Scale tolerance has its own branch in the simplifier — exercise it
    // independently by holding translation/rotation constant and varying
    // only the scale.
    auto skel = Ogre::SkeletonManager::getSingleton().create(
        "simplify_scale_skel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* bone = skel->createBone("root", 0);
    skel->setBindingPose();

    auto* anim = skel->createAnimation("breathe", 1.0f);
    auto* track = anim->createNodeTrack(0);
    track->setAssociatedNode(bone);

    // Linear scale ramp from (1,1,1) to (2,1,1) with 11 keys — every middle
    // key sits exactly on the lerp.
    for (int i = 0; i < 11; ++i) {
        float t = i / 10.0f;
        auto* kf = track->createNodeKeyFrame(t);
        kf->setTranslate(Ogre::Vector3::ZERO);
        kf->setRotation(Ogre::Quaternion::IDENTITY);
        kf->setScale(Ogre::Vector3(1.0f + t, 1.0f, 1.0f));
    }

    AnimationMerger::SimplifyTolerances tightTol;
    tightTol.scale = 1e-6f;
    int total = 0, redundant = 0;
    AnimationMerger::analyzeRedundantKeyframes(skel->getAnimation("breathe"),
                                               tightTol, &total, &redundant);
    EXPECT_GT(redundant, 0); // perfect linear ramp collapses regardless of tolerance

    // Add 1% scale wobble — tight tolerance must keep it.
    skel->removeAnimation("breathe");
    auto* anim2 = skel->createAnimation("breathe", 1.0f);
    auto* track2 = anim2->createNodeTrack(0);
    track2->setAssociatedNode(bone);
    for (int i = 0; i < 11; ++i) {
        float t = i / 10.0f;
        float wobble = (i % 2 == 0) ? 0.01f : -0.01f;
        auto* kf = track2->createNodeKeyFrame(t);
        kf->setTranslate(Ogre::Vector3::ZERO);
        kf->setRotation(Ogre::Quaternion::IDENTITY);
        kf->setScale(Ogre::Vector3(1.0f + t + wobble, 1.0f, 1.0f));
    }
    int totalT = 0, redundantTight = 0;
    AnimationMerger::analyzeRedundantKeyframes(skel->getAnimation("breathe"),
                                               tightTol, &totalT, &redundantTight);
    EXPECT_EQ(redundantTight, 0);

    AnimationMerger::SimplifyTolerances looseTol;
    looseTol.scale = 0.1f;
    int totalL = 0, redundantLoose = 0;
    AnimationMerger::analyzeRedundantKeyframes(skel->getAnimation("breathe"),
                                               looseTol, &totalL, &redundantLoose);
    EXPECT_GT(redundantLoose, 5);

    Ogre::SkeletonManager::getSingleton().remove(skel);
}

TEST(AnimationMergerStandaloneTest, TolerancesForPresetMapping)
{
    // The shared preset table is the contract between CLI / MCP / Inspector;
    // pin the values so a future tweak forces an explicit update everywhere.
    auto cons = AnimationMerger::tolerancesForPreset("conservative");
    EXPECT_FLOAT_EQ(cons.translation, 1e-4f);
    EXPECT_FLOAT_EQ(cons.rotationDeg, 0.05f);

    auto bal = AnimationMerger::tolerancesForPreset("balanced");
    EXPECT_FLOAT_EQ(bal.translation, 1e-3f);
    EXPECT_FLOAT_EQ(bal.rotationDeg, 0.5f);

    auto agg = AnimationMerger::tolerancesForPreset("aggressive");
    EXPECT_FLOAT_EQ(agg.translation, 1e-2f);
    EXPECT_FLOAT_EQ(agg.rotationDeg, 1.0f);

    // Empty string falls back to conservative (the safe default — simplify
    // is destructive) and is reported as OK. Assert the full tuple — a
    // partial check would let rotationDeg / scale regress silently.
    bool ok = false;
    auto def = AnimationMerger::tolerancesForPreset("", &ok);
    EXPECT_TRUE(ok);
    EXPECT_FLOAT_EQ(def.translation, 1e-4f);
    EXPECT_FLOAT_EQ(def.rotationDeg, 0.05f);
    EXPECT_FLOAT_EQ(def.scale,       1e-4f);

    // Unknown preset reports !ok but still returns conservative defaults
    bool ok2 = true;
    auto bad = AnimationMerger::tolerancesForPreset("garbage", &ok2);
    EXPECT_FALSE(ok2);
    EXPECT_FLOAT_EQ(bad.translation, 1e-4f);
    EXPECT_FLOAT_EQ(bad.rotationDeg, 0.05f);
    EXPECT_FLOAT_EQ(bad.scale,       1e-4f);

    // Case-insensitive
    bool ok3 = false;
    auto upper = AnimationMerger::tolerancesForPreset("AGGRESSIVE", &ok3);
    EXPECT_TRUE(ok3);
    EXPECT_FLOAT_EQ(upper.translation, 1e-2f);
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
