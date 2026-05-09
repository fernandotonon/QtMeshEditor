#include <gtest/gtest.h>
#include "MaterialPresetLibrary.h"
#include "Manager.h"
#include "RTShaderHelper.h"
#include "SelectionSet.h"
#include "TestHelpers.h"
#include <OgreRTShaderSystem.h>
#include <QApplication>
#include <QCoreApplication>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QThread>

class MaterialPresetLibraryTests : public ::testing::Test {
protected:
    QApplication* app = nullptr;

    void SetUp() override {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
    }

    void TearDown() override {
        if (app) app->processEvents();
    }

    Ogre::Entity* createSelectedEntity(const QString& nodeName,
                                       const QString& entityName,
                                       const QString& meshName)
    {
        auto mesh = createInMemoryTriangleMesh(meshName.toStdString());
        EXPECT_NE(mesh, nullptr);

        auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
        auto* node = Manager::getSingleton()->addSceneNode(nodeName);
        EXPECT_NE(node, nullptr);

        if (!mesh || !node)
            return nullptr;

        auto* entity = sceneMgr->createEntity(entityName.toStdString(), mesh);
        EXPECT_NE(entity, nullptr);
        if (!entity)
            return nullptr;

        node->attachObject(entity);
        SelectionSet::getSingleton()->selectOne(entity);
        return entity;
    }
};

TEST_F(MaterialPresetLibraryTests, SingletonInstance) {
    auto* inst = MaterialPresetLibrary::instance();
    ASSERT_NE(inst, nullptr);
    EXPECT_EQ(inst, MaterialPresetLibrary::instance());
}

TEST_F(MaterialPresetLibraryTests, KillAndRecreate) {
    auto* inst1 = MaterialPresetLibrary::instance();
    ASSERT_NE(inst1, nullptr);

    MaterialPresetLibrary::kill();

    auto* inst2 = MaterialPresetLibrary::instance();
    ASSERT_NE(inst2, nullptr);
    // After kill+recreate, it should be a new instance
    // (pointer may or may not differ due to memory reuse, but it should work)
    EXPECT_NE(inst2, nullptr);
}

TEST_F(MaterialPresetLibraryTests, PresetNamesNotEmpty) {
    auto* inst = MaterialPresetLibrary::instance();
    QStringList names = inst->presetNames();
    EXPECT_FALSE(names.isEmpty());
    EXPECT_GE(names.size(), 10); // We know there are 12 presets
}

TEST_F(MaterialPresetLibraryTests, PresetNamesContainsExpected) {
    auto* inst = MaterialPresetLibrary::instance();
    QStringList names = inst->presetNames();

    EXPECT_TRUE(names.contains("Plastic (Red)"));
    EXPECT_TRUE(names.contains("Plastic (Blue)"));
    EXPECT_TRUE(names.contains("Plastic (White)"));
    EXPECT_TRUE(names.contains("Metal (Silver)"));
    EXPECT_TRUE(names.contains("Metal (Gold)"));
    EXPECT_TRUE(names.contains("Metal (Copper)"));
    EXPECT_TRUE(names.contains("Wood (Oak)"));
    EXPECT_TRUE(names.contains("Wood (Birch)"));
    EXPECT_TRUE(names.contains("Glass (Clear)"));
    EXPECT_TRUE(names.contains("Glass (Tinted)"));
    EXPECT_TRUE(names.contains("Unlit (White)"));
    EXPECT_TRUE(names.contains("Wireframe"));
}

TEST_F(MaterialPresetLibraryTests, ApplyPresetWithoutSelection) {
    // No Ogre needed: applyPreset returns early when MaterialManager is absent
    // and the selection resolves to no entities.
    Manager::kill();
    QThread::msleep(50);

    auto* inst = MaterialPresetLibrary::instance();
    SelectionSet::getSingleton()->clear();
    EXPECT_NO_THROW(inst->applyPreset("Plastic (Red)"));
}

