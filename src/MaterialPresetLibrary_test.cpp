#include <gtest/gtest.h>
#include "MaterialPresetLibrary.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"
#include <QApplication>
#include <QCoreApplication>
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
