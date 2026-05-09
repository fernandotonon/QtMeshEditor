#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <OgreMaterialManager.h>
#include <assimp/material.h>

#define private public
#include "MaterialProcessor.h"
#undef private

#include "../TestHelpers.h"

TEST(MaterialProcessorTest, LoadSceneProcessesAllMaterials) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    ensureMaterialManagerInitialised();
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
    ensureMaterialManagerInitialised();
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
    ensureMaterialManagerInitialised();
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

TEST(MaterialProcessorTest, LoadSceneWithNoMaterialsKeepsProcessorEmpty) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    ensureMaterialManagerInitialised();

    MaterialProcessor processor;
    aiScene scene{};
    scene.mNumMaterials = 0;
    scene.mMaterials = nullptr;

    processor.loadScene(&scene);

    EXPECT_EQ(processor.size(), 0UL);
}

TEST(MaterialProcessorTest, ProcessMaterialAppliesColorAndShininessProperties) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    ensureMaterialManagerInitialised();

    MaterialProcessor processor;
    aiScene scene{};
    aiMaterial material;

    aiString matName(std::string("MaterialProcessorProps"));
    material.AddProperty(&matName, AI_MATKEY_NAME);

    aiColor3D diffuse(0.8f, 0.2f, 0.1f);
    aiColor3D ambient(0.3f, 0.4f, 0.5f);
    aiColor3D specular(0.6f, 0.7f, 0.8f);
    aiColor3D emissive(0.1f, 0.2f, 0.3f);
    float shininess = 77.0f;

    material.AddProperty(&diffuse, 1, AI_MATKEY_COLOR_DIFFUSE);
    material.AddProperty(&ambient, 1, AI_MATKEY_COLOR_AMBIENT);
    material.AddProperty(&specular, 1, AI_MATKEY_COLOR_SPECULAR);
    material.AddProperty(&emissive, 1, AI_MATKEY_COLOR_EMISSIVE);
    material.AddProperty(&shininess, 1, AI_MATKEY_SHININESS);

    Ogre::MaterialPtr out = processor.processMaterial(&material, &scene);
    ASSERT_TRUE(out);

    Ogre::Pass* pass = out->getTechnique(0)->getPass(0);
    ASSERT_NE(pass, nullptr);

    auto d = pass->getDiffuse();
    auto a = pass->getAmbient();
    auto s = pass->getSpecular();
    auto e = pass->getSelfIllumination();
    EXPECT_NEAR(d.r, 0.8f, 1e-4f);
    EXPECT_NEAR(d.g, 0.2f, 1e-4f);
    EXPECT_NEAR(d.b, 0.1f, 1e-4f);
    EXPECT_NEAR(a.r, 0.3f, 1e-4f);
    EXPECT_NEAR(a.g, 0.4f, 1e-4f);
    EXPECT_NEAR(a.b, 0.5f, 1e-4f);
    EXPECT_NEAR(s.r, 0.6f, 1e-4f);
    EXPECT_NEAR(s.g, 0.7f, 1e-4f);
    EXPECT_NEAR(s.b, 0.8f, 1e-4f);
    EXPECT_NEAR(e.r, 0.1f, 1e-4f);
    EXPECT_NEAR(e.g, 0.2f, 1e-4f);
    EXPECT_NEAR(e.b, 0.3f, 1e-4f);
    EXPECT_NEAR(pass->getShininess(), 77.0f, 1e-4f);
}

TEST(MaterialProcessorTest, ProcessMaterialWithEmptyNameGeneratesImportedMaterialName) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    ensureMaterialManagerInitialised();

    MaterialProcessor processor;
    aiScene scene{};
    aiMaterial material;

    Ogre::MaterialPtr out = processor.processMaterial(&material, &scene);
    ASSERT_TRUE(out);
    EXPECT_THAT(out->getName(), ::testing::StartsWith("importedMaterial"));
}

TEST(MaterialProcessorTest, ProcessMaterialKeepsDefaultAmbientWhenAmbientIsBlack) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    ensureMaterialManagerInitialised();

    MaterialProcessor processor;
    aiScene scene{};
    aiMaterial material;
    aiString matName(std::string("MaterialProcessorAmbientBlack"));
    material.AddProperty(&matName, AI_MATKEY_NAME);

    aiColor3D ambientBlack(0.0f, 0.0f, 0.0f);
    material.AddProperty(&ambientBlack, 1, AI_MATKEY_COLOR_AMBIENT);

    Ogre::MaterialPtr out = processor.processMaterial(&material, &scene);
    ASSERT_TRUE(out);

    Ogre::Pass* pass = out->getTechnique(0)->getPass(0);
    ASSERT_NE(pass, nullptr);
    auto a = pass->getAmbient();

    // Branch check: ambient (0,0,0) from Assimp should not override Ogre default.
    EXPECT_GT(a.r, 0.9f);
    EXPECT_GT(a.g, 0.9f);
    EXPECT_GT(a.b, 0.9f);
}

