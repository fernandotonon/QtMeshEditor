#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "AnimationProcessor.h"

// Test if processAnimations processes all animations. Each animation must carry
// a channel targeting a REAL bone: processAnimation skips channel-less clips
// (glTF morph-weight anims — issue #517 skeletal+morph) and drops clips that
// resolve to zero node tracks (non-bone / node-transform channels — issue #517
// reimport hygiene). So a bone "Root" is created and each clip keys it, keeping
// both alive to prove the loop visits every aiAnimation.
TEST(AnimationProcessorTest, ProcessAllAnimations) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    auto mockSkeleton= Ogre::SkeletonManager::getSingleton().create("MockSkeleton",Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);
    mockSkeleton->createBone("Root");
    AnimationProcessor processor(mockSkeleton);

    auto makeBoneChannel = []() {
        auto* ch = new aiNodeAnim;
        ch->mNodeName = aiString(std::string("Root"));
        ch->mNumPositionKeys = 1;
        ch->mPositionKeys = new aiVectorKey[1]{ aiVectorKey(0.0, aiVector3D(0, 0, 0)) };
        return ch;
    };

    aiScene scene;
    scene.mNumAnimations = 2;
    scene.mAnimations = new aiAnimation*[2];
    scene.mAnimations[0] = new aiAnimation;
    scene.mAnimations[1] = new aiAnimation;
    scene.mAnimations[0]->mName = aiString( std::string( "Animation1"));
    scene.mAnimations[1]->mName = aiString( std::string( "Animation2"));
    scene.mAnimations[0]->mNumChannels=1;
    scene.mAnimations[0]->mChannels = new aiNodeAnim*[1]{ makeBoneChannel() };
    scene.mAnimations[1]->mNumChannels=1;
    scene.mAnimations[1]->mChannels = new aiNodeAnim*[1]{ makeBoneChannel() };

    processor.processAnimations(&scene);

    EXPECT_EQ(mockSkeleton->getNumAnimations(), 2);
    EXPECT_TRUE(mockSkeleton->hasAnimation("Animation1"));
    EXPECT_TRUE(mockSkeleton->hasAnimation("Animation2"));
}

// #936: scale keys must be stored RELATIVE to the channel node's bind scale.
// Blender-style FBX rigs re-express the armature's static scale (e.g. ×100)
// in every animation curve; Ogre applies keyframe scale multiplicatively on
// the bind pose, so a raw key double-applies the scale and blows the skinned
// mesh out of frame. A key equal to the node's bind scale must become 1.
TEST(AnimationProcessorTest, ScaleKeysAreRelativeToNodeBindScale) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    auto skel = Ogre::SkeletonManager::getSingleton().create(
        "ScaleRelSkeleton", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);
    Ogre::Bone* bone = skel->createBone("Armature");
    bone->setScale(Ogre::Vector3::UNIT_SCALE);   // re-rooted Ogre bind: scale 1

    aiScene scene;
    scene.mRootNode = new aiNode("Scene");
    aiNode* arm = new aiNode("Armature");
    aiMatrix4x4::Scaling(aiVector3D(100.f, 100.f, 100.f), arm->mTransformation);
    arm->mParent = scene.mRootNode;
    scene.mRootNode->mNumChildren = 1;
    scene.mRootNode->mChildren = new aiNode*[1]{arm};

    auto* channel = new aiNodeAnim;
    channel->mNodeName = aiString(std::string("Armature"));
    channel->mNumScalingKeys = 1;
    channel->mScalingKeys = new aiVectorKey[1];
    channel->mScalingKeys[0] = aiVectorKey(0.0, aiVector3D(100.f, 100.f, 100.f));

    auto* anim = new aiAnimation;
    anim->mName = aiString(std::string("Clip"));
    anim->mDuration = 1.0;
    anim->mTicksPerSecond = 1.0;
    anim->mNumChannels = 1;
    anim->mChannels = new aiNodeAnim*[1]{channel};
    scene.mNumAnimations = 1;
    scene.mAnimations = new aiAnimation*[1]{anim};

    AnimationProcessor processor(skel);
    processor.processAnimations(&scene);

    ASSERT_EQ(skel->getNumAnimations(), 1);
    Ogre::NodeAnimationTrack* track =
        skel->getAnimation(0)->getNodeTrack(bone->getHandle());
    ASSERT_NE(track, nullptr);
    ASSERT_GE(track->getNumKeyFrames(), 1);
    auto* kf = static_cast<Ogre::TransformKeyFrame*>(track->getKeyFrame(0));
    // key(100) / nodeBindScale(100) → identity: the pose stays at bind size.
    EXPECT_NEAR(kf->getScale().x, 1.0f, 1e-4f);
    EXPECT_NEAR(kf->getScale().y, 1.0f, 1e-4f);
    EXPECT_NEAR(kf->getScale().z, 1.0f, 1e-4f);
}