TEST_F(MaterialPresetLibraryTests, ApplyPresetEmitsSignalWithEntity) {
    Manager::kill();
    QThread::msleep(50);

    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
    createStandardOgreMaterials();

    auto mesh = createInMemoryTriangleMesh("PresetTestMesh");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("PresetTestNode");
    auto* entity = sceneMgr->createEntity("PresetTestEnt", mesh);
    node->attachObject(entity);

    SelectionSet::getSingleton()->selectOne(entity);

    auto* inst = MaterialPresetLibrary::instance();
    QSignalSpy spy(inst, &MaterialPresetLibrary::presetApplied);

    inst->applyPreset("Plastic (Blue)");

    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(spy.at(0).at(0).toString(), "Plastic (Blue)");

    // Verify the material was applied
    EXPECT_EQ(std::string(entity->getSubEntity(0)->getMaterialName()), "Preset/Plastic (Blue)");

    SelectionSet::getSingleton()->clear();
}

TEST_F(MaterialPresetLibraryTests, ApplyAllPresets) {
    Manager::kill();
    QThread::msleep(50);

    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
    createStandardOgreMaterials();

    auto mesh = createInMemoryTriangleMesh("PresetAllMesh");
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    auto* node = Manager::getSingleton()->addSceneNode("PresetAllNode");
    auto* entity = sceneMgr->createEntity("PresetAllEnt", mesh);
    node->attachObject(entity);

    SelectionSet::getSingleton()->selectOne(entity);

    auto* inst = MaterialPresetLibrary::instance();
    QStringList names = inst->presetNames();

    // Apply every preset to ensure none crash
    for (const QString& name : names) {
        EXPECT_NO_THROW(inst->applyPreset(name));
    }

    SelectionSet::getSingleton()->clear();
}

TEST_F(MaterialPresetLibraryTests, QmlInstanceReturnsSameAsInstance) {
    auto* inst1 = MaterialPresetLibrary::instance();
    auto* inst2 = MaterialPresetLibrary::qmlInstance(nullptr, nullptr);
    EXPECT_EQ(inst1, inst2);
}

TEST_F(MaterialPresetLibraryTests, PlasticPresetConfiguresExpectedMaterialProperties) {
    Manager::kill();
    QThread::msleep(50);

    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
    createStandardOgreMaterials();

    Ogre::Entity* entity = createSelectedEntity("PlasticNode", "PlasticEntity", "PlasticMesh");
    ASSERT_NE(entity, nullptr);

    auto* inst = MaterialPresetLibrary::instance();
    inst->applyPreset("Plastic (White)");

    auto mat = Ogre::MaterialManager::getSingleton().getByName("Preset/Plastic (White)");
    ASSERT_TRUE(bool(mat));
    Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
    EXPECT_FLOAT_EQ(pass->getDiffuse().r, 0.9f);
    EXPECT_FLOAT_EQ(pass->getDiffuse().g, 0.9f);
    EXPECT_FLOAT_EQ(pass->getDiffuse().b, 0.9f);
    EXPECT_FLOAT_EQ(pass->getShininess(), 30.0f);
    EXPECT_EQ(QString::fromStdString(entity->getSubEntity(0)->getMaterialName()), QString("Preset/Plastic (White)"));
}

TEST_F(MaterialPresetLibraryTests, MetalPresetConfiguresExpectedMaterialProperties) {
    Manager::kill();
    QThread::msleep(50);

    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
    createStandardOgreMaterials();

    Ogre::Entity* entity = createSelectedEntity("MetalNode", "MetalEntity", "MetalMesh");
    ASSERT_NE(entity, nullptr);

    auto* inst = MaterialPresetLibrary::instance();
    inst->applyPreset("Metal (Gold)");

    auto mat = Ogre::MaterialManager::getSingleton().getByName("Preset/Metal (Gold)");
    ASSERT_TRUE(bool(mat));
    Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
    EXPECT_FLOAT_EQ(pass->getDiffuse().r, 0.9f);
    EXPECT_FLOAT_EQ(pass->getDiffuse().g, 0.75f);
    EXPECT_FLOAT_EQ(pass->getDiffuse().b, 0.3f);
    EXPECT_FLOAT_EQ(pass->getShininess(), 80.0f);
    EXPECT_EQ(QString::fromStdString(entity->getSubEntity(0)->getMaterialName()), QString("Preset/Metal (Gold)"));
}

