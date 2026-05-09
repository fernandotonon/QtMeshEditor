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
// constants and binds them to the slice E canonical slot names. These tests
// stand a Texture Manager up so processMaterial's getByName lookup succeeds
// without needing a real file on disk, then assert the right slots appear.

namespace {

// Create a 1x1 white texture under a given name so MaterialProcessor's
// loadTexture/TextureManager::getByName lookup succeeds in tests without
// touching the filesystem.
Ogre::TexturePtr ensureTinyTexture(const std::string& name)
{
    auto& tm = Ogre::TextureManager::getSingleton();
    if (auto t = tm.getByName(name)) return t;
    return tm.createManual(
        name,
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
        Ogre::TEX_TYPE_2D, 1, 1, 0, Ogre::PF_BYTE_RGBA);
}

// Stamp a texture-file property on an aiMaterial for the given type.
// Mirrors what Assimp does internally when parsing FBX/glTF source files.
void addAiTexture(aiMaterial* mat, aiTextureType type, const char* path)
{
    aiString s;
    s.Set(path);
    mat->AddProperty(&s, _AI_MATKEY_TEXTURE_BASE, type, 0);
}

// Find a TUS by slot name on the first pass. Returns null if absent.
Ogre::TextureUnitState* findSlot(const Ogre::MaterialPtr& mat, const std::string& slot)
{
    if (!mat || mat->getNumTechniques() == 0) return nullptr;
    auto* pass = mat->getTechnique(0)->getPass(0);
    for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
        auto* tus = pass->getTextureUnitState(i);
        if (tus->getName() == slot) return tus;
    }
    return nullptr;
}

} // namespace

TEST(MaterialProcessorTest, PbrSlotsBoundFromAssimpTextureTypes) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    ensureMaterialManagerInitialised();

    // Pre-create the textures the importer will look up. These names
    // mirror what a glTF or modern FBX would give Assimp.
    ensureTinyTexture("baseColor.png");
    ensureTinyTexture("metalRough.png");
    ensureTinyTexture("ao.png");
    ensureTinyTexture("emissive.png");
    ensureTinyTexture("rough.png");

    MaterialProcessor processor;
    aiScene scene{};
    aiMaterial material;
    aiString matName(std::string("PbrSlotsMaterial"));
    material.AddProperty(&matName, AI_MATKEY_NAME);

    addAiTexture(&material, aiTextureType_BASE_COLOR,        "baseColor.png");
    addAiTexture(&material, aiTextureType_METALNESS,         "metalRough.png");
    addAiTexture(&material, aiTextureType_DIFFUSE_ROUGHNESS, "rough.png");
    addAiTexture(&material, aiTextureType_AMBIENT_OCCLUSION, "ao.png");
    addAiTexture(&material, aiTextureType_EMISSIVE,          "emissive.png");

    Ogre::MaterialPtr out = processor.processMaterial(&material, &scene);
    ASSERT_TRUE(out);

    EXPECT_NE(findSlot(out, "albedo"),    nullptr);
    EXPECT_NE(findSlot(out, "metallic"),  nullptr);
    EXPECT_NE(findSlot(out, "roughness"), nullptr);
    EXPECT_NE(findSlot(out, "ao"),        nullptr);
    EXPECT_NE(findSlot(out, "emissive"),  nullptr);

    // Slice F3 deliberately does NOT tag PBR-on-import materials with
    // pbr_workflow — see comment in MaterialProcessor.cpp. Tagging would
    // promote the material to Cook-Torrance via applyNormalMap's redirect
    // and produce dark output without IBL.
    auto* pass = out->getTechnique(0)->getPass(0);
    auto tag = pass->getUserObjectBindings().getUserAny("pbr_workflow");
    EXPECT_FALSE(tag.has_value());

    if (Ogre::MaterialManager::getSingleton().getByName("PbrSlotsMaterial"))
        Ogre::MaterialManager::getSingleton().remove("PbrSlotsMaterial");
}

