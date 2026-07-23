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
#include "MotionInbetween.h"

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

    // #854 arm-space helpers. Build a T-posed humanoid (torso + both arm
    // chains + legs for the Ct frame), wrap it in a mesh + Entity, and return
    // the Entity so tests drive the SkeletonInstance (safe to apply()) — a
    // bare Skeleton SIGSEGVs on _updateTransforms.
    Ogre::Entity* makeArmRigEntity(const std::string& name)
    {
        auto skel = Ogre::SkeletonManager::getSingleton().create(
            name + "_skel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        unsigned short h = 0;
        auto bone = [&](const char* n, const Ogre::Vector3& p, Ogre::Bone* parent) {
            auto* b = skel->createBone(n, h++);
            b->setPosition(p);
            if (parent) parent->addChild(b);
            return b;
        };
        auto* hips  = bone("Hips",  {0, 1.0f, 0}, nullptr);
        auto* chest = bone("Spine", {0, 0.3f, 0}, hips);
        bone("Head", {0, 0.4f, 0}, chest);
        bone("LeftUpLeg",  {0.15f, -0.1f, 0}, hips);
        bone("RightUpLeg", {-0.15f, -0.1f, 0}, hips);
        auto* rsh = bone("RightArm", {-0.2f, 0.1f, 0}, chest);
        bone("RightForeArm", {-0.3f, 0, 0}, rsh);
        auto* lsh = bone("LeftArm", {0.2f, 0.1f, 0}, chest);
        bone("LeftForeArm", {0.3f, 0, 0}, lsh);
        skel->setBindingPose();
        auto* anim = skel->createAnimation("clip", 1.0f);
        for (unsigned short i = 0; i < skel->getNumBones(); ++i) {
            auto* trk = anim->createNodeTrack(i, skel->getBone(i));
            trk->createNodeKeyFrame(0.0f);
            trk->createNodeKeyFrame(1.0f);
        }
        auto mesh = createInMemoryMesh(name + "_mesh", skel);
        auto* sm = Manager::getSingleton()->getSceneMgr();
        return sm->createEntity(name + "_ent", mesh);
    }

    // World direction of a bone toward its child, at t=0.5 of `anim`.
    // NB: Ogre's Animation::apply ACCUMULATES onto the current pose (it calls
    // node->rotate, not set), so reset to bind first — otherwise each
    // measurement compounds the previous apply and the pose appears to lag.
    static Ogre::Vector3 armWorldDir(Ogre::SkeletonInstance* skel,
                                     const char* boneName, const char* childName,
                                     const char* anim = "clip")
    {
        skel->reset(true);
        skel->getAnimation(anim)->apply(skel, 0.5f);
        skel->_updateTransforms();
        const Ogre::Vector3 a = skel->getBone(boneName)->_getDerivedPosition();
        const Ogre::Vector3 b = skel->getBone(childName)->_getDerivedPosition();
        return (b - a).normalisedCopy();
    }
    static float degBetween(const Ogre::Vector3& a, const Ogre::Vector3& b)
    {
        return std::acos(std::min(1.0f, std::max(-1.0f, a.dotProduct(b))))
               * 180.0f / static_cast<float>(M_PI);
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

// ── #839: rig→canonical clip extraction ─────────────────────────────────────

TEST_F(AnimationMergerTest, ExtractCanonicalClipsRejectsNonHumanoid)
{
    // Root/Child bone names resolve no canonical roles → empty result.
    Ogre::Entity* ent = createAnimatedTestEntity("extract_nonhuman");
    ASSERT_NE(ent, nullptr);
    EXPECT_TRUE(AnimationMerger::extractCanonicalClips(ent).empty());
}

TEST_F(AnimationMergerTest, ExtractCanonicalClipsSamplesWorldFrame)
{
    // Minimal humanoid: Mixamo-style names resolve hip/head/lhip/rhip roles.
    auto skel = Ogre::SkeletonManager::getSingleton().create(
        "extract_skel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    auto* hips = skel->createBone("Hips", 0);
    hips->setPosition(Ogre::Vector3(0, 1, 0));
    auto* head = skel->createBone("Head", 1);
    head->setPosition(Ogre::Vector3(0, 0.7f, 0));
    hips->addChild(head);
    auto* lleg = skel->createBone("LeftUpLeg", 2);
    lleg->setPosition(Ogre::Vector3(0.15f, -0.1f, 0));
    hips->addChild(lleg);
    auto* rleg = skel->createBone("RightUpLeg", 3);
    rleg->setPosition(Ogre::Vector3(-0.15f, -0.1f, 0));
    hips->addChild(rleg);
    skel->setBindingPose();

    auto* anim = skel->createAnimation("Spin", 1.0f);
    auto* track = anim->createNodeTrack(1, head);
    track->createNodeKeyFrame(0.0f);
    auto* k1 = static_cast<Ogre::TransformKeyFrame*>(
        track->createNodeKeyFrame(1.0f));
    k1->setRotation(Ogre::Quaternion(Ogre::Degree(90),
                                     Ogre::Vector3::UNIT_Y));

    auto mesh = createInMemoryMesh("extract_mesh", skel);
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    Ogre::Entity* ent = sceneMgr->createEntity("extract_ent", mesh);

    const auto clips = AnimationMerger::extractCanonicalClips(ent, 30);
    ASSERT_EQ(clips.size(), 1u);
    const auto& c = clips.front();
    EXPECT_EQ(c.animation, QStringLiteral("Spin"));
    EXPECT_EQ(c.resolvedRoles, 4);
    EXPECT_EQ(c.frames, 31);                       // 1s @ 30fps inclusive
    ASSERT_EQ(static_cast<int>(c.quats.size()), c.frames);
    for (const auto& pose : c.quats)
        ASSERT_EQ(pose.size(),
                  static_cast<size_t>(MotionInbetween::canonicalJointCount()));

    // Unresolved roles (e.g. chest, index 2) stay identity in every frame.
    const auto& idq = c.quats.front()[2];
    EXPECT_FLOAT_EQ(idq[0], 0.f); EXPECT_FLOAT_EQ(idq[3], 1.f);

    // The animated head (role 5) actually rotates ~90° about the up axis
    // between first and last frame — world-frame delta, conjugation-safe.
    const auto& h0 = c.quats.front()[5];
    const auto& h1 = c.quats.back()[5];
    const Ogre::Quaternion q0(h0[3], h0[0], h0[1], h0[2]);
    const Ogre::Quaternion q1(h1[3], h1[0], h1[1], h1[2]);
    const Ogre::Quaternion d = q1 * q0.Inverse();
    const double ang = 2.0 * std::acos(std::min(1.0,
        static_cast<double>(std::abs(d.w)))) * 180.0 / M_PI;
    EXPECT_NEAR(ang, 90.0, 5.0);

    sceneMgr->destroyEntity(ent);
}

// ── #854: arm-space post-process ────────────────────────────────────────────

// These run as TEST_F on the AnimationMergerTest fixture so they can reuse
// createInMemoryMesh — a bare Ogre::Skeleton is NOT safe to apply()/
// _updateTransforms() on (it SIGSEGVs); the runtime always drives a
// SkeletonInstance obtained from a loaded Entity, so the tests do the same.
TEST_F(AnimationMergerTest, ArmSpaceWidensAndTucksArms)
{
    Ogre::Entity* ent = makeArmRigEntity("armspace_widen");
    ASSERT_NE(ent, nullptr);
    Ogre::SkeletonInstance* skel = ent->getSkeleton();

    const Ogre::Vector3 rBase = armWorldDir(skel, "RightArm", "RightForeArm");
    const Ogre::Vector3 lBase = armWorldDir(skel, "LeftArm", "LeftForeArm");

    // +30° swings each arm ~30° about the torso forward axis (widen).
    ASSERT_TRUE(AnimationMerger::adjustArmSpace(skel, "clip", 30.0f));
    const Ogre::Vector3 rWide = armWorldDir(skel, "RightArm", "RightForeArm");
    const Ogre::Vector3 lWide = armWorldDir(skel, "LeftArm", "LeftForeArm");
    EXPECT_NEAR(degBetween(rBase, rWide), 30.0f, 5.0f);
    EXPECT_NEAR(degBetween(lBase, lWide), 30.0f, 5.0f);

    // MIRROR check — the swing must be mirrored across the sagittal (X=0)
    // plane, not applied the same way to both arms (that would rotate the
    // whole shoulder line rigidly instead of spreading). Rotating about the
    // forward (Z) axis moves each arm in the X-Y plane; a per-side sign
    // regression shows up as the two arms picking up OPPOSITE-signed Y
    // (one lifts, one drops) instead of the same sign. Reflecting the right
    // result's X must match the left result (both components), which only
    // holds when the per-side rotation signs are correct.
    const Ogre::Vector3 rMirrored(-rWide.x, rWide.y, rWide.z);
    EXPECT_GT(rMirrored.dotProduct(lWide), 0.98f)
        << "arms did not swing symmetrically (per-side sign regression): "
        << "R=(" << rWide.x << "," << rWide.y << "," << rWide.z << ") "
        << "L=(" << lWide.x << "," << lWide.y << "," << lWide.z << ")";
    // The arms stay on their own sides (right −X, left +X) throughout.
    EXPECT_LT(rWide.x, 0.0f);
    EXPECT_GT(lWide.x, 0.0f);
    // Symmetric magnitude: |Δ| the same on both sides.
    EXPECT_NEAR(degBetween(rBase, rWide), degBetween(lBase, lWide), 1.0f);
}

TEST_F(AnimationMergerTest, ArmSpaceIsIdempotentAndAbsolute)
{
    Ogre::Entity* ent = makeArmRigEntity("armspace_idem");
    ASSERT_NE(ent, nullptr);
    Ogre::SkeletonInstance* skel = ent->getSkeleton();
    const Ogre::Vector3 base = armWorldDir(skel, "RightArm", "RightForeArm");

    // 20 then 10 == 10 from the original (absolute, reverts the prior value).
    AnimationMerger::adjustArmSpace(skel, "clip", 20.0f);
    AnimationMerger::adjustArmSpace(skel, "clip", 10.0f);
    const Ogre::Vector3 at10 = armWorldDir(skel, "RightArm", "RightForeArm");
    EXPECT_FLOAT_EQ(AnimationMerger::currentArmSpace(skel, "clip"), 10.0f);
    EXPECT_NEAR(degBetween(base, at10), 10.0f, 1.5f);   // net 10°, not 30°

    Ogre::Entity* ref = makeArmRigEntity("armspace_idem_ref");
    ASSERT_NE(ref, nullptr);
    AnimationMerger::adjustArmSpace(ref->getSkeleton(), "clip", 10.0f);
    const Ogre::Vector3 ref10 =
        armWorldDir(ref->getSkeleton(), "RightArm", "RightForeArm");
    EXPECT_GT(at10.dotProduct(ref10), 0.9999f);

    // Back to 0 restores the original pose bit-near-exactly.
    AnimationMerger::adjustArmSpace(skel, "clip", 0.0f);
    EXPECT_FLOAT_EQ(AnimationMerger::currentArmSpace(skel, "clip"), 0.0f);
    EXPECT_GT(base.dotProduct(armWorldDir(skel, "RightArm", "RightForeArm")),
              0.9999f);
}

TEST_F(AnimationMergerTest, ArmSpaceLeavesNonArmBonesUntouched)
{
    Ogre::Entity* ent = makeArmRigEntity("armspace_legs");
    ASSERT_NE(ent, nullptr);
    Ogre::SkeletonInstance* skel = ent->getSkeleton();
    const Ogre::Vector3 legBase = armWorldDir(skel, "Hips", "LeftUpLeg");
    const Ogre::Vector3 spineBase = armWorldDir(skel, "Hips", "Spine");

    AnimationMerger::adjustArmSpace(skel, "clip", 40.0f);
    EXPECT_GT(legBase.dotProduct(armWorldDir(skel, "Hips", "LeftUpLeg")),
              0.99999f);
    EXPECT_GT(spineBase.dotProduct(armWorldDir(skel, "Hips", "Spine")),
              0.99999f);
}

TEST_F(AnimationMergerTest, ArmSpaceNoOpWhenAnimationMissing)
{
    Ogre::Entity* ent = makeArmRigEntity("armspace_missing");
    ASSERT_NE(ent, nullptr);
    EXPECT_FALSE(AnimationMerger::adjustArmSpace(ent->getSkeleton(), "nope", 20.0f));
    EXPECT_FALSE(AnimationMerger::adjustArmSpace(nullptr, "clip", 20.0f));
}

TEST_F(AnimationMergerTest, ArmSpaceFollowsAnimationRename)
{
    Ogre::Entity* ent = makeArmRigEntity("armspace_rename");
    ASSERT_NE(ent, nullptr);
    Ogre::SkeletonInstance* skel = ent->getSkeleton();

    AnimationMerger::adjustArmSpace(skel, "clip", 25.0f);
    ASSERT_FLOAT_EQ(AnimationMerger::currentArmSpace(skel, "clip"), 25.0f);

    // Rename the clip — the tracked angle must move with it (and the old key
    // must be gone), so a UI targeting the renamed clip sees 25, not 0.
    AnimationMerger::renameAnimation(skel, "clip", "walk_wide");
    EXPECT_FLOAT_EQ(AnimationMerger::currentArmSpace(skel, "walk_wide"), 25.0f);
    EXPECT_FLOAT_EQ(AnimationMerger::currentArmSpace(skel, "clip"), 0.0f);

    // And a follow-up adjust on the new name computes its delta from 25:
    // going back to 0 restores the bind pose (would over-rotate if it thought
    // the clip were at 0).
    const Ogre::Vector3 wide = armWorldDir(skel, "RightArm", "RightForeArm",
                                           "walk_wide");
    AnimationMerger::adjustArmSpace(skel, "walk_wide", 0.0f);
    const Ogre::Vector3 neutral = armWorldDir(skel, "RightArm", "RightForeArm",
                                              "walk_wide");
    EXPECT_GT(degBetween(wide, neutral), 15.0f);   // it actually moved back
}

// ── #857: twist transport in the bind-referenced direction retarget ─────────

namespace {
// Canonical-clip inputs for a virtual SOURCE rig whose bind is rotated 90°
// about Z (a deliberately foreign convention — exercises the restWorld
// conjugation): restDir = clean canonical T-pose directions, restWorld = the
// same non-identity quat everywhere, and identity motion W(f) = restQ.
constexpr int kJc = 22;
const Ogre::Quaternion kSrcRest(Ogre::Degree(90), Ogre::Vector3::UNIT_Z);

std::vector<std::array<float, 3>> canonRestDirs()
{
    static const float d[kJc][3] = {
        {0,1,0},{0,1,0},{0,1,0},{0,1,0},{0,1,0},{0,1,0},
        {-1,0,0},{-1,0,0},{-1,0,0},{-1,0,0},
        {1,0,0},{1,0,0},{1,0,0},{1,0,0},
        {0,-1,0},{0,-1,0},{0,-1,0},{0,0,1},
        {0,-1,0},{0,-1,0},{0,-1,0},{0,0,1}};
    std::vector<std::array<float, 3>> out(kJc);
    for (int c = 0; c < kJc; ++c) out[c] = {d[c][0], d[c][1], d[c][2]};
    return out;
}

std::vector<std::array<float, 4>> srcRestWorld()
{
    return std::vector<std::array<float, 4>>(
        kJc, {kSrcRest.x, kSrcRest.y, kSrcRest.z, kSrcRest.w});
}

// frames of identity motion (every joint sits at the source bind)
std::vector<std::vector<std::array<float, 4>>> identityClip(int frames)
{
    return std::vector<std::vector<std::array<float, 4>>>(
        frames, std::vector<std::array<float, 4>>(
                    kJc, {kSrcRest.x, kSrcRest.y, kSrcRest.z, kSrcRest.w}));
}

// signed rotation angle of world-orientation delta `q` about unit axis `ax`
float twistDegAbout(const Ogre::Quaternion& q, const Ogre::Vector3& ax)
{
    const float s = q.x * ax.x + q.y * ax.y + q.z * ax.z;
    return 2.0f * std::atan2(s, q.w) * 180.0f / static_cast<float>(M_PI);
}
}  // namespace

TEST_F(AnimationMergerTest, TwistTransportCarriesBoneRoll)
{
    // makeArmRigEntity resolves only 9/22 roles — below applyMotionClip's
    // humanoid gate (>= 11) — so build a fuller rig (13 roles: + hands/feet).
    auto skelRes = Ogre::SkeletonManager::getSingleton().create(
        "twist_roll_skel",
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    unsigned short h = 0;
    auto bone = [&](const char* n, const Ogre::Vector3& p, Ogre::Bone* par) {
        auto* b = skelRes->createBone(n, h++);
        b->setPosition(p);
        if (par) par->addChild(b);
        return b;
    };
    auto* hips = bone("Hips", {0, 1.0f, 0}, nullptr);
    auto* spine = bone("Spine", {0, 0.3f, 0}, hips);
    bone("Head", {0, 0.4f, 0}, spine);
    auto* lleg = bone("LeftUpLeg", {0.15f, -0.1f, 0}, hips);
    bone("LeftFoot", {0, -0.8f, 0}, lleg);
    auto* rleg = bone("RightUpLeg", {-0.15f, -0.1f, 0}, hips);
    bone("RightFoot", {0, -0.8f, 0}, rleg);
    auto* rsh = bone("RightArm", {-0.2f, 0.1f, 0}, spine);
    auto* rfa = bone("RightForeArm", {-0.3f, 0, 0}, rsh);
    bone("RightHand", {-0.25f, 0, 0}, rfa);
    auto* lsh = bone("LeftArm", {0.2f, 0.1f, 0}, spine);
    auto* lfa = bone("LeftForeArm", {0.3f, 0, 0}, lsh);
    bone("LeftHand", {0.25f, 0, 0}, lfa);
    skelRes->setBindingPose();
    auto mesh = createInMemoryMesh("twist_roll_mesh", skelRes);
    auto* sm = Manager::getSingleton()->getSceneMgr();
    Ogre::Entity* ent = sm->createEntity("twist_roll_ent", mesh);
    ASSERT_NE(ent, nullptr);
    Ogre::SkeletonInstance* skel = ent->getSkeleton();

    // Source: left upper arm (role 11, +X) ROLLS 60° about its own axis over
    // the clip while its direction stays put. Pre-#857 the retarget dropped
    // this entirely (aim-only) — the target bone never moved.
    const int frames = 31;
    auto quats = identityClip(frames);
    for (int f = 0; f < frames; ++f) {
        const float a = 60.0f * static_cast<float>(f) / (frames - 1);
        const Ogre::Quaternion w =
            Ogre::Quaternion(Ogre::Degree(a), Ogre::Vector3::UNIT_X)
            * kSrcRest;
        quats[f][11] = {w.x, w.y, w.z, w.w};
    }
    const auto res = AnimationMerger::applyMotionClip(
        skel, "twistclip", quats, 30, /*worldFrame=*/true, srcRestWorld(),
        false, 8, false, canonRestDirs());
    ASSERT_TRUE(res.ok) << res.error.toStdString();

    // Direction is invariant under a pure roll…
    const Ogre::Vector3 dir0(1, 0, 0);
    skel->reset(true);
    skel->getAnimation("twistclip")->apply(skel, 1.0f);
    skel->_updateTransforms();
    const Ogre::Vector3 a =
        skel->getBone("LeftArm")->_getDerivedPosition();
    const Ogre::Vector3 b =
        skel->getBone("LeftForeArm")->_getDerivedPosition();
    EXPECT_GT((b - a).normalisedCopy().dotProduct(dir0), 0.99f);

    // …but the bone's world orientation now carries the 60° roll about it.
    const Ogre::Quaternion w =
        skel->getBone("LeftArm")->_getDerivedOrientation();
    EXPECT_NEAR(twistDegAbout(w, dir0), 60.0f, 4.0f);

    // Legs saw identity source motion — they must not pick up any rotation.
    const Ogre::Quaternion leg =
        skel->getBone("LeftUpLeg")->_getDerivedOrientation();
    EXPECT_NEAR(std::abs(leg.w), 1.0f, 1e-3f);

    sm->destroyEntity(ent);
}

TEST_F(AnimationMergerTest, TwistUnwrapKeepsDampedCollarContinuous)
{
    // Rig WITH clavicles (Mixamo "Shoulder" → collar roles 6/10).
    auto skelRes = Ogre::SkeletonManager::getSingleton().create(
        "twist_collar_skel",
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    unsigned short h = 0;
    auto bone = [&](const char* n, const Ogre::Vector3& p, Ogre::Bone* par) {
        auto* b = skelRes->createBone(n, h++);
        b->setPosition(p);
        if (par) par->addChild(b);
        return b;
    };
    auto* hips = bone("Hips", {0, 1.0f, 0}, nullptr);
    auto* spine = bone("Spine", {0, 0.3f, 0}, hips);
    bone("Head", {0, 0.4f, 0}, spine);
    bone("LeftUpLeg", {0.15f, -0.1f, 0}, hips);
    bone("RightUpLeg", {-0.15f, -0.1f, 0}, hips);
    auto* lcol = bone("LeftShoulder", {0.05f, 0.25f, 0}, spine);
    auto* larm = bone("LeftArm", {0.15f, 0, 0}, lcol);
    bone("LeftForeArm", {0.3f, 0, 0}, larm);
    auto* rcol = bone("RightShoulder", {-0.05f, 0.25f, 0}, spine);
    auto* rarm = bone("RightArm", {-0.15f, 0, 0}, rcol);
    bone("RightForeArm", {-0.3f, 0, 0}, rarm);
    skelRes->setBindingPose();
    auto mesh = createInMemoryMesh("twist_collar_mesh", skelRes);
    auto* sm = Manager::getSingleton()->getSceneMgr();
    Ogre::Entity* ent = sm->createEntity("twist_collar_ent", mesh);
    ASSERT_NE(ent, nullptr);
    Ogre::SkeletonInstance* skel = ent->getSkeleton();
    ASSERT_NE(skel, nullptr);

    // Source: left collar (role 10, +X) rolls 0 → 240° — past the ±180° wrap.
    // This is where a missing unwrap explodes: the wrapped angle would flip
    // sign mid-clip and snap. With unwrap, the collar's model twist cap
    // (30° = 0.52 rad) clamps first, THEN the 0.5× collar gain applies →
    // final steady roll ≈ 30° × 0.5 = 15° about +X.
    const int frames = 61;
    auto quats = identityClip(frames);
    for (int f = 0; f < frames; ++f) {
        const float a = 240.0f * static_cast<float>(f) / (frames - 1);
        const Ogre::Quaternion w =
            Ogre::Quaternion(Ogre::Degree(a), Ogre::Vector3::UNIT_X)
            * kSrcRest;
        quats[f][10] = {w.x, w.y, w.z, w.w};
    }
    // modelClip=true: the per-role twist caps only apply on the model path
    // (authored/self clips run uncapped since #837 review). This test asserts
    // the capped collar behavior, so it exercises the model path explicitly.
    const auto res = AnimationMerger::applyMotionClip(
        skel, "collarclip", quats, 30, true, srcRestWorld(),
        false, 8, false, canonRestDirs(), /*modelClip=*/true);
    ASSERT_TRUE(res.ok) << res.error.toStdString();

    // Sample densely: the collar's world orientation must move CONTINUOUSLY
    // (no wrap snap) and end near 240° × 0.5 = 120° about +X.
    Ogre::Quaternion prev = Ogre::Quaternion::IDENTITY;
    float maxStepDeg = 0.0f;
    Ogre::Quaternion last;
    auto* anim = skel->getAnimation("collarclip");
    const float len = anim->getLength();
    for (int s = 0; s <= 60; ++s) {
        skel->reset(true);
        anim->apply(skel, len * static_cast<float>(s) / 60.0f);
        skel->_updateTransforms();
        const Ogre::Quaternion w =
            skel->getBone("LeftShoulder")->_getDerivedOrientation();
        if (s > 0) {
            const Ogre::Quaternion d = w * prev.Inverse();
            const float step = 2.0f * std::acos(std::min(
                1.0f, std::abs(d.w))) * 180.0f / static_cast<float>(M_PI);
            maxStepDeg = std::max(maxStepDeg, step);
        }
        prev = w;
        last = w;
    }
    EXPECT_LT(maxStepDeg, 15.0f) << "collar roll snapped mid-clip (unwrap)";
    // 240° source twist is clamped to the collar model cap (30°) FIRST, then
    // scaled by the 0.5× collar gain: 30 × 0.5 = 15° steady roll about +X.
    EXPECT_NEAR(twistDegAbout(last, Ogre::Vector3::UNIT_X), 15.0f, 8.0f);

    sm->destroyEntity(ent);
}

TEST_F(AnimationMergerTest, VerticalDescentLowersRootDescentOnly)
{
    // #838: a non-locomotion clip carrying a per-frame rootY (hip drop in
    // leg-lengths) lowers the ROOT bone's keyframe Y by rootY × target-leg-len,
    // but ONLY the negative (descent) component — positive rootY never lifts.
    auto skelRes = Ogre::SkeletonManager::getSingleton().create(
        "descent_skel",
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    unsigned short h = 0;
    auto bone = [&](const char* n, const Ogre::Vector3& p, Ogre::Bone* par) {
        auto* b = skelRes->createBone(n, h++);
        b->setPosition(p);
        if (par) par->addChild(b);
        return b;
    };
    // A FULL humanoid bone set — applyMotionClip rejects a rig that resolves
    // fewer than ~half of the 22 canonical roles ("not a humanoid rig"), so the
    // minimal 9-bone skeleton isn't enough. Mixamo-style names map onto the
    // canonical roles (hip/spine/chest/neck/head, collar/shoulder/elbow/hand,
    // upleg/leg/foot per side).
    // Hips at world Y=1.0; each leg chain reaches down to Y=0 → leg length 1.0.
    // Legs are PURELY VERTICAL (no lateral X) so the bind-pose hip→foot
    // Euclidean distance the retarget scales by is exactly 1.0 — then rootY=-0.5
    // maps to an exact -0.5 hip delta (a lateral offset would make the 3D leg
    // length sqrt(0.15²+1²)≈1.011 and skew the expectation).
    auto* hips  = bone("Hips",  {0, 1.0f, 0}, nullptr);        // role 0
    auto* spine = bone("Spine", {0, 0.2f, 0}, hips);           // role 1 (abdomen)
    auto* chest = bone("Spine1", {0, 0.2f, 0}, spine);         // role 2 (chest)
    auto* neck  = bone("Neck",  {0, 0.2f, 0}, chest);          // role 3
    bone("Head", {0, 0.15f, 0}, neck);                         // role 5
    // Arms: Shoulder(collar) → Arm(shoulder) → ForeArm(elbow) → Hand.
    auto* rsh = bone("RightShoulder", {-0.05f, 0.1f, 0}, chest);
    auto* rarm = bone("RightArm", {-0.15f, 0, 0}, rsh);
    auto* rfa = bone("RightForeArm", {-0.25f, 0, 0}, rarm);
    bone("RightHand", {-0.2f, 0, 0}, rfa);
    auto* lsh = bone("LeftShoulder", {0.05f, 0.1f, 0}, chest);
    auto* larm = bone("LeftArm", {0.15f, 0, 0}, lsh);
    auto* lfa = bone("LeftForeArm", {0.25f, 0, 0}, larm);
    bone("LeftHand", {0.2f, 0, 0}, lfa);
    // Legs: UpLeg(hip) → Leg(knee) → Foot.
    auto* rleg = bone("RightUpLeg", {0, -0.25f, 0}, hips);
    auto* rknee = bone("RightLeg", {0, -0.25f, 0}, rleg);
    bone("RightFoot", {0, -0.5f, 0}, rknee);                   // role 17 → Y=0
    auto* lleg = bone("LeftUpLeg", {0, -0.25f, 0}, hips);
    auto* lknee = bone("LeftLeg", {0, -0.25f, 0}, lleg);
    bone("LeftFoot", {0, -0.5f, 0}, lknee);                    // role 21 → Y=0
    skelRes->setBindingPose();
    auto mesh = createInMemoryMesh("descent_mesh", skelRes);
    auto* sm = Manager::getSingleton()->getSceneMgr();
    Ogre::Entity* ent = sm->createEntity("descent_ent", mesh);
    ASSERT_NE(ent, nullptr);
    Ogre::SkeletonInstance* skel = ent->getSkeleton();

    const int frames = 3;
    auto quats = identityClip(frames);   // no rotation — isolate translation
    // rootY: flat, then −0.5 leg (descend), then +0.4 leg (would rise, clamped)
    const std::vector<float> rootY = {0.0f, -0.5f, +0.4f};

    const auto res = AnimationMerger::applyMotionClip(
        skel, "descentclip", quats, 30, /*worldFrame=*/true, srcRestWorld(),
        false, 8, false, canonRestDirs(), /*modelClip=*/false,
        rootY, /*verticalDescent=*/true);
    ASSERT_TRUE(res.ok) << res.error.toStdString();

    // Target leg length here is hip(Y=1) → foot(Y=0) = 1.0, so the descent
    // frame drops the hip by 0.5 world units; the "rising" frame clamps to 0.
    auto* anim = skel->getAnimation("descentclip");
    auto* track = anim->getNodeTrack(hips->getHandle());
    ASSERT_NE(track, nullptr);
    const float y0 = track->getNodeKeyFrame(0)->getTranslate().y;
    const float y1 = track->getNodeKeyFrame(1)->getTranslate().y;
    const float y2 = track->getNodeKeyFrame(2)->getTranslate().y;
    EXPECT_NEAR(y1, y0 - 0.5f, 1e-3f) << "descent frame did not lower the hip";
    EXPECT_NEAR(y2, y0, 1e-3f) << "positive rootY must not lift the hip";

    // Control: a LOCOMOTION clip (verticalDescent=false) keeps the root flat
    // even when a rootY is present.
    const auto res2 = AnimationMerger::applyMotionClip(
        skel, "flatclip", quats, 30, /*worldFrame=*/true, srcRestWorld(),
        false, 8, false, canonRestDirs(), /*modelClip=*/false,
        rootY, /*verticalDescent=*/false);
    ASSERT_TRUE(res2.ok) << res2.error.toStdString();
    auto* track2 = skel->getAnimation("flatclip")->getNodeTrack(hips->getHandle());
    ASSERT_NE(track2, nullptr);
    EXPECT_NEAR(track2->getNodeKeyFrame(1)->getTranslate().y,
                track2->getNodeKeyFrame(0)->getTranslate().y, 1e-3f)
        << "locomotion clip must keep a flat root";

    sm->destroyEntity(ent);
}
