#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "MaterialProcessor.h"

// Mock classes for Assimp's aiMaterial and aiScene
class MockAiMaterial : public aiMaterial {

};

class MockAiScene : public aiScene {
public:
    unsigned int mNumMaterials;
    MockAiMaterial** mMaterials;
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

// Test cases:

// Test if loadScene processes all materials
TEST_F(MaterialProcessorTest, LoadSceneProcessesAllMaterials) {
    MockAiScene scene;
    MockAiMaterial mockMaterial1, mockMaterial2;
    
    scene.mNumMaterials = 2;
    MockAiMaterial* materials[2] = { &mockMaterial1, &mockMaterial2 };
    scene.mMaterials = materials;

    processor.loadScene(&scene);

    EXPECT_EQ(processor.size(), 2);
}

// Test if material naming handles empty names
TEST_F(MaterialProcessorTest, MaterialNamingHandlesEmptyNames) {
    MockAiMaterial mockMaterial;

    processor.processMaterial(&mockMaterial);

    // Assuming there's a method called getLastProcessedMaterialName in MaterialProcessor
    EXPECT_EQ(processor.getLastProcessedMaterialName(), "importedMaterial0");
}
