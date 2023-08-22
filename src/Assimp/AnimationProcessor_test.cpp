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

    scene.mNumAnimations = 2;
    aiAnimation* animations[2] = { &mockAnimation1, &mockAnimation2 };
    scene.mAnimations = animations;

    processor.processAnimations(&scene);

    EXPECT_CALL(*mockSkeleton, createAnimation(testing::_, testing::_)).Times(2);
}
