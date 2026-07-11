#include "SkinningDisplay.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSubEntity.h>
#include <OgreRTShaderSystem.h>

#include <string>
#include <vector>

#include "Manager.h"
#include "RTShaderHelper.h"
#include "TestHelpers.h"

// Slice D (#819): dual-quaternion skinning display toggle. Uses a
// live skinned entity — needs the Ogre + RTSS init the Manager
// singleton performs (Linux CI; macOS local runs skip at the
// test_main gate like every other Ogre-backed suite).

namespace {

std::string uniqueName(const char* prefix)
{
    static int counter = 0;
    return std::string(prefix) + "_sd_" + std::to_string(counter++);
}

// The user-binding key Ogre's HardwareSkinningFactory imprints
// under (mirrors SkinningDisplay.cpp).
const char* kOgreHsDataKey = "HS_SRS_DATA";

bool materialHasImprint(Ogre::Entity* entity)
{
    // Ogre imprints on Technique(0)'s bindings (not a pass) —
    // verified against v14.5.2 imprintSkeletonData.
    for (size_t i = 0; i < entity->getNumSubEntities(); ++i) {
        const auto& mat = entity->getSubEntity(i)->getMaterial();
        if (!mat || mat->getNumTechniques() == 0)
            continue;
        const Ogre::Any& any = mat->getTechnique(0)
            ->getUserObjectBindings().getUserAny(kOgreHsDataKey);
        if (any.has_value()) return true;
    }
    return false;
}

} // namespace

class SkinningDisplayTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_NE(qobject_cast<QApplication*>(QCoreApplication::instance()),
                  nullptr);
        ASSERT_TRUE(tryInitOgre())
            << "Ogre init failed — invalid CI/runtime environment";
        ASSERT_TRUE(canLoadMeshFiles()) << "no GL context";
        createStandardOgreMaterials();
        // The test harness never runs Manager::loadResources() (the
        // GUI/CLI path that initializes RTSS), so bring the shader
        // generator + HS factory up explicitly — the same pattern
        // MaterialPresetLibrary_test uses. Safe to repeat: it
        // early-returns once the generator exists.
        RTShaderHelper::initialize(Manager::getSingleton()->getSceneMgr());
    }
};

TEST_F(SkinningDisplayTest, GuardsRejectInvalidEntities)
{
    QString err;
    EXPECT_FALSE(SkinningDisplay::apply(nullptr,
        SkinningDisplay::Mode::DualQuaternion, &err));
    EXPECT_FALSE(err.isEmpty());

    // Skeleton-less entity → error mentioning the skeleton.
    Ogre::MeshPtr mesh = createInMemoryTriangleMesh(uniqueName("static"));
    ASSERT_TRUE(mesh);
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    Ogre::Entity* ent = sceneMgr->createEntity(uniqueName("static_ent"), mesh);
    err.clear();
    EXPECT_FALSE(SkinningDisplay::apply(ent,
        SkinningDisplay::Mode::DualQuaternion, &err));
    EXPECT_TRUE(err.contains(QStringLiteral("skeleton")))
        << err.toStdString();
    sceneMgr->destroyEntity(ent);
}

TEST_F(SkinningDisplayTest, DqsAppliesAndLinearReverts)
{
    Ogre::Entity* ent = createAnimatedTestEntity(uniqueName("hero"));
    ASSERT_NE(ent, nullptr);

    // RTSS must be up (Manager initializes it) — the factory is
    // registered by RTShaderHelper::initialize.
    ASSERT_NE(Ogre::RTShader::ShaderGenerator::getSingletonPtr(), nullptr);
    ASSERT_NE(Ogre::RTShader::HardwareSkinningFactory::getSingletonPtr(),
              nullptr);

    // Default state.
    EXPECT_EQ(SkinningDisplay::current(ent), SkinningDisplay::Mode::Linear);
    EXPECT_FALSE(materialHasImprint(ent));

    // DQS: imprint lands on the material, mode tracked on the entity.
    QString err;
    ASSERT_TRUE(SkinningDisplay::apply(
        ent, SkinningDisplay::Mode::DualQuaternion, &err))
        << err.toStdString();
    EXPECT_EQ(SkinningDisplay::current(ent),
              SkinningDisplay::Mode::DualQuaternion);
    EXPECT_TRUE(materialHasImprint(ent))
        << "prepareEntityForSkinning left no HS_SRS_DATA imprint";

    // Back to Linear: imprint erased.
    ASSERT_TRUE(SkinningDisplay::apply(
        ent, SkinningDisplay::Mode::Linear, &err)) << err.toStdString();
    EXPECT_EQ(SkinningDisplay::current(ent), SkinningDisplay::Mode::Linear);
    EXPECT_FALSE(materialHasImprint(ent))
        << "Linear mode must erase the HS imprint";
}

TEST_F(SkinningDisplayTest, SharedMaterialEntitiesTrackTheSameMode)
{
    // The RTSS imprint is material-level: a second entity sharing
    // the material switches with the first, so its tracked mode
    // must be stamped too (Codex P2 on PR #830).
    Ogre::Entity* a = createAnimatedTestEntity(uniqueName("shared_a"));
    ASSERT_NE(a, nullptr);
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    Ogre::Entity* b =
        sceneMgr->createEntity(uniqueName("shared_b"), a->getMesh());
    ASSERT_NE(b, nullptr);

    QString err;
    ASSERT_TRUE(SkinningDisplay::apply(
        a, SkinningDisplay::Mode::DualQuaternion, &err))
        << err.toStdString();
    EXPECT_EQ(SkinningDisplay::current(b),
              SkinningDisplay::Mode::DualQuaternion)
        << "entity sharing the imprinted material reports a stale mode";

    ASSERT_TRUE(SkinningDisplay::apply(a, SkinningDisplay::Mode::Linear,
                                       &err)) << err.toStdString();
    EXPECT_EQ(SkinningDisplay::current(b), SkinningDisplay::Mode::Linear);

    sceneMgr->destroyEntity(b);
}

TEST_F(SkinningDisplayTest, ModeStringRoundTrip)
{
    EXPECT_EQ(SkinningDisplay::modeToString(SkinningDisplay::Mode::Linear),
              QStringLiteral("linear"));
    EXPECT_EQ(SkinningDisplay::modeToString(
                  SkinningDisplay::Mode::DualQuaternion),
              QStringLiteral("dual-quaternion"));
    EXPECT_EQ(SkinningDisplay::modeFromString("dual-quaternion"),
              SkinningDisplay::Mode::DualQuaternion);
    EXPECT_EQ(SkinningDisplay::modeFromString("dqs"),
              SkinningDisplay::Mode::DualQuaternion);
    EXPECT_EQ(SkinningDisplay::modeFromString("linear"),
              SkinningDisplay::Mode::Linear);
    EXPECT_EQ(SkinningDisplay::modeFromString("whatever"),
              SkinningDisplay::Mode::Linear);
    // Null entity reads as Linear.
    EXPECT_EQ(SkinningDisplay::current(nullptr),
              SkinningDisplay::Mode::Linear);
}

// Regression for issue #833: Ogre caches the per-scheme
// hardware-animation decision per entity (Entity::mSchemeHardwareAnim)
// the first time it renders. Toggling DQS regenerates the technique
// with a skeletal-animation vertex program, but without a cache
// re-evaluation the entity keeps SOFTWARE-skinning and binds
// blend-info-stripped buffers — the shader reads all-zero blend
// weights and every vertex collapses to the origin (the mesh
// disappears, silently). SkinningDisplay::apply must force the
// re-evaluation, so the entity must still produce pixels after the
// toggle in BOTH directions.
TEST_F(SkinningDisplayTest, EntityStillRendersAfterDqsToggle)
{
    auto* sceneMgr = Manager::getSingleton()->getSceneMgr();
    Ogre::Entity* ent = createAnimatedTestEntity(uniqueName("dqs_px"));
    ASSERT_NE(ent, nullptr);

    sceneMgr->setAmbientLight(Ogre::ColourValue::White);

    const std::string camName = uniqueName("dqs_px_cam");
    Ogre::Camera* cam = sceneMgr->createCamera(camName);
    cam->setNearClipDistance(0.05f);
    auto* camNode = sceneMgr->getRootSceneNode()->createChildSceneNode();
    camNode->attachObject(cam);
    camNode->setPosition(0.4f, 0.4f, 3.0f);
    camNode->lookAt(Ogre::Vector3(0.4f, 0.4f, 0.0f), Ogre::Node::TS_WORLD);

    const int W = 64, H = 64;
    Ogre::TexturePtr rtt = Ogre::TextureManager::getSingleton().createManual(
        uniqueName("dqs_px_rtt"),
        Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME,
        Ogre::TEX_TYPE_2D, W, H, 0, Ogre::PF_BYTE_RGBA,
        Ogre::TU_RENDERTARGET);
    auto* target = rtt->getBuffer()->getRenderTarget();
    auto* vp = target->addViewport(cam);
    vp->setBackgroundColour(Ogre::ColourValue::Black);
    vp->setOverlaysEnabled(false);
    // Render through the RTSS scheme — the DQS technique lives there.
    vp->setMaterialScheme(Ogre::MSN_SHADERGEN);

    std::vector<Ogre::uchar> pixels(static_cast<size_t>(W) * H * 4);
    const auto litPixels = [&]() -> int {
        target->update();
        Ogre::PixelBox pb(W, H, 1, Ogre::PF_BYTE_RGBA, pixels.data());
        target->copyContentsToMemory(Ogre::Box(0, 0, W, H), pb);
        int lit = 0;
        for (size_t i = 0; i < pixels.size(); i += 4)
            if (pixels[i] > 16 || pixels[i + 1] > 16 || pixels[i + 2] > 16)
                ++lit;
        return lit;
    };

    // First render in Linear populates Ogre's cached decision.
    const int litLinear = litPixels();
    ASSERT_GT(litLinear, 0) << "fixture entity renders nothing in Linear "
                               "— test setup problem, not a DQS issue";

    QString err;
    ASSERT_TRUE(SkinningDisplay::apply(
        ent, SkinningDisplay::Mode::DualQuaternion, &err))
        << err.toStdString();
    EXPECT_GT(litPixels(), 0)
        << "entity vanished after switching to dual-quaternion (#833)";

    ASSERT_TRUE(SkinningDisplay::apply(ent, SkinningDisplay::Mode::Linear,
                                       &err)) << err.toStdString();
    EXPECT_GT(litPixels(), 0)
        << "entity vanished after switching back to linear";

    target->removeAllViewports();
    sceneMgr->destroyCamera(cam);
    sceneMgr->destroySceneNode(camNode);
    Ogre::TextureManager::getSingleton().remove(rtt);
}
