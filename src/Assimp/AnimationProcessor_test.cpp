#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "AnimationProcessor.h"

// Mock classes for Assimp types
class MockAiScene {
public:
    unsigned int mNumAnimations;
    aiAnimation** mAnimations;
};

class MockAiAnimation {
public:
    MOCK_METHOD0(mName, aiString&());
    MOCK_METHOD0(mDuration, double());
    unsigned int mNumChannels;
    aiNodeAnim** mChannels;
};

class MockAiNodeAnim {
public:
    MOCK_METHOD0(mNodeName, aiString&());
    unsigned int mNumPositionKeys;
    aiVectorKey* mPositionKeys;
    unsigned int mNumRotationKeys;
    aiQuatKey* mRotationKeys;
    unsigned int mNumScalingKeys;
    aiVectorKey* mScalingKeys;
};

// Mock classes for Ogre types
class MockOgreAnimation {
public:
    MOCK_METHOD2(createNodeTrack, Ogre::NodeAnimationTrack*(int, Ogre::Bone*));
    MOCK_METHOD1(createNodeTrack, void(double));
};

class MockOgreBone {
public:
    MOCK_METHOD0(getPosition, Ogre::Vector3&());
    MOCK_METHOD0(getOrientation, Ogre::Quaternion&());
    MOCK_METHOD0(getHandle, int());
};
class MockOgreSkeleton : public Ogre::Skeleton {
public:
    MockOgreSkeleton(Ogre::ResourceManager* creator, const Ogre::String& name, Ogre::ResourceHandle handle,
                     const Ogre::String& group, bool isManual = false, Ogre::ManualResourceLoader* loader = 0)
        : Ogre::Skeleton(creator, name, handle, group, isManual, loader) {}

    MOCK_METHOD2(createAnimation, Ogre::Animation*(const Ogre::String& name, Ogre::Real length));
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
