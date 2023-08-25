#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "BoneProcessor.h"

class BoneProcessorTest : public ::testing::Test {
protected:
    std::unique_ptr<Ogre::Root> ogreRoot;
    Ogre::SkeletonPtr mockSkeleton;
    BoneProcessor processor;

    aiScene mockScene;

    void SetUp() override {
        ogreRoot = std::make_unique<Ogre::Root>();
        mockSkeleton = Ogre::SkeletonManager::getSingleton().create("MockSkeleton", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);
        processor = BoneProcessor();
        // Set up mockScene and mockMesh here if necessary
    }

    void TearDown() override {
        // Cleanup code if needed
    }
};

TEST_F(BoneProcessorTest, CreateBones) {
    mockScene.mNumMeshes = 1;
    mockScene.mMeshes = new aiMesh*[1];
    mockScene.mMeshes[0] = new aiMesh();
    mockScene.mMeshes[0]->mNumBones = 2;
    mockScene.mMeshes[0]->mBones = new aiBone*[2];
    mockScene.mMeshes[0]->mBones[0] = new aiBone();
    mockScene.mMeshes[0]->mBones[1] = new aiBone();
    mockScene.mMeshes[0]->mBones[0]->mName = aiString("Bone1");
    mockScene.mMeshes[0]->mBones[1]->mName = aiString("Bone2");
    mockScene.mMeshes[0]->mBones[0]->mOffsetMatrix = aiMatrix4x4();
    mockScene.mMeshes[0]->mBones[1]->mOffsetMatrix = aiMatrix4x4();

    processor.processBones(mockSkeleton, &mockScene);

    EXPECT_TRUE(mockSkeleton->hasBone("Bone1"));
    EXPECT_TRUE(mockSkeleton->hasBone("Bone2"));
    EXPECT_FALSE(mockSkeleton->hasBone("Bone3"));

    // Running processBones again
    processor.processBones(mockSkeleton, &mockScene);

    // Assuming 2 bones were already present in the skeleton
    const size_t expectedBoneCount = 2;
    EXPECT_EQ(mockSkeleton->getNumBones(), expectedBoneCount);
}

TEST_F(BoneProcessorTest, BoneHierarchy) {
    mockScene.mNumMeshes = 1;
    mockScene.mMeshes = new aiMesh*[1];
    mockScene.mMeshes[0] = new aiMesh();
    mockScene.mMeshes[0]->mNumBones = 1;
    mockScene.mMeshes[0]->mBones = new aiBone*[1];

    aiBone* parentBone = new aiBone();
    aiBone* childBone = new aiBone();
    parentBone->mOffsetMatrix = aiMatrix4x4(
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
        );
    childBone->mOffsetMatrix = aiMatrix4x4(
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
        );
    parentBone->mName = aiString("RootBone");
    childBone->mName = aiString("ChildBone");

    aiNode* parentNode = new aiNode();
    aiNode* childNode = new aiNode();

    parentNode->mName = aiString("RootBone");
    parentNode->mNumChildren = 1;
    parentNode->mChildren = new aiNode*[1];
    parentNode->mChildren[0] = childNode;
    parentBone->mNode = parentNode;

    childNode->mName = aiString("ChildBone");
    childNode->mParent = parentNode;
    childBone->mNode = childNode;

    mockScene.mMeshes[0]->mBones[0] = childBone;

    processor.processBones(mockSkeleton, &mockScene);

    EXPECT_EQ(mockSkeleton->getBone("ChildBone")->getParent(), mockSkeleton->getBone("RootBone"));
}


TEST_F(BoneProcessorTest, BoneTransformation) {
    aiNode* rootNode = new aiNode();
    rootNode->mName = aiString("RootBone");
    aiBone* mockBone = new aiBone();
    mockBone->mName = aiString("TestBone");
    aiNode* mockNode = new aiNode();
    mockBone->mNode = mockNode;
    mockNode->mParent = rootNode;

    aiMatrix4x4 mockOffsetMatrix(
        1, 0, 0, -2,
        0, 2, 0, -6,
        0, 0, 1, -4,
        0, 0, 0, 1
        );

    mockBone->mOffsetMatrix = mockOffsetMatrix;

    mockScene.mNumMeshes = 1;
    mockScene.mMeshes = new aiMesh*[1];
    mockScene.mMeshes[0] = new aiMesh();
    mockScene.mMeshes[0]->mNumBones = 1;
    mockScene.mMeshes[0]->mBones = new aiBone*[1];
    mockScene.mMeshes[0]->mBones[0] = mockBone;

    processor.processBones(mockSkeleton, &mockScene);

    Ogre::Bone* testBone = mockSkeleton->getBone("TestBone");

    Ogre::Vector3 expectedPosition(2, 3, 4);
    Ogre::Quaternion expectedOrientation(1, 0, 0, 0);  // No rotation, so it's an identity quaternion
    Ogre::Vector3 expectedScale(1, 0.5, 1);

    EXPECT_EQ(testBone->getPosition(), expectedPosition);
    EXPECT_EQ(testBone->getOrientation(), expectedOrientation);
    EXPECT_EQ(testBone->getScale(), expectedScale);
}

TEST_F(BoneProcessorTest, AnimationBones) {
    aiAnimation* mockAnimation = new aiAnimation();
    aiNodeAnim* mockNodeAnim = new aiNodeAnim();
    mockNodeAnim->mNodeName = aiString("AnimBone");

    mockAnimation->mNumChannels = 1;
    mockAnimation->mChannels = new aiNodeAnim*[1];
    mockAnimation->mChannels[0] = mockNodeAnim;

    mockScene.mNumAnimations = 1;
    mockScene.mAnimations = new aiAnimation*[1];
    mockScene.mAnimations[0] = mockAnimation;

    processor.processBones(mockSkeleton, &mockScene);

    // Check if animation bone "AnimBone" is correctly created
    EXPECT_TRUE(mockSkeleton->hasBone("AnimBone"));
}