// Many older FBX exporters write the base colour under aiTextureType_DIFFUSE
// (legacy Phong slot) only — never aiTextureType_BASE_COLOR. The albedo
// fallback aliases the diffuse_map texture under "albedo" so PBR-aware tools
// see a populated albedo slot without disturbing the visible FFP rendering.
TEST(MaterialProcessorTest, AlbedoFallsBackToLegacyDiffuseWhenNoBaseColor) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    ensureMaterialManagerInitialised();

    ensureTinyTexture("legacy_diffuse.png");
    ensureTinyTexture("metal.png");

    MaterialProcessor processor;
    aiScene scene{};
    aiMaterial material;
    aiString matName(std::string("LegacyDiffusePbrMaterial"));
    material.AddProperty(&matName, AI_MATKEY_NAME);

    addAiTexture(&material, aiTextureType_DIFFUSE,   "legacy_diffuse.png");
    addAiTexture(&material, aiTextureType_METALNESS, "metal.png");

    Ogre::MaterialPtr out = processor.processMaterial(&material, &scene);
    ASSERT_TRUE(out);

    auto* diffuse = findSlot(out, "diffuse_map");
    ASSERT_NE(diffuse, nullptr) << "Legacy diffuse slot must still be bound";
    auto* albedo = findSlot(out, "albedo");
    ASSERT_NE(albedo, nullptr) << "Albedo fallback alias missing";
    EXPECT_EQ(albedo->getTextureName(), diffuse->getTextureName())
        << "Albedo fallback should alias the diffuse_map texture";

    if (Ogre::MaterialManager::getSingleton().getByName("LegacyDiffusePbrMaterial"))
        Ogre::MaterialManager::getSingleton().remove("LegacyDiffusePbrMaterial");
}

// SHININESS is the FBX-side fallback location for the roughness texture
// when the exporter doesn't use aiTextureType_DIFFUSE_ROUGHNESS.
TEST(MaterialProcessorTest, RoughnessFallsBackToShininessTextureType) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    ensureMaterialManagerInitialised();

    ensureTinyTexture("base.png");
    ensureTinyTexture("rough_via_shininess.png");

    MaterialProcessor processor;
    aiScene scene{};
    aiMaterial material;
    aiString matName(std::string("ShininessRoughnessMaterial"));
    material.AddProperty(&matName, AI_MATKEY_NAME);

    addAiTexture(&material, aiTextureType_BASE_COLOR, "base.png");
    addAiTexture(&material, aiTextureType_SHININESS,  "rough_via_shininess.png");
    // No DIFFUSE_ROUGHNESS — must fall back to SHININESS for the roughness slot.

    Ogre::MaterialPtr out = processor.processMaterial(&material, &scene);
    ASSERT_TRUE(out);

    auto* roughness = findSlot(out, "roughness");
    ASSERT_NE(roughness, nullptr) << "Roughness slot missing — SHININESS fallback didn't fire";
    EXPECT_EQ(roughness->getTextureName(), "rough_via_shininess.png");

    if (Ogre::MaterialManager::getSingleton().getByName("ShininessRoughnessMaterial"))
        Ogre::MaterialManager::getSingleton().remove("ShininessRoughnessMaterial");
}

// Materials with no PBR maps at all stay non-tagged — the slot population
// path must not run in this case.
TEST(MaterialProcessorTest, NonPbrMaterialDoesNotGetPbrSlotsOrTag) {
    auto ogreRoot = std::make_unique<Ogre::Root>();
    ensureMaterialManagerInitialised();

    ensureTinyTexture("only_diffuse.png");

    MaterialProcessor processor;
    aiScene scene{};
    aiMaterial material;
    aiString matName(std::string("NonPbrMaterial"));
    material.AddProperty(&matName, AI_MATKEY_NAME);
    addAiTexture(&material, aiTextureType_DIFFUSE, "only_diffuse.png");

    Ogre::MaterialPtr out = processor.processMaterial(&material, &scene);
    ASSERT_TRUE(out);

    EXPECT_NE(findSlot(out, "diffuse_map"), nullptr);
    EXPECT_EQ(findSlot(out, "albedo"),     nullptr);
    EXPECT_EQ(findSlot(out, "metallic"),   nullptr);
    EXPECT_EQ(findSlot(out, "roughness"),  nullptr);
    EXPECT_EQ(findSlot(out, "ao"),         nullptr);
    EXPECT_EQ(findSlot(out, "emissive"),   nullptr);

    auto* pass = out->getTechnique(0)->getPass(0);
    auto tag = pass->getUserObjectBindings().getUserAny("pbr_workflow");
    EXPECT_FALSE(tag.has_value());

    if (Ogre::MaterialManager::getSingleton().getByName("NonPbrMaterial"))
        Ogre::MaterialManager::getSingleton().remove("NonPbrMaterial");
}