TEST(MaterialProcessorTest, ProcessMaterialReturnsExistingMaterialIfAlreadyCreated) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    ensureMaterialManagerInitialised();

    const std::string name = "MaterialProcessorExistingMat";
    Ogre::MaterialPtr existing = Ogre::MaterialManager::getSingleton().create(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    ASSERT_TRUE(existing);

    MaterialProcessor processor;
    aiScene scene{};
    aiMaterial material;
    aiString matName(name);
    material.AddProperty(&matName, AI_MATKEY_NAME);

    Ogre::MaterialPtr out = processor.processMaterial(&material, &scene);
    ASSERT_TRUE(out);
    EXPECT_EQ(out.get(), existing.get());
    EXPECT_EQ(out->getName(), name);

    Ogre::MaterialManager::getSingleton().remove(name);
}

TEST(MaterialProcessorTest, ExistingMaterialWithoutNormalMapIsReturnedUnchanged) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    ensureMaterialManagerInitialised();

    const std::string name = "MaterialProcessorExistingNoNormal";
    Ogre::MaterialPtr existing = Ogre::MaterialManager::getSingleton().create(
        name, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
    ASSERT_TRUE(existing);
    ASSERT_NE(existing->getTechnique(0), nullptr);
    ASSERT_NE(existing->getTechnique(0)->getPass(0), nullptr);
    EXPECT_EQ(existing->getTechnique(0)->getPass(0)->getNumTextureUnitStates(), 0u);

    MaterialProcessor processor;
    aiScene scene{};
    aiMaterial material;
    aiString matName(name);
    material.AddProperty(&matName, AI_MATKEY_NAME);

    Ogre::MaterialPtr out = processor.processMaterial(&material, &scene);
    ASSERT_TRUE(out);
    EXPECT_EQ(out.get(), existing.get());
    EXPECT_EQ(out->getTechnique(0)->getPass(0)->getNumTextureUnitStates(), 0u);

    Ogre::MaterialManager::getSingleton().remove(name);
}

TEST(MaterialProcessorTest, LoadSceneUnnamedMaterialsGetSequentialImportedNames) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    ensureMaterialManagerInitialised();

    // Ensure predictable names even if previous tests left materials around.
    if (Ogre::MaterialManager::getSingleton().getByName("importedMaterial0"))
        Ogre::MaterialManager::getSingleton().remove("importedMaterial0");
    if (Ogre::MaterialManager::getSingleton().getByName("importedMaterial1"))
        Ogre::MaterialManager::getSingleton().remove("importedMaterial1");

    MaterialProcessor processor;
    aiScene scene{};
    scene.mNumMaterials = 2;
    scene.mMaterials = new aiMaterial*[2];
    scene.mMaterials[0] = new aiMaterial();
    scene.mMaterials[1] = new aiMaterial();

    processor.loadScene(&scene);

    ASSERT_EQ(processor.size(), 2UL);
    EXPECT_EQ(processor[0]->getName(), "importedMaterial0");
    EXPECT_EQ(processor[1]->getName(), "importedMaterial1");

    if (Ogre::MaterialManager::getSingleton().getByName("importedMaterial0"))
        Ogre::MaterialManager::getSingleton().remove("importedMaterial0");
    if (Ogre::MaterialManager::getSingleton().getByName("importedMaterial1"))
        Ogre::MaterialManager::getSingleton().remove("importedMaterial1");
}

// ─── Slice F3 PBR slot population ─────────────────────────────────────────────
//
// MaterialProcessor::processMaterial reads PBR-specific aiTextureType_*
// constants and binds them to the slice E canonical slot names. The
// behaviour is exercised end-to-end by SceneSaveLoadTest::
// RoundTrip_PbrSlots_PreservedAcrossExportImport in MeshImporterExporter_test
// — which uses tryInitOgre() so it has a full GL context for
// TextureManager::createManual to allocate a real texture handle.
//
// Stand-alone unit tests against MaterialProcessor were attempted but
// they crashed unit-tests-linux with SIGSEGV because the lightweight
// `auto ogreRoot = std::make_unique<Ogre::Root>();` test fixture used by
// the rest of this file doesn't initialise a render system, so
// TextureManager::createManual / getByName segfault on the missing GL
// state. The integration test in MeshImporterExporter_test covers the
// import → export → reimport round-trip end-to-end and is the primary
// regression guard for slice F3.