TEST_F(MaterialPresetLibraryTests, WoodPresetConfiguresExpectedMaterialProperties) {
    Manager::kill();
    QThread::msleep(50);

    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
    createStandardOgreMaterials();

    Ogre::Entity* entity = createSelectedEntity("WoodNode", "WoodEntity", "WoodMesh");
    ASSERT_NE(entity, nullptr);

    auto* inst = MaterialPresetLibrary::instance();
    inst->applyPreset("Wood (Oak)");

    auto mat = Ogre::MaterialManager::getSingleton().getByName("Preset/Wood (Oak)");
    ASSERT_TRUE(bool(mat));
    Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
    EXPECT_FLOAT_EQ(pass->getDiffuse().r, 0.6f);
    EXPECT_FLOAT_EQ(pass->getDiffuse().g, 0.4f);
    EXPECT_FLOAT_EQ(pass->getDiffuse().b, 0.2f);
    EXPECT_FLOAT_EQ(pass->getShininess(), 5.0f);
    EXPECT_EQ(QString::fromStdString(entity->getSubEntity(0)->getMaterialName()), QString("Preset/Wood (Oak)"));
}

TEST_F(MaterialPresetLibraryTests, GlassPresetConfiguresTransparentMaterialProperties) {
    Manager::kill();
    QThread::msleep(50);

    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
    createStandardOgreMaterials();

    Ogre::Entity* entity = createSelectedEntity("GlassNode", "GlassEntity", "GlassMesh");
    ASSERT_NE(entity, nullptr);

    auto* inst = MaterialPresetLibrary::instance();
    inst->applyPreset("Glass (Tinted)");

    auto mat = Ogre::MaterialManager::getSingleton().getByName("Preset/Glass (Tinted)");
    ASSERT_TRUE(bool(mat));
    Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
    EXPECT_FLOAT_EQ(pass->getDiffuse().a, 0.55f);
    EXPECT_FLOAT_EQ(pass->getShininess(), 100.0f);
    EXPECT_FALSE(pass->getDepthWriteEnabled());
    EXPECT_EQ(QString::fromStdString(entity->getSubEntity(0)->getMaterialName()), QString("Preset/Glass (Tinted)"));
}

TEST_F(MaterialPresetLibraryTests, UnlitAndWireframePresetsDisableLighting) {
    Manager::kill();
    QThread::msleep(50);

    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    ASSERT_TRUE(canLoadMeshFiles()) << "entity creation requires GL (Xvfb in CI)";
    createStandardOgreMaterials();

    Ogre::Entity* entity = createSelectedEntity("SpecialNode", "SpecialEntity", "SpecialMesh");
    ASSERT_NE(entity, nullptr);

    auto* inst = MaterialPresetLibrary::instance();
    inst->applyPreset("Unlit (White)");

    auto unlitMat = Ogre::MaterialManager::getSingleton().getByName("Preset/Unlit (White)");
    ASSERT_TRUE(bool(unlitMat));
    Ogre::Pass* unlitPass = unlitMat->getTechnique(0)->getPass(0);
    EXPECT_FALSE(unlitPass->getLightingEnabled());

    inst->applyPreset("Wireframe");
    auto wireMat = Ogre::MaterialManager::getSingleton().getByName("Preset/Wireframe");
    ASSERT_TRUE(bool(wireMat));
    Ogre::Pass* wirePass = wireMat->getTechnique(0)->getPass(0);
    EXPECT_FALSE(wirePass->getLightingEnabled());
    EXPECT_EQ(wirePass->getPolygonMode(), Ogre::PM_WIREFRAME);
    EXPECT_EQ(QString::fromStdString(entity->getSubEntity(0)->getMaterialName()), QString("Preset/Wireframe"));
}

// ── PBR Templates (slice E) ──────────────────────────────────────────────────

namespace {
// Canonical PBR slot order — matches the kPbrSlots array in the cpp.
const QStringList kExpectedPbrSlots = {
    "albedo", "normal_map", "metallic", "roughness", "ao", "emissive"
};
} // namespace

TEST_F(MaterialPresetLibraryTests, PresetNamesContainsPbrTemplates) {
    auto names = MaterialPresetLibrary::instance()->presetNames();
    EXPECT_TRUE(names.contains("Metallic-Roughness"));
    EXPECT_TRUE(names.contains("Specular-Glossiness"));
    EXPECT_TRUE(names.contains("Unlit PBR"));
}

TEST_F(MaterialPresetLibraryTests, MetallicRoughnessTemplateCreatesSixCanonicalSlots) {
    Manager::kill();
    QThread::msleep(50);
    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    ASSERT_TRUE(canLoadMeshFiles());
    createStandardOgreMaterials();

    Ogre::Entity* entity = createSelectedEntity("PbrMR_Node", "PbrMR_Entity", "PbrMR_Mesh");
    ASSERT_NE(entity, nullptr);

    MaterialPresetLibrary::instance()->applyPreset("Metallic-Roughness");

    auto mat = Ogre::MaterialManager::getSingleton().getByName("Preset/Metallic-Roughness");
    ASSERT_TRUE(bool(mat));
    Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);

    ASSERT_EQ(pass->getNumTextureUnitStates(), 6u);
    for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
        EXPECT_EQ(QString::fromStdString(pass->getTextureUnitState(i)->getName()),
                  kExpectedPbrSlots[i])
            << "PBR slot " << i << " out of order";
    }

    // Workflow tag readable by future PBR sub-render-state.
    auto tag = pass->getUserObjectBindings().getUserAny(
        MaterialPresetLibrary::kPbrWorkflowKey);
    ASSERT_FALSE(tag.has_value() == false);
    EXPECT_EQ(Ogre::any_cast<Ogre::String>(tag),
              Ogre::String(MaterialPresetLibrary::kPbrWorkflowMetallic));

    // Phong approximation sanity: shininess in the lit window.
    EXPECT_GE(pass->getShininess(), 20.0f);
    EXPECT_LE(pass->getShininess(), 80.0f);
    EXPECT_TRUE(pass->getLightingEnabled());

    EXPECT_EQ(QString::fromStdString(entity->getSubEntity(0)->getMaterialName()),
              QString("Preset/Metallic-Roughness"));
}

TEST_F(MaterialPresetLibraryTests, SpecularGlossinessTemplateTagsWorkflow) {
    Manager::kill();
    QThread::msleep(50);
    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    ASSERT_TRUE(canLoadMeshFiles());
    createStandardOgreMaterials();

    Ogre::Entity* entity = createSelectedEntity("PbrSG_Node", "PbrSG_Entity", "PbrSG_Mesh");
    ASSERT_NE(entity, nullptr);

    MaterialPresetLibrary::instance()->applyPreset("Specular-Glossiness");

    auto mat = Ogre::MaterialManager::getSingleton().getByName("Preset/Specular-Glossiness");
    ASSERT_TRUE(bool(mat));
    Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);

    EXPECT_EQ(pass->getNumTextureUnitStates(), 6u);
    auto tag = pass->getUserObjectBindings().getUserAny(
        MaterialPresetLibrary::kPbrWorkflowKey);
    EXPECT_EQ(Ogre::any_cast<Ogre::String>(tag),
              Ogre::String(MaterialPresetLibrary::kPbrWorkflowSpecular));
    EXPECT_TRUE(pass->getLightingEnabled());
}

TEST_F(MaterialPresetLibraryTests, UnlitPbrDisablesLightingButKeepsSlots) {
    Manager::kill();
    QThread::msleep(50);
    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    ASSERT_TRUE(canLoadMeshFiles());
    createStandardOgreMaterials();

    Ogre::Entity* entity = createSelectedEntity("PbrUL_Node", "PbrUL_Entity", "PbrUL_Mesh");
    ASSERT_NE(entity, nullptr);

    MaterialPresetLibrary::instance()->applyPreset("Unlit PBR");

    auto mat = Ogre::MaterialManager::getSingleton().getByName("Preset/Unlit PBR");
    ASSERT_TRUE(bool(mat));
    Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);

    EXPECT_FALSE(pass->getLightingEnabled());
    EXPECT_EQ(pass->getNumTextureUnitStates(), 6u);
    auto tag = pass->getUserObjectBindings().getUserAny(
        MaterialPresetLibrary::kPbrWorkflowKey);
    EXPECT_EQ(Ogre::any_cast<Ogre::String>(tag),
              Ogre::String(MaterialPresetLibrary::kPbrWorkflowUnlit));
}

TEST_F(MaterialPresetLibraryTests, PbrTemplateReapplyIsIdempotent) {
    Manager::kill();
    QThread::msleep(50);
    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    ASSERT_TRUE(canLoadMeshFiles());
    createStandardOgreMaterials();

    Ogre::Entity* entity = createSelectedEntity("PbrIdem_Node", "PbrIdem_Entity", "PbrIdem_Mesh");
    ASSERT_NE(entity, nullptr);

    auto* inst = MaterialPresetLibrary::instance();
    inst->applyPreset("Metallic-Roughness");
    inst->applyPreset("Metallic-Roughness");

    auto mat = Ogre::MaterialManager::getSingleton().getByName("Preset/Metallic-Roughness");
    ASSERT_TRUE(bool(mat));
    // Re-applying the same preset must not duplicate the 6 slots —
    // the second call hits the resourceExists-true branch and skips
    // configurePbrSlots entirely.
    EXPECT_EQ(mat->getTechnique(0)->getPass(0)->getNumTextureUnitStates(), 6u);
}

// PBR slots must have semantic colour operations so they don't render as
// plain stacked texture layers. These tests check the colour-op-ex set on
// each slot at preset-apply time. Real PBR shading lands in slice F; these
// FFP approximations are slice E's "make-something-visible" implementation.
TEST_F(MaterialPresetLibraryTests, PbrSlotColourOpsApproximatePbrSemantics) {
    Manager::kill();
    QThread::msleep(50);
    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    ASSERT_TRUE(canLoadMeshFiles());
    createStandardOgreMaterials();

    Ogre::Entity* entity = createSelectedEntity(
        "PbrColourOp_Node", "PbrColourOp_Entity", "PbrColourOp_Mesh");
    ASSERT_NE(entity, nullptr);

    MaterialPresetLibrary::instance()->applyPreset("Metallic-Roughness");

    auto mat = Ogre::MaterialManager::getSingleton().getByName("Preset/Metallic-Roughness");
    ASSERT_TRUE(bool(mat));
    Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
    ASSERT_EQ(pass->getNumTextureUnitStates(), 6u);

    // Build name → TUS map so the test doesn't depend on slot ordering.
    QMap<QString, Ogre::TextureUnitState*> byName;
    for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
        auto* tus = pass->getTextureUnitState(i);
        byName.insert(QString::fromStdString(tus->getName()), tus);
    }
    ASSERT_TRUE(byName.contains("albedo"));
    ASSERT_TRUE(byName.contains("ao"));
    ASSERT_TRUE(byName.contains("emissive"));
    ASSERT_TRUE(byName.contains("metallic"));
    ASSERT_TRUE(byName.contains("roughness"));

    const auto& albedoBlend   = byName["albedo"]->getColourBlendMode();
    const auto& aoBlend       = byName["ao"]->getColourBlendMode();
    const auto& emissiveBlend = byName["emissive"]->getColourBlendMode();
    const auto& metallicBlend = byName["metallic"]->getColourBlendMode();
    const auto& roughBlend    = byName["roughness"]->getColourBlendMode();

    // albedo: textured base — modulate texture × per-vertex diffuse.
    EXPECT_EQ(albedoBlend.operation, Ogre::LBX_MODULATE);
    EXPECT_EQ(albedoBlend.source1,   Ogre::LBS_TEXTURE);
    EXPECT_EQ(albedoBlend.source2,   Ogre::LBS_DIFFUSE);

    // ao: darken by sampled value, anchored to vertex diffuse so the
    // result is independent of TUS chain position.
    EXPECT_EQ(aoBlend.operation, Ogre::LBX_MODULATE);
    EXPECT_EQ(aoBlend.source1,   Ogre::LBS_TEXTURE);
    EXPECT_EQ(aoBlend.source2,   Ogre::LBS_DIFFUSE);

    // emissive: additive on top of running colour — visible even with
    // ambient=black (the user-visible test for slice E emissive).
    EXPECT_EQ(emissiveBlend.operation, Ogre::LBX_ADD);
    EXPECT_EQ(emissiveBlend.source1,   Ogre::LBS_TEXTURE);
    EXPECT_EQ(emissiveBlend.source2,   Ogre::LBS_CURRENT);

    // metallic: ADD_SIGNED brightens the running colour where the texture
    // is bright, faking a metallic look in pure FFP.
    EXPECT_EQ(metallicBlend.operation, Ogre::LBX_ADD_SIGNED);
    EXPECT_EQ(metallicBlend.source1,   Ogre::LBS_TEXTURE);
    EXPECT_EQ(metallicBlend.source2,   Ogre::LBS_CURRENT);

    // roughness: MODULATE_X2 brightens smooth (low-roughness) regions
    // — opposite-direction approximation of glossiness, again pure FFP.
    EXPECT_EQ(roughBlend.operation, Ogre::LBX_MODULATE_X2);
    EXPECT_EQ(roughBlend.source1,   Ogre::LBS_TEXTURE);
    EXPECT_EQ(roughBlend.source2,   Ogre::LBS_CURRENT);
}

// Slice F1: Metallic-Roughness preset auto-attaches Ogre's stock
// SRS_COOK_TORRANCE_LIGHTING SubRenderState via
// RTShaderHelper::applyPbrIfTagged. Without this, the preset would
// only render via slice E's FFP approximation. We initialize RTSS
// explicitly in this test (production launches do this in
// Manager::loadResources, but the test fixture's tryInitOgre() does
// not run that path).
TEST_F(MaterialPresetLibraryTests, MetallicRoughnessPresetAttachesPbrShaderTechnique) {
    Manager::kill();
    QThread::msleep(50);
    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    ASSERT_TRUE(canLoadMeshFiles());
    createStandardOgreMaterials();

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    ASSERT_NE(sceneMgr, nullptr);
    RTShaderHelper::initialize(sceneMgr);
    auto rtssShutdown = qScopeGuard([sceneMgr] {
        RTShaderHelper::shutdown(sceneMgr);
    });

    Ogre::Entity* entity = createSelectedEntity(
        "PbrShader_Node", "PbrShader_Entity", "PbrShader_Mesh");
    ASSERT_NE(entity, nullptr);

    MaterialPresetLibrary::instance()->applyPreset("Metallic-Roughness");

    auto mat = Ogre::MaterialManager::getSingleton().getByName("Preset/Metallic-Roughness");
    ASSERT_TRUE(bool(mat));

    // The workflow tag must be set regardless of RTSS state.
    auto* pass = mat->getTechnique(0)->getPass(0);
    auto tag = pass->getUserObjectBindings().getUserAny(
        MaterialPresetLibrary::kPbrWorkflowKey);
    ASSERT_TRUE(tag.has_value());
    EXPECT_EQ(Ogre::any_cast<Ogre::String>(tag),
              Ogre::String(MaterialPresetLibrary::kPbrWorkflowMetallic));

    // applyPbrIfTagged calls createShaderBasedTechnique, which adds a
    // technique to the material under the RTSS scheme name. Count
    // techniques whose scheme matches the ShaderGenerator's scheme.
    bool hasShaderTech = false;
    const auto& shaderGenScheme =
        Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME;
    for (auto* tech : mat->getTechniques()) {
        if (tech->getSchemeName() == shaderGenScheme) {
            hasShaderTech = true;
            break;
        }
    }
    ASSERT_TRUE(hasShaderTech)
        << "Metallic-Roughness preset must produce an RTSS shader technique "
           "(via applyPbrIfTagged → createShaderBasedTechnique)";

    // Stronger assertion: the RTSS RenderState for this material must
    // carry the SRS_COOK_TORRANCE_LIGHTING SubRenderState specifically.
    // Without this, a regression that drops the Cook-Torrance SRS but
    // leaves a default-Phong RTSS technique attached would still pass
    // hasShaderTech above (false confidence).
    auto* shaderGen = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
    ASSERT_NE(shaderGen, nullptr);
    auto* renderState = shaderGen->getRenderState(
        Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME, *mat, 0);
    ASSERT_NE(renderState, nullptr) << "RTSS render state missing post-apply";
    auto* cookSRS = renderState->getSubRenderState(
        Ogre::RTShader::SRS_COOK_TORRANCE_LIGHTING);
    EXPECT_NE(cookSRS, nullptr)
        << "Cook-Torrance SubRenderState not attached — slice F1 PBR path "
           "did not run, or createSubRenderState returned null and the "
           "function silently fell through.";
}

