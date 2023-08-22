#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "MaterialProcessor.h"

TEST(MaterialProcessorTest, LoadSceneProcessesAllMaterials) {
    MaterialProcessor processor;
    aiScene scene;

    scene.mNumMaterials = 2;
    scene.mMaterials = new aiMaterial[2];
    scene.mMaterials[0] = new aiMaterial;
    scene.mMaterials[1] = new aiMaterial;

    processor.loadScene(&scene);

    EXPECT_EQ(processor.size(), 2);
}

TEST(MaterialProcessorTest, MaterialIndexing) {
    MaterialProcessor processor;
    aiScene scene;
    aiMaterial mockMaterial1, mockMaterial2;

    scene.mNumMaterials = 2;
    aiMaterial* materials[2] = { &mockMaterial1, &mockMaterial2 };
    scene.mMaterials = materials;

    processor.loadScene(&scene);

    EXPECT_NE(processor[0], nullptr);
    EXPECT_NE(processor[1], nullptr);
}

TEST(MaterialProcessorTest, MaterialSize) {
    MaterialProcessor processor;
    aiScene scene;
    aiMaterial mockMaterial1, mockMaterial2, mockMaterial3;

    scene.mNumMaterials = 3;
    aiMaterial* materials[3] = { &mockMaterial1, &mockMaterial2, &mockMaterial3 };
    scene.mMaterials = materials;

    processor.loadScene(&scene);

    EXPECT_EQ(processor.size(), 3);
}