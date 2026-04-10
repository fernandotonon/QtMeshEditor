#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <cmath>
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
    mockScene.mRootNode = new aiNode();
    mockScene.mRootNode->mName = aiString("Root");
    mockScene.mRootNode->mTransformation = aiMatrix4x4();
    mockScene.mRootNode->mNumChildren = 2;
    mockScene.mRootNode->mChildren = new aiNode*[2];
    mockScene.mRootNode->mChildren[0] = new aiNode();
    mockScene.mRootNode->mChildren[1] = new aiNode();
    mockScene.mRootNode->mChildren[0]->mName = aiString("Bone1");
    mockScene.mRootNode->mChildren[1]->mName = aiString("Bone2");
    mockScene.mRootNode->mChildren[0]->mTransformation = aiMatrix4x4();
    mockScene.mRootNode->mChildren[1]->mTransformation = aiMatrix4x4();
    mockScene.mMeshes[0]->mBones[0]->mNode = mockScene.mRootNode->mChildren[0];
    mockScene.mMeshes[0]->mBones[1]->mNode = mockScene.mRootNode->mChildren[1];

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

    auto parentBone = std::make_unique<aiBone>();
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

    mockScene.mRootNode = parentNode;
    mockScene.mRootNode->mTransformation = aiMatrix4x4();

    processor.processBones(mockSkeleton, &mockScene);

    EXPECT_EQ(mockSkeleton->getBone("ChildBone")->getParent(), mockSkeleton->getBone("RootBone"));
}


TEST_F(BoneProcessorTest, BoneTransformation) {
    aiNode* rootNode = new aiNode();
    rootNode->mName = aiString("RootBone");
    aiBone* mockBone = new aiBone();
    mockBone->mName = aiString("TestBone");
    aiNode* mockNode = new aiNode();

    mockNode->mName = aiString("TestBone");
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

    mockScene.mRootNode = rootNode;
    mockScene.mRootNode->mTransformation = aiMatrix4x4();
    mockScene.mRootNode->mNumChildren = 1;
    mockScene.mRootNode->mChildren = new aiNode*[1];
    mockScene.mRootNode->mChildren[0] = mockNode;

    processor.processBones(mockSkeleton, &mockScene);

    Ogre::Bone* testBone = mockSkeleton->getBone("TestBone");

    Ogre::Vector3 expectedPosition(2, 3, 4);
    Ogre::Quaternion expectedOrientation(1, 0, 0, 0);  // No rotation, so it's an identity quaternion
    Ogre::Vector3 expectedScale(1, 0.5, 1);

    EXPECT_EQ(testBone->getPosition(), expectedPosition);
    EXPECT_EQ(testBone->getOrientation(), expectedOrientation);
    EXPECT_EQ(testBone->getScale(), expectedScale);
}

TEST_F(BoneProcessorTest, BakeZupToYupRotatesOnlyRootBones)
{
    Ogre::SkeletonPtr skeleton = Ogre::SkeletonManager::getSingleton().create(
        "BakeZupToYupSkeleton",
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
        true);
    ASSERT_TRUE(skeleton);

    Ogre::Bone* root = skeleton->createBone("Root");
    Ogre::Bone* child = skeleton->createBone("Child");
    root->addChild(child);

    root->setPosition(Ogre::Vector3(0.0f, 0.0f, 2.0f));
    root->setOrientation(Ogre::Quaternion::IDENTITY);
    child->setPosition(Ogre::Vector3(1.0f, 2.0f, 3.0f));
    child->setOrientation(Ogre::Quaternion::IDENTITY);

    BoneProcessor::bakeZupToYup(skeleton);

    const Ogre::Quaternion rx90(Ogre::Degree(90), Ogre::Vector3::UNIT_X);
    const Ogre::Vector3 expectedRootPos = rx90 * Ogre::Vector3(0.0f, 0.0f, 2.0f);
    EXPECT_NEAR(root->getPosition().x, expectedRootPos.x, 1e-5);
    EXPECT_NEAR(root->getPosition().y, expectedRootPos.y, 1e-5);
    EXPECT_NEAR(root->getPosition().z, expectedRootPos.z, 1e-5);
    EXPECT_NEAR(std::abs(root->getOrientation().Dot(rx90)), 1.0f, 1e-5);

    // Child local transforms are preserved.
    EXPECT_EQ(child->getPosition(), Ogre::Vector3(1.0f, 2.0f, 3.0f));
    EXPECT_EQ(child->getOrientation(), Ogre::Quaternion::IDENTITY);
}

TEST_F(BoneProcessorTest, AnimationOnlyHierarchyCreatesAnimatedBonesAndLeafChildren)
{
    mockScene.mNumMeshes = 0;
    mockScene.mMeshes = nullptr;

    aiNode* rootNode = new aiNode();
    rootNode->mName = aiString("Root");
    rootNode->mTransformation = aiMatrix4x4();

    aiNode* armatureNode = new aiNode();
    armatureNode->mName = aiString("Armature");
    armatureNode->mTransformation = aiMatrix4x4();
    armatureNode->mParent = rootNode;

    aiNode* hipsNode = new aiNode();
    hipsNode->mName = aiString("Hips");
    hipsNode->mTransformation = aiMatrix4x4();
    hipsNode->mParent = armatureNode;

    aiNode* tipNode = new aiNode();
    tipNode->mName = aiString("HandTip");
    tipNode->mTransformation = aiMatrix4x4();
    tipNode->mParent = hipsNode;

    rootNode->mNumChildren = 1;
    rootNode->mChildren = new aiNode*[1]{armatureNode};
    armatureNode->mNumChildren = 1;
    armatureNode->mChildren = new aiNode*[1]{hipsNode};
    hipsNode->mNumChildren = 1;
    hipsNode->mChildren = new aiNode*[1]{tipNode};
    tipNode->mNumChildren = 0;
    tipNode->mChildren = nullptr;

    mockScene.mRootNode = rootNode;

    aiAnimation* anim = new aiAnimation();
    anim->mName = aiString("Take001");
    anim->mNumChannels = 1;
    anim->mChannels = new aiNodeAnim*[1];
    anim->mChannels[0] = new aiNodeAnim();
    anim->mChannels[0]->mNodeName = aiString("Hips");
    anim->mChannels[0]->mNumPositionKeys = 0;
    anim->mChannels[0]->mNumRotationKeys = 0;
    anim->mChannels[0]->mNumScalingKeys = 0;

    mockScene.mNumAnimations = 1;
    mockScene.mAnimations = new aiAnimation*[1]{anim};

    processor.processBones(mockSkeleton, &mockScene);

    EXPECT_TRUE(mockSkeleton->hasBone("Hips"));
    EXPECT_TRUE(mockSkeleton->hasBone("HandTip"));
    EXPECT_FALSE(mockSkeleton->hasBone("Armature"));
    EXPECT_EQ(mockSkeleton->getBone("HandTip")->getParent(), mockSkeleton->getBone("Hips"));
}