// Non-PBR presets must NOT trigger the Cook-Torrance path — they use
// the legacy FFP rendering only. Plastic is the canonical legacy preset
// (no `pbr_workflow` tag set on its pass).
TEST_F(MaterialPresetLibraryTests, NonPbrPresetDoesNotAttachPbrShaderTechnique) {
    Manager::kill();
    QThread::msleep(50);
    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    ASSERT_TRUE(canLoadMeshFiles());
    createStandardOgreMaterials();

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    ASSERT_NE(sceneMgr, nullptr);
    RTShaderHelper::initialize(sceneMgr);
    auto rtssShutdown = qScopeGuard([sceneMgr] {
        RTShaderHelper::shutdown(sceneMgr);
    });

    Ogre::Entity* entity = createSelectedEntity(
        "NonPbr_Node", "NonPbr_Entity", "NonPbr_Mesh");
    ASSERT_NE(entity, nullptr);

    MaterialPresetLibrary::instance()->applyPreset("Plastic (Red)");

    auto mat = Ogre::MaterialManager::getSingleton().getByName("Preset/Plastic (Red)");
    ASSERT_TRUE(bool(mat));

    // The Plastic preset's pass has no `pbr_workflow` user-binding, and
    // its TUS layout doesn't match the 6-slot PBR layout either, so
    // applyPbrIfTagged must early-return without creating a shader
    // technique. Verify both signals.
    auto* pass = mat->getTechnique(0)->getPass(0);
    auto tag = pass->getUserObjectBindings().getUserAny(
        MaterialPresetLibrary::kPbrWorkflowKey);
    EXPECT_FALSE(tag.has_value())
        << "Plastic preset must not carry a pbr_workflow tag";

    // Stronger guarantee: no RTSS scheme technique attached *because of*
    // applyPreset. Future calls (e.g. handleSchemeNotFound on first render)
    // may add one for FFP shading later — that's fine; the assertion is
    // simply that applyPreset itself did not trigger a Cook-Torrance
    // technique. We check the SRS list under the ShaderGenerator scheme.
    auto* shaderGen = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
    ASSERT_NE(shaderGen, nullptr);
    if (shaderGen->hasRenderState(
            Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME)) {
        auto* renderState = shaderGen->getRenderState(
            Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME, *mat, 0);
        if (renderState) {
            auto* cookSRS = renderState->getSubRenderState(
                Ogre::RTShader::SRS_COOK_TORRANCE_LIGHTING);
            EXPECT_EQ(cookSRS, nullptr)
                << "Plastic preset unexpectedly produced a Cook-Torrance SRS — "
                   "applyPbrIfTagged should reject non-PBR materials.";
        }
    }
}

// Slice F2: when the Metallic-Roughness preset's pass has a texture in
// the normal_map slot, applyPbrIfTagged must compose SRS_NORMALMAP with
// SRS_COOK_TORRANCE_LIGHTING in the same render state. Previously the
// normal-map SRS was only added by applyNormalMap, which then reset
// the render state and dropped Cook-Torrance — so PBR + normal map
// rendered as either PBR or normal-mapped, never both. We seed the
// normal_map slot with a small in-memory texture, then assert both
// SRSes appear in the render state after applyPreset.
TEST_F(MaterialPresetLibraryTests, MetallicRoughnessPresetComposesNormalMapWithCookTorrance) {
    Manager::kill();
    QThread::msleep(50);
    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
    ASSERT_TRUE(canLoadMeshFiles());
    createStandardOgreMaterials();

    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    ASSERT_NE(sceneMgr, nullptr);
    RTShaderHelper::initialize(sceneMgr);
    auto rtssShutdown = qScopeGuard([sceneMgr] {
        RTShaderHelper::shutdown(sceneMgr);
    });

    Ogre::Entity* entity = createSelectedEntity(
        "PbrNormal_Node", "PbrNormal_Entity", "PbrNormal_Mesh");
    ASSERT_NE(entity, nullptr);

    // Create the M-R preset material first so we can inject a normal map
    // texture into its normal_map slot, then apply again so applyPbrIfTagged
    // re-runs and picks up the texture.
    MaterialPresetLibrary::instance()->applyPreset("Metallic-Roughness");

    auto mat = Ogre::MaterialManager::getSingleton().getByName("Preset/Metallic-Roughness");
    ASSERT_TRUE(bool(mat));
    auto* pass = mat->getTechnique(0)->getPass(0);

    // Create a 1x1 fallback texture so the slot has a real texture name.
    const std::string normTexName = "PbrNormal_FakeNormalTex";
    if (!Ogre::TextureManager::getSingleton().resourceExists(normTexName)) {
        Ogre::TextureManager::getSingleton().createManual(
            normTexName,
            Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
            Ogre::TEX_TYPE_2D, 1, 1, 0, Ogre::PF_BYTE_RGBA);
    }

    // Find the normal_map slot and assign the texture.
    bool found = false;
    for (unsigned short i = 0; i < pass->getNumTextureUnitStates(); ++i) {
        auto* tus = pass->getTextureUnitState(i);
        if (tus->getName() == "normal_map") {
            tus->setTextureName(normTexName);
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found) << "M-R preset is missing the normal_map slot";

    // Re-run the PBR wiring now that the normal slot has a texture.
    ASSERT_TRUE(RTShaderHelper::applyPbrIfTagged(mat));

    auto* shaderGen = Ogre::RTShader::ShaderGenerator::getSingletonPtr();
    ASSERT_NE(shaderGen, nullptr);
    auto* renderState = shaderGen->getRenderState(
        Ogre::RTShader::ShaderGenerator::DEFAULT_SCHEME_NAME, *mat, 0);
    ASSERT_NE(renderState, nullptr);

    EXPECT_NE(renderState->getSubRenderState(
                  Ogre::RTShader::SRS_COOK_TORRANCE_LIGHTING), nullptr)
        << "Cook-Torrance SRS missing — composition path dropped PBR";
    EXPECT_NE(renderState->getSubRenderState(
                  Ogre::RTShader::SRS_NORMALMAP), nullptr)
        << "SRS_NORMALMAP missing — composition path skipped the normal map";
}
