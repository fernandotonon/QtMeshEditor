#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "AnimationProcessor.h"

class MockOgreSkeleton : public Ogre::Skeleton {
public:
    MockOgreSkeleton(Ogre::ResourceManager* creator, const Ogre::String& name, Ogre::ResourceHandle handle,
                     const Ogre::String& group, bool isManual = false, Ogre::ManualResourceLoader* loader = 0)
        : Ogre::Skeleton(creator, name, handle, group, isManual, loader) {}

    MOCK_METHOD1(createAnimation, Ogre::Animation*(const Ogre::String& name, Ogre::Real length));
    MOCK_METHOD1(getBone, Ogre::Bone*(const Ogre::String& name));
};


// Test fixture
class AnimationProcessorTest : public ::testing::Test {
protected:
    std::shared_ptr<MockOgreSkeleton> mockSkeleton;
    AnimationProcessor processor;

    void SetUp() override {
        mockSkeleton = std::make_shared<MockOgreSkeleton>(nullptr, "MockSkeleton", 0, "Group");
        processor = AnimationProcessor(mockSkeleton);
    }

    void TearDown() override {
        // Cleanup code if needed
    }
};

// Test if processAnimations processes all animations
TEST_F(AnimationProcessorTest, ProcessAllAnimations) {
    MockAiScene scene;
    MockAiAnimation mockAnimation1, mockAnimation2;

    scene.mNumAnimations = 2;
    aiAnimation* animations[2] = { &mockAnimation1, &mockAnimation2 };
    scene.mAnimations = animations;

    EXPECT_CALL(*mockSkeleton, createAnimation(testing::_, testing::_)).Times(2);

    processor.processAnimations(&scene);
}
