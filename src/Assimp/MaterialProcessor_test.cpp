#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "MaterialProcessor.h"

// Mock classes for Assimp's aiScene
class MockAiScene : public aiScene {
public:
    unsigned int mNumMaterials;
    aiMaterial** mMaterials;
};

// Test suite for MaterialProcessor
class MaterialProcessorTest : public ::testing::Test {
protected:
    MaterialProcessor processor;

    // Add any setup or tear down methods if needed
    void SetUp() override {
        // Setup code if needed
    }

    void TearDown() override {
        // Cleanup code if needed
    }
};

TEST_F(MaterialProcessorTest, LoadSceneProcessesAllMaterials) {
    MockAiScene scene;
    aiMaterial mockMaterial1, mockMaterial2;

    scene.mNumMaterials = 2;
    aiMaterial* materials[2] = { &mockMaterial1, &mockMaterial2 };
    scene.mMaterials = materials;

    processor.loadScene(&scene);

    EXPECT_EQ(processor.size(), 2);
}

TEST_F(MaterialProcessorTest, MaterialIndexing) {
    MockAiScene scene;
    aiMaterial mockMaterial1, mockMaterial2;

    scene.mNumMaterials = 2;
    aiMaterial* materials[2] = { &mockMaterial1, &mockMaterial2 };
    scene.mMaterials = materials;

    processor.loadScene(&scene);

    EXPECT_NE(processor[0], nullptr);
    EXPECT_NE(processor[1], nullptr);
}

TEST_F(MaterialProcessorTest, MaterialSize) {
    MockAiScene scene;
    aiMaterial mockMaterial1, mockMaterial2, mockMaterial3;

    scene.mNumMaterials = 3;
    aiMaterial* materials[3] = { &mockMaterial1, &mockMaterial2, &mockMaterial3 };
    scene.mMaterials = materials;

    processor.loadScene(&scene);

    EXPECT_EQ(processor.size(), 3);
}