#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <cmath>
#include "Assimp/AnimationProcessor.h"

// These tests exercise the per-channel keyframe math, the ticks-per-second
// default-to-24 branch, and the same-time key-merge logic in AnimationProcessor.
//
// AnimationProcessor only needs an in-memory Ogre::Skeleton (no GL/display), so a
// bare Ogre::Root is sufficient. The SkeletonManager singleton is owned by Root,
// so each test constructs its own Root + skeleton to stay isolated (mirrors the
// existing AnimationProcessor_test / BoneProcessor_test fixture style).

namespace {

// Helper: build an aiNodeAnim with heap-allocated key arrays. Caller owns the
// returned object; the aiScene/aiAnimation that holds it owns the arrays once
// assigned. For test simplicity we let these in-memory allocations leak (the
// existing suite does the same) — the process exits at test-binary teardown.
aiNodeAnim* makeNodeAnim(const std::string& boneName,
                         const std::vector<aiVectorKey>& posKeys,
                         const std::vector<aiQuatKey>& rotKeys,
                         const std::vector<aiVectorKey>& scaleKeys) {
    auto* nodeAnim = new aiNodeAnim();
    nodeAnim->mNodeName = aiString(boneName);

    nodeAnim->mNumPositionKeys = static_cast<unsigned int>(posKeys.size());
    if (!posKeys.empty()) {
        nodeAnim->mPositionKeys = new aiVectorKey[posKeys.size()];
        for (size_t i = 0; i < posKeys.size(); ++i) nodeAnim->mPositionKeys[i] = posKeys[i];
    } else {
        nodeAnim->mPositionKeys = nullptr;
    }

    nodeAnim->mNumRotationKeys = static_cast<unsigned int>(rotKeys.size());
    if (!rotKeys.empty()) {
        nodeAnim->mRotationKeys = new aiQuatKey[rotKeys.size()];
        for (size_t i = 0; i < rotKeys.size(); ++i) nodeAnim->mRotationKeys[i] = rotKeys[i];
    } else {
        nodeAnim->mRotationKeys = nullptr;
    }

    nodeAnim->mNumScalingKeys = static_cast<unsigned int>(scaleKeys.size());
    if (!scaleKeys.empty()) {
        nodeAnim->mScalingKeys = new aiVectorKey[scaleKeys.size()];
        for (size_t i = 0; i < scaleKeys.size(); ++i) nodeAnim->mScalingKeys[i] = scaleKeys[i];
    } else {
        nodeAnim->mScalingKeys = nullptr;
    }
    return nodeAnim;
}

// Helper: wrap a single channel into an aiScene with one animation.
aiScene* makeSceneWithChannel(const std::string& animName,
                             double duration,
                             double ticksPerSecond,
                             aiNodeAnim* channel /* may be nullptr */) {
    auto* scene = new aiScene();
    scene->mNumAnimations = 1;
    scene->mAnimations = new aiAnimation*[1];
    auto* anim = new aiAnimation();
    anim->mName = aiString(animName);
    anim->mDuration = duration;
    anim->mTicksPerSecond = ticksPerSecond;
    if (channel) {
        anim->mNumChannels = 1;
        anim->mChannels = new aiNodeAnim*[1];
        anim->mChannels[0] = channel;
    } else {
        anim->mNumChannels = 0;
        anim->mChannels = nullptr;
    }
    scene->mAnimations[0] = anim;
    return scene;
}

} // namespace

// ---------------------------------------------------------------------------
// Ticks-per-second default-to-24 branch
// ---------------------------------------------------------------------------

// mTicksPerSecond == 0 → length should be mDuration / 24.
// NB the channel must target a REAL bone: AnimationProcessor now drops a clip
// that resolves to zero node tracks (a node-transform / non-bone channel would
// otherwise leak a phantom skeletal clip — issue #517). So we add a bone named
// "B0" and key it, keeping the clip alive for the length assertion.
TEST(AnimationProcessorChannelTest, TicksPerSecondDefaultsTo24WhenZero) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    auto skeleton = Ogre::SkeletonManager::getSingleton().create(
        "TicksZeroSkel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);
    skeleton->createBone("B0");
    AnimationProcessor processor(skeleton);

    std::vector<aiVectorKey> posKeys = { aiVectorKey(0.0, aiVector3D(0, 0, 0)) };
    aiNodeAnim* channel = makeNodeAnim("B0", posKeys, {}, {});
    aiScene* scene = makeSceneWithChannel("ZeroTicks", /*duration*/48.0, /*ticks*/0.0, channel);
    processor.processAnimations(scene);

    ASSERT_EQ(skeleton->getNumAnimations(), 1u);
    Ogre::Animation* anim = skeleton->getAnimation("ZeroTicks");
    EXPECT_NEAR(anim->getLength(), 48.0 / 24.0, 1e-4);  // == 2.0
}

// mTicksPerSecond != 0 → length should be mDuration / ticks.
TEST(AnimationProcessorChannelTest, TicksPerSecondUsedWhenNonZero) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    auto skeleton = Ogre::SkeletonManager::getSingleton().create(
        "TicksNonZeroSkel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);
    skeleton->createBone("B0");
    AnimationProcessor processor(skeleton);

    std::vector<aiVectorKey> posKeys = { aiVectorKey(0.0, aiVector3D(0, 0, 0)) };
    aiNodeAnim* channel = makeNodeAnim("B0", posKeys, {}, {});
    aiScene* scene = makeSceneWithChannel("NonZeroTicks", /*duration*/60.0, /*ticks*/30.0, channel);
    processor.processAnimations(scene);

    ASSERT_EQ(skeleton->getNumAnimations(), 1u);
    Ogre::Animation* anim = skeleton->getAnimation("NonZeroTicks");
    EXPECT_NEAR(anim->getLength(), 60.0 / 30.0, 1e-4);  // == 2.0
}

// ---------------------------------------------------------------------------
// Zero-animation scene boundary
// ---------------------------------------------------------------------------

// mNumAnimations == 0 → skeleton left with 0 animations.
TEST(AnimationProcessorChannelTest, ZeroAnimationsLeavesSkeletonEmpty) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    auto skeleton = Ogre::SkeletonManager::getSingleton().create(
        "ZeroAnimSkel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);
    AnimationProcessor processor(skeleton);

    aiScene scene;
    scene.mNumAnimations = 0;
    scene.mAnimations = nullptr;

    processor.processAnimations(&scene);

    EXPECT_EQ(skeleton->getNumAnimations(), 0u);
}

// ---------------------------------------------------------------------------
// Bone-not-in-skeleton early return
// ---------------------------------------------------------------------------

// A channel referencing an unknown bone yields no node track, so the whole clip
// resolves to zero tracks. AnimationProcessor now DROPS such a clip (previously
// it was kept as an empty animation): an all-non-bone aiAnimation is a node-
// transform clip whose empty skeletal twin would otherwise leak into the
// Inspector list / dope sheet and get auto-selected (issue #517). Node clips are
// rebuilt separately by reconstructNodeClipsFrom*.
TEST(AnimationProcessorChannelTest, UnknownBoneAddsNoTrack) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    auto skeleton = Ogre::SkeletonManager::getSingleton().create(
        "UnknownBoneSkel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);
    // Note: no bone named "Ghost" is created.
    AnimationProcessor processor(skeleton);

    std::vector<aiVectorKey> posKeys = { aiVectorKey(0.0, aiVector3D(1, 2, 3)) };
    aiNodeAnim* channel = makeNodeAnim("Ghost", posKeys, {}, {});
    aiScene* scene = makeSceneWithChannel("GhostAnim", 24.0, 24.0, channel);

    processor.processAnimations(scene);

    // The zero-track clip is dropped entirely.
    EXPECT_EQ(skeleton->getNumAnimations(), 0u);
    EXPECT_FALSE(skeleton->hasAnimation("GhostAnim"));
}

// ---------------------------------------------------------------------------
// Position-key local-space delta math
// ---------------------------------------------------------------------------

