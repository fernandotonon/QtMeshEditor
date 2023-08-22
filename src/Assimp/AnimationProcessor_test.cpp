#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "AnimationProcessor.h"

// Mock classes for Assimp types
class MockAiScene : public aiScene {
public:
    unsigned int mNumAnimations;
    aiAnimation** mAnimations;
};

// Mock classes for Ogre types
class MockOgreSkeleton : public Ogre::Skeleton {
public:
    MockOgreSkeleton(Ogre::ResourceManager* creator, const Ogre::String& name, Ogre::ResourceHandle handle,
                     const Ogre::String& group, bool isManual = false, Ogre::ManualResourceLoader* loader = 0)
        : Ogre::Skeleton(creator, name, handle, group, isManual, loader) {}

    MOCK_METHOD2(createAnimation, Ogre::Animation*(const Ogre::String& name, Ogre::Real length));
    MOCK_METHOD1(getBone, Ogre::Bone*(const Ogre::String& name));
};

// Test if processAnimations processes all animations
TEST(AnimationProcessorTest, ProcessAllAnimations) {
    std::shared_ptr<MockOgreSkeleton> mockSkeleton= std::make_shared<MockOgreSkeleton>(nullptr, "MockSkeleton", 0, "Group");
    AnimationProcessor processor(mockSkeleton);

    MockAiScene scene;
    aiAnimation mockAnimation1, mockAnimation2;
    mockAnimation1.mName = aiString("Animation1");
    mockAnimation2.mName = aiString("Animation2");
    mockAnimation1.mDuration = 10.0f;
    mockAnimation2.mDuration = 5.0f;

    scene.mNumAnimations = 2;
    scene.mAnimations = new aiAnimation*[2];
    scene.mAnimations[0] = &mockAnimation1;
    scene.mAnimations[1] = &mockAnimation2;

    processor.processAnimations(&scene);

    EXPECT_CALL(*mockSkeleton, createAnimation("Animation1", 1.0f));
    EXPECT_CALL(*mockSkeleton, createAnimation("Animation2", 0.5f));
}
