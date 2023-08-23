#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "MaterialProcessor.h"

TEST(MaterialProcessorTest, LoadSceneProcessesAllMaterials) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    MaterialProcessor processor;
    aiScene scene;

    scene.mNumMaterials = 2;
    scene.mMaterials = new aiMaterial*[2];
    scene.mMaterials[0] = new aiMaterial;
    scene.mMaterials[1] = new aiMaterial;
    aiString matName1( std::string( "testMaterial1"));
    aiString matName2( std::string( "testMaterial2"));
    scene.mMaterials[0]->AddProperty(&matName1, AI_MATKEY_NAME);
    scene.mMaterials[1]->AddProperty(&matName2, AI_MATKEY_NAME);

    processor.loadScene(&scene);

    EXPECT_EQ(processor.size(), 2);
}

TEST(MaterialProcessorTest, MaterialIndexing) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    MaterialProcessor processor;
    aiScene scene;

    scene.mNumMaterials = 2;
    scene.mMaterials = new aiMaterial*[2];
    scene.mMaterials[0] = new aiMaterial;
    scene.mMaterials[1] = new aiMaterial;
    aiString matName1( std::string( "testMaterial1"));
    aiString matName2( std::string( "testMaterial2"));
    scene.mMaterials[0]->AddProperty(&matName1, AI_MATKEY_NAME);
    scene.mMaterials[1]->AddProperty(&matName2, AI_MATKEY_NAME);

    processor.loadScene(&scene);

    EXPECT_EQ(processor[0].get()->getName(), "testMaterial1");
    EXPECT_EQ(processor[1].get()->getName(), "testMaterial2");
}

TEST(MaterialProcessorTest, MaterialSize) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    MaterialProcessor processor;
    aiScene scene;

    scene.mNumMaterials = 3;
    scene.mMaterials = new aiMaterial*[3];
    scene.mMaterials[0] = new aiMaterial;
    scene.mMaterials[1] = new aiMaterial;
    scene.mMaterials[2] = new aiMaterial;
    aiString matName1( std::string( "testMaterial1"));
    aiString matName2( std::string( "testMaterial2"));
    aiString matName3( std::string( "testMaterial3"));
    scene.mMaterials[0]->AddProperty(&matName1, AI_MATKEY_NAME);
    scene.mMaterials[1]->AddProperty(&matName2, AI_MATKEY_NAME);
    scene.mMaterials[2]->AddProperty(&matName3, AI_MATKEY_NAME);

    processor.loadScene(&scene);

    EXPECT_EQ(processor.size(), 3);
}