// Identity-orientation bone at origin: a key (1,2,3) yields keyframe translate
// (1,2,3) at time mTime/ticks.
TEST(AnimationProcessorChannelTest, PositionKeyIdentityBoneAtOrigin) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    auto skeleton = Ogre::SkeletonManager::getSingleton().create(
        "PosIdentitySkel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);

    Ogre::Bone* bone = skeleton->createBone("PosBone");
    bone->setPosition(Ogre::Vector3::ZERO);
    bone->setOrientation(Ogre::Quaternion::IDENTITY);

    AnimationProcessor processor(skeleton);

    const double ticks = 24.0;
    const double keyTime = 12.0;
    std::vector<aiVectorKey> posKeys = { aiVectorKey(keyTime, aiVector3D(1, 2, 3)) };
    aiNodeAnim* channel = makeNodeAnim("PosBone", posKeys, {}, {});
    aiScene* scene = makeSceneWithChannel("PosAnim", 24.0, ticks, channel);

    processor.processAnimations(scene);

    Ogre::Animation* anim = skeleton->getAnimation("PosAnim");
    ASSERT_EQ(anim->getNumNodeTracks(), 1u);
    Ogre::NodeAnimationTrack* track = anim->getNodeTrack(bone->getHandle());
    ASSERT_EQ(track->getNumKeyFrames(), 1u);

    Ogre::TransformKeyFrame* kf = track->getNodeKeyFrame(0);
    EXPECT_NEAR(kf->getTime(), keyTime / ticks, 1e-4);
    Ogre::Vector3 t = kf->getTranslate();
    EXPECT_NEAR(t.x, 1.0, 1e-4);
    EXPECT_NEAR(t.y, 2.0, 1e-4);
    EXPECT_NEAR(t.z, 3.0, 1e-4);
}

// Non-trivial T-pose: translate == boneTPoseInverseRotation * (key - boneTPosePosition).
// Bone at position (10,0,0), identity orientation, key (11,2,3) → delta (1,2,3).
TEST(AnimationProcessorChannelTest, PositionKeyOffsetBoneDeltaMath) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    auto skeleton = Ogre::SkeletonManager::getSingleton().create(
        "PosOffsetSkel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);

    Ogre::Bone* bone = skeleton->createBone("OffBone");
    bone->setPosition(Ogre::Vector3(10, 0, 0));
    bone->setOrientation(Ogre::Quaternion::IDENTITY);

    AnimationProcessor processor(skeleton);

    std::vector<aiVectorKey> posKeys = { aiVectorKey(0.0, aiVector3D(11, 2, 3)) };
    aiNodeAnim* channel = makeNodeAnim("OffBone", posKeys, {}, {});
    aiScene* scene = makeSceneWithChannel("PosOffAnim", 24.0, 24.0, channel);

    processor.processAnimations(scene);

    Ogre::Animation* anim = skeleton->getAnimation("PosOffAnim");
    Ogre::NodeAnimationTrack* track = anim->getNodeTrack(bone->getHandle());
    ASSERT_EQ(track->getNumKeyFrames(), 1u);

    Ogre::Vector3 t = track->getNodeKeyFrame(0)->getTranslate();
    EXPECT_NEAR(t.x, 1.0, 1e-4);
    EXPECT_NEAR(t.y, 2.0, 1e-4);
    EXPECT_NEAR(t.z, 3.0, 1e-4);
}

// Rotated T-pose bone: a 90-deg-about-Z bone at origin, key (1,0,0) → the delta
// is rotated by the inverse T-pose rotation (Z by -90 deg) → (0,-1,0).
TEST(AnimationProcessorChannelTest, PositionKeyRotatedBoneAppliesInverseRotation) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    auto skeleton = Ogre::SkeletonManager::getSingleton().create(
        "PosRotSkel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);

    Ogre::Bone* bone = skeleton->createBone("RotBone");
    bone->setPosition(Ogre::Vector3::ZERO);
    Ogre::Quaternion zRot(Ogre::Degree(90), Ogre::Vector3::UNIT_Z);
    bone->setOrientation(zRot);

    AnimationProcessor processor(skeleton);

    std::vector<aiVectorKey> posKeys = { aiVectorKey(0.0, aiVector3D(1, 0, 0)) };
    aiNodeAnim* channel = makeNodeAnim("RotBone", posKeys, {}, {});
    aiScene* scene = makeSceneWithChannel("PosRotAnim", 24.0, 24.0, channel);

    processor.processAnimations(scene);

    Ogre::Animation* anim = skeleton->getAnimation("PosRotAnim");
    Ogre::NodeAnimationTrack* track = anim->getNodeTrack(bone->getHandle());
    ASSERT_EQ(track->getNumKeyFrames(), 1u);

    // expected = zRot.Inverse() * (1,0,0)
    Ogre::Vector3 expected = zRot.Inverse() * Ogre::Vector3(1, 0, 0);
    Ogre::Vector3 t = track->getNodeKeyFrame(0)->getTranslate();
    EXPECT_NEAR(t.x, expected.x, 1e-4);
    EXPECT_NEAR(t.y, expected.y, 1e-4);
    EXPECT_NEAR(t.z, expected.z, 1e-4);
}

// ---------------------------------------------------------------------------
// Rotation-key path
// ---------------------------------------------------------------------------

// Identity T-pose: keyframe rotation equals the input quat (w,x,y,z),
// normalised. Use a 90-deg-about-Y rotation.
TEST(AnimationProcessorChannelTest, RotationKeyIdentityTPose) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    auto skeleton = Ogre::SkeletonManager::getSingleton().create(
        "RotKeySkel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);

    Ogre::Bone* bone = skeleton->createBone("RBone");
    bone->setPosition(Ogre::Vector3::ZERO);
    bone->setOrientation(Ogre::Quaternion::IDENTITY);

    AnimationProcessor processor(skeleton);

    Ogre::Quaternion expected(Ogre::Degree(90), Ogre::Vector3::UNIT_Y);
    // aiQuaternion is (w, x, y, z)
    aiQuaternion aq;
    aq.w = expected.w; aq.x = expected.x; aq.y = expected.y; aq.z = expected.z;
    std::vector<aiQuatKey> rotKeys = { aiQuatKey(0.0, aq) };
    aiNodeAnim* channel = makeNodeAnim("RBone", {}, rotKeys, {});
    aiScene* scene = makeSceneWithChannel("RotAnim", 24.0, 24.0, channel);

    processor.processAnimations(scene);

    Ogre::Animation* anim = skeleton->getAnimation("RotAnim");
    Ogre::NodeAnimationTrack* track = anim->getNodeTrack(bone->getHandle());
    ASSERT_EQ(track->getNumKeyFrames(), 1u);

    Ogre::Quaternion r = track->getNodeKeyFrame(0)->getRotation();
    // Quaternion may come back with overall sign flipped (q and -q are equal
    // rotations); compare via dot magnitude.
    Ogre::Real dot = std::abs(r.Dot(expected));
    EXPECT_NEAR(dot, 1.0, 1e-3);
}

// Rotated T-pose: result == boneTPoseInverseRotation * inputQuat (normalised).
TEST(AnimationProcessorChannelTest, RotationKeyRotatedTPoseAppliesInverse) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    auto skeleton = Ogre::SkeletonManager::getSingleton().create(
        "RotKeyRotSkel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);

    Ogre::Bone* bone = skeleton->createBone("RRBone");
    bone->setPosition(Ogre::Vector3::ZERO);
    Ogre::Quaternion tpose(Ogre::Degree(45), Ogre::Vector3::UNIT_X);
    bone->setOrientation(tpose);

    AnimationProcessor processor(skeleton);

    Ogre::Quaternion input(Ogre::Degree(90), Ogre::Vector3::UNIT_Y);
    aiQuaternion aq;
    aq.w = input.w; aq.x = input.x; aq.y = input.y; aq.z = input.z;
    std::vector<aiQuatKey> rotKeys = { aiQuatKey(0.0, aq) };
    aiNodeAnim* channel = makeNodeAnim("RRBone", {}, rotKeys, {});
    aiScene* scene = makeSceneWithChannel("RotRotAnim", 24.0, 24.0, channel);

    processor.processAnimations(scene);

    Ogre::Animation* anim = skeleton->getAnimation("RotRotAnim");
    Ogre::NodeAnimationTrack* track = anim->getNodeTrack(bone->getHandle());
    ASSERT_EQ(track->getNumKeyFrames(), 1u);

    Ogre::Quaternion expected = tpose.Inverse() * input;
    expected.normalise();
    Ogre::Quaternion r = track->getNodeKeyFrame(0)->getRotation();
    Ogre::Real dot = std::abs(r.Dot(expected));
    EXPECT_NEAR(dot, 1.0, 1e-3);
}

// ---------------------------------------------------------------------------
// Keyframe-merge branch: rotation key at SAME mTime as a position key
// ---------------------------------------------------------------------------

// A rotation key sharing a position key's mTime updates get<1> of the existing
// tuple — one keyframe, both translate and rotation set.
TEST(AnimationProcessorChannelTest, RotationKeyMergesWithSameTimePositionKey) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    auto skeleton = Ogre::SkeletonManager::getSingleton().create(
        "MergeRotSkel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);

    Ogre::Bone* bone = skeleton->createBone("MBone");
    bone->setPosition(Ogre::Vector3::ZERO);
    bone->setOrientation(Ogre::Quaternion::IDENTITY);

    AnimationProcessor processor(skeleton);

    const double sharedTime = 0.0;
    std::vector<aiVectorKey> posKeys = { aiVectorKey(sharedTime, aiVector3D(1, 2, 3)) };

    Ogre::Quaternion expectedRot(Ogre::Degree(90), Ogre::Vector3::UNIT_Y);
    aiQuaternion aq;
    aq.w = expectedRot.w; aq.x = expectedRot.x; aq.y = expectedRot.y; aq.z = expectedRot.z;
    std::vector<aiQuatKey> rotKeys = { aiQuatKey(sharedTime, aq) };

    aiNodeAnim* channel = makeNodeAnim("MBone", posKeys, rotKeys, {});
    aiScene* scene = makeSceneWithChannel("MergeRotAnim", 24.0, 24.0, channel);

    processor.processAnimations(scene);

    Ogre::Animation* anim = skeleton->getAnimation("MergeRotAnim");
    Ogre::NodeAnimationTrack* track = anim->getNodeTrack(bone->getHandle());
    // Single merged keyframe, not two.
    ASSERT_EQ(track->getNumKeyFrames(), 1u);

    Ogre::TransformKeyFrame* kf = track->getNodeKeyFrame(0);
    Ogre::Vector3 t = kf->getTranslate();
    EXPECT_NEAR(t.x, 1.0, 1e-4);
    EXPECT_NEAR(t.y, 2.0, 1e-4);
    EXPECT_NEAR(t.z, 3.0, 1e-4);

    Ogre::Quaternion r = kf->getRotation();
    Ogre::Real dot = std::abs(r.Dot(expectedRot));
    EXPECT_NEAR(dot, 1.0, 1e-3);
}

// Rotation key at a distinct time → separate keyframe (new-time insert branch).
TEST(AnimationProcessorChannelTest, RotationKeyDistinctTimeInsertsNewKeyframe) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    auto skeleton = Ogre::SkeletonManager::getSingleton().create(
        "DistinctRotSkel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);

    Ogre::Bone* bone = skeleton->createBone("DBone");
    bone->setPosition(Ogre::Vector3::ZERO);
    bone->setOrientation(Ogre::Quaternion::IDENTITY);

    AnimationProcessor processor(skeleton);

    std::vector<aiVectorKey> posKeys = { aiVectorKey(0.0, aiVector3D(1, 0, 0)) };
    Ogre::Quaternion q(Ogre::Degree(90), Ogre::Vector3::UNIT_Y);
    aiQuaternion aq; aq.w = q.w; aq.x = q.x; aq.y = q.y; aq.z = q.z;
    std::vector<aiQuatKey> rotKeys = { aiQuatKey(12.0, aq) };

    aiNodeAnim* channel = makeNodeAnim("DBone", posKeys, rotKeys, {});
    aiScene* scene = makeSceneWithChannel("DistinctRotAnim", 24.0, 24.0, channel);

    processor.processAnimations(scene);

    Ogre::Animation* anim = skeleton->getAnimation("DistinctRotAnim");
    Ogre::NodeAnimationTrack* track = anim->getNodeTrack(bone->getHandle());
    EXPECT_EQ(track->getNumKeyFrames(), 2u);
}

// ---------------------------------------------------------------------------
// Scaling-key branches: new-time insert vs same-time merge into get<2>
// ---------------------------------------------------------------------------

// Scaling key at a fresh time → new keyframe; scale equals input.
TEST(AnimationProcessorChannelTest, ScalingKeyNewTimeInsert) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    auto skeleton = Ogre::SkeletonManager::getSingleton().create(
        "ScaleNewSkel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);

    Ogre::Bone* bone = skeleton->createBone("SBone");
    bone->setPosition(Ogre::Vector3::ZERO);
    bone->setOrientation(Ogre::Quaternion::IDENTITY);

    AnimationProcessor processor(skeleton);

    std::vector<aiVectorKey> scaleKeys = { aiVectorKey(0.0, aiVector3D(2, 3, 4)) };
    aiNodeAnim* channel = makeNodeAnim("SBone", {}, {}, scaleKeys);
    aiScene* scene = makeSceneWithChannel("ScaleNewAnim", 24.0, 24.0, channel);

    processor.processAnimations(scene);

    Ogre::Animation* anim = skeleton->getAnimation("ScaleNewAnim");
    Ogre::NodeAnimationTrack* track = anim->getNodeTrack(bone->getHandle());
    ASSERT_EQ(track->getNumKeyFrames(), 1u);

    Ogre::Vector3 s = track->getNodeKeyFrame(0)->getScale();
    EXPECT_NEAR(s.x, 2.0, 1e-4);
    EXPECT_NEAR(s.y, 3.0, 1e-4);
    EXPECT_NEAR(s.z, 4.0, 1e-4);
}

// Scaling key sharing a position key's time → merges into get<2>; single keyframe
// carrying both translate and scale.
TEST(AnimationProcessorChannelTest, ScalingKeyMergesWithSameTimePositionKey) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    auto skeleton = Ogre::SkeletonManager::getSingleton().create(
        "ScaleMergeSkel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);

    Ogre::Bone* bone = skeleton->createBone("SMBone");
    bone->setPosition(Ogre::Vector3::ZERO);
    bone->setOrientation(Ogre::Quaternion::IDENTITY);

    AnimationProcessor processor(skeleton);

    const double sharedTime = 0.0;
    std::vector<aiVectorKey> posKeys = { aiVectorKey(sharedTime, aiVector3D(1, 2, 3)) };
    std::vector<aiVectorKey> scaleKeys = { aiVectorKey(sharedTime, aiVector3D(2, 2, 2)) };

    aiNodeAnim* channel = makeNodeAnim("SMBone", posKeys, {}, scaleKeys);
    aiScene* scene = makeSceneWithChannel("ScaleMergeAnim", 24.0, 24.0, channel);

    processor.processAnimations(scene);

    Ogre::Animation* anim = skeleton->getAnimation("ScaleMergeAnim");
    Ogre::NodeAnimationTrack* track = anim->getNodeTrack(bone->getHandle());
    ASSERT_EQ(track->getNumKeyFrames(), 1u);

    Ogre::TransformKeyFrame* kf = track->getNodeKeyFrame(0);
    Ogre::Vector3 t = kf->getTranslate();
    EXPECT_NEAR(t.x, 1.0, 1e-4);
    EXPECT_NEAR(t.y, 2.0, 1e-4);
    EXPECT_NEAR(t.z, 3.0, 1e-4);

    Ogre::Vector3 s = kf->getScale();
    EXPECT_NEAR(s.x, 2.0, 1e-4);
    EXPECT_NEAR(s.y, 2.0, 1e-4);
    EXPECT_NEAR(s.z, 2.0, 1e-4);
}

// ---------------------------------------------------------------------------
// All three key types sharing one time → a single fully-populated keyframe
// ---------------------------------------------------------------------------

TEST(AnimationProcessorChannelTest, PositionRotationScaleAllMergeAtSameTime) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    auto skeleton = Ogre::SkeletonManager::getSingleton().create(
        "AllMergeSkel", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);

    Ogre::Bone* bone = skeleton->createBone("ABone");
    bone->setPosition(Ogre::Vector3::ZERO);
    bone->setOrientation(Ogre::Quaternion::IDENTITY);

    AnimationProcessor processor(skeleton);

    const double sharedTime = 0.0;
    std::vector<aiVectorKey> posKeys = { aiVectorKey(sharedTime, aiVector3D(5, 6, 7)) };
    Ogre::Quaternion q(Ogre::Degree(90), Ogre::Vector3::UNIT_Z);
    aiQuaternion aq; aq.w = q.w; aq.x = q.x; aq.y = q.y; aq.z = q.z;
    std::vector<aiQuatKey> rotKeys = { aiQuatKey(sharedTime, aq) };
    std::vector<aiVectorKey> scaleKeys = { aiVectorKey(sharedTime, aiVector3D(3, 3, 3)) };

    aiNodeAnim* channel = makeNodeAnim("ABone", posKeys, rotKeys, scaleKeys);
    aiScene* scene = makeSceneWithChannel("AllMergeAnim", 24.0, 24.0, channel);

    processor.processAnimations(scene);

    Ogre::Animation* anim = skeleton->getAnimation("AllMergeAnim");
    Ogre::NodeAnimationTrack* track = anim->getNodeTrack(bone->getHandle());
    ASSERT_EQ(track->getNumKeyFrames(), 1u);

    Ogre::TransformKeyFrame* kf = track->getNodeKeyFrame(0);
    EXPECT_NEAR(kf->getTranslate().x, 5.0, 1e-4);
    EXPECT_NEAR(kf->getScale().x, 3.0, 1e-4);
    Ogre::Real dot = std::abs(kf->getRotation().Dot(q));
    EXPECT_NEAR(dot, 1.0, 1e-3);
}
