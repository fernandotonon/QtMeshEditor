#include <gtest/gtest.h>
#include "MeshProcessor.h"

class MockMeshProcessor: public MeshProcessor {
public:
    MockMeshProcessor(Ogre::SkeletonPtr skeleton): MeshProcessor(skeleton){}
    SubMeshData* processMesh(aiMesh* mesh, const aiScene* scene){return MeshProcessor::processMesh(mesh, scene);}
};

class MeshProcessorTest : public ::testing::Test {
protected:
    std::unique_ptr<Ogre::Root> ogreRoot;
    Ogre::SkeletonPtr mockSkeleton;
    aiScene mockScene;

    void SetUp() override {
        // Initialize the Ogre Root object and the mock skeleton
        ogreRoot = std::make_unique<Ogre::Root>();
        mockSkeleton = Ogre::SkeletonManager::getSingleton().create("MockSkeleton", Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME, true);
        mockSkeleton->createBone("MockBone");
    }
};

TEST_F(MeshProcessorTest, MeshDataProcessingTest) {
    MockMeshProcessor processor(mockSkeleton);

    // Set up mock aiMesh with vertices, normals, tangents, etc.
    auto mockMesh = std::make_unique<aiMesh>();

    // Setup vertices
    mockMesh->mNumVertices = 3;
    mockMesh->mVertices = new aiVector3D[3]{aiVector3D(0, 0, 0), aiVector3D(1, 1, 1), aiVector3D(2, 2, 2)};

    // Setup normals
    mockMesh->mNormals = new aiVector3D[3]{aiVector3D(0, 1, 0), aiVector3D(0, 1, 0), aiVector3D(0, 1, 0)};

    // Setup tangents
    mockMesh->mTangents = new aiVector3D[3]{aiVector3D(1, 0, 0), aiVector3D(1, 0, 0), aiVector3D(1, 0, 0)};

    // Setup bitangents
    mockMesh->mBitangents = new aiVector3D[3]{aiVector3D(0, 0, 1), aiVector3D(0, 0, 1), aiVector3D(0, 0, 1)};

    // Setup texture coordinates
    mockMesh->mTextureCoords[0] = new aiVector3D[3]{aiVector3D(0, 0, 0), aiVector3D(0.5, 0.5, 0), aiVector3D(1, 1, 0)};

    // Setup colors
    mockMesh->mColors[0] = new aiColor4D[3]{aiColor4D(1, 0, 0, 1), aiColor4D(0, 1, 0, 1), aiColor4D(0, 0, 1, 1)};

    // Assuming there's one bone affecting the mesh
    mockMesh->mNumBones = 1;
    mockMesh->mBones = new aiBone*[1];
    mockMesh->mBones[0] = new aiBone();
    mockMesh->mBones[0]->mNumWeights = mockMesh->mNumVertices;
    mockMesh->mBones[0]->mName = aiString("MockBone");

    // Assign blend indices and weights for each vertex
    mockMesh->mBones[0]->mWeights = new aiVertexWeight[mockMesh->mNumVertices];
    for (unsigned int i = 0; i < mockMesh->mNumVertices; i++) {
        mockMesh->mBones[0]->mWeights[i].mVertexId = i;
        mockMesh->mBones[0]->mWeights[i].mWeight = 0.5f*(i+1);  // Mock weight for example
    }

    // Process mesh
    SubMeshData* resultData = processor.processMesh(mockMesh.get(), &mockScene);

    // Verify vertices
    EXPECT_EQ(resultData->vertices[0], Ogre::Vector3(0, 0, 0));
    EXPECT_EQ(resultData->vertices[1], Ogre::Vector3(1, 1, 1));
    EXPECT_EQ(resultData->vertices[2], Ogre::Vector3(2, 2, 2));

    // Verify normals
    EXPECT_EQ(resultData->normals[0], Ogre::Vector3(0, 1, 0));
    EXPECT_EQ(resultData->normals[1], Ogre::Vector3(0, 1, 0));
    EXPECT_EQ(resultData->normals[2], Ogre::Vector3(0, 1, 0));

    // Verify tangents
    EXPECT_EQ(resultData->tangents[0], Ogre::Vector3(1, 0, 0));
    EXPECT_EQ(resultData->tangents[1], Ogre::Vector3(1, 0, 0));
    EXPECT_EQ(resultData->tangents[2], Ogre::Vector3(1, 0, 0));

    // Verify bitangents
    EXPECT_EQ(resultData->bitangents[0], Ogre::Vector3(0, 0, 1));
    EXPECT_EQ(resultData->bitangents[1], Ogre::Vector3(0, 0, 1));
    EXPECT_EQ(resultData->bitangents[2], Ogre::Vector3(0, 0, 1));

    // Verify texture coordinates
    EXPECT_EQ(resultData->texCoords[0], Ogre::Vector2(0, 0));
    EXPECT_EQ(resultData->texCoords[1], Ogre::Vector2(0.5, 0.5));
    EXPECT_EQ(resultData->texCoords[2], Ogre::Vector2(1, 1));

    // Verify colors
    EXPECT_EQ(resultData->colors[0], Ogre::ColourValue(1, 0, 0, 1));
    EXPECT_EQ(resultData->colors[1], Ogre::ColourValue(0, 1, 0, 1));
    EXPECT_EQ(resultData->colors[2], Ogre::ColourValue(0, 0, 1, 1));

    // Verify vertex data
    EXPECT_EQ(resultData->vertices[0], Ogre::Vector3(0, 0, 0));
    EXPECT_EQ(resultData->vertices[1], Ogre::Vector3(1, 1, 1));
    EXPECT_EQ(resultData->vertices[2], Ogre::Vector3(2, 2, 2));

    // Verify blend indices and blend weights for each vertex
    for (unsigned int i = 0; i < mockMesh->mNumVertices; i++) {
        EXPECT_EQ(resultData->blendIndices[i][0], 0);  // Assuming the bone only one bone
        EXPECT_FLOAT_EQ(resultData->blendWeights[i][0], 1.0f);  // the values get normalized (0.5+1+1.5)/3=1
    }
}
// TODO: test the other functions
