#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "AnimationProcessor.h"

// Test if processAnimations processes all animations
TEST(AnimationProcessorTest, ProcessAllAnimations) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    auto mockSkeleton= Ogre::SkeletonManager::getSingleton().create("MockSkeleton",Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);
    AnimationProcessor processor(mockSkeleton);

    aiScene scene;
    scene.mNumAnimations = 2;
    scene.mAnimations = new aiAnimation*[2];
    scene.mAnimations[0] = new aiAnimation;
    scene.mAnimations[1] = new aiAnimation;
    scene.mAnimations[0]->mName = aiString( std::string( "Animation1"));
    scene.mAnimations[1]->mName = aiString( std::string( "Animation2"));
    scene.mAnimations[0]->mNumChannels=0;
    scene.mAnimations[1]->mNumChannels=0;

    processor.processAnimations(&scene);

    EXPECT_EQ(mockSkeleton->getNumAnimations(), 2);
    EXPECT_EQ(mockSkeleton->getAnimation(0)->getName(), "Animation1");
    EXPECT_EQ(mockSkeleton->getAnimation(1)->getName(), "Animation2");
}
