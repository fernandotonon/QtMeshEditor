#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "MaterialProcessor.h"

TEST(MaterialProcessorTest, LoadSceneProcessesAllMaterials) {
    MaterialProcessor processor;
    aiScene scene;

    scene.mNumMaterials = 2;
    scene.mMaterials = new aiMaterial*[2];
    scene.mMaterials[0] = new aiMaterial;
    scene.mMaterials[1] = new aiMaterial;
    scene.mMaterials[0]->AddProperty(&aiString("testMaterial1"), AI_MATKEY_NAME);
    scene.mMaterials[1]->AddProperty(&aiString("testMaterial2"), AI_MATKEY_NAME);

    processor.loadScene(&scene);

    EXPECT_EQ(processor.size(), 2);
}

TEST(MaterialProcessorTest, MaterialIndexing) {
    MaterialProcessor processor;
    aiScene scene;

    scene.mNumMaterials = 2;
    scene.mMaterials = new aiMaterial*[2];
    scene.mMaterials[0] = new aiMaterial;
    scene.mMaterials[1] = new aiMaterial;
    scene.mMaterials[0]->AddProperty(&aiString("testMaterial1"), AI_MATKEY_NAME);
    scene.mMaterials[1]->AddProperty(&aiString("testMaterial2"), AI_MATKEY_NAME);

    processor.loadScene(&scene);

    EXPECT_EQ(processor[0].get()->getName(), "testMaterial1");
    EXPECT_EQ(processor[1].get()->getName(), "testMaterial2");
}

TEST(MaterialProcessorTest, MaterialSize) {
    MaterialProcessor processor;
    aiScene scene;

    scene.mNumMaterials = 3;
    scene.mMaterials = new aiMaterial*[3];
    scene.mMaterials[0] = new aiMaterial;
    scene.mMaterials[1] = new aiMaterial;
    scene.mMaterials[2] = new aiMaterial;
    scene.mMaterials[0]->AddProperty(&aiString("testMaterial1"), AI_MATKEY_NAME);
    scene.mMaterials[1]->AddProperty(&aiString("testMaterial2"), AI_MATKEY_NAME);
    scene.mMaterials[2]->AddProperty(&aiString("testMaterial3"), AI_MATKEY_NAME);

    processor.loadScene(&scene);

    EXPECT_EQ(processor.size(), 3);
}