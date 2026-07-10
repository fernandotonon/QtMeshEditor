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
    for (size_t i = 0; i < entity->getNumSubEntities(); ++i) {
        const auto& mat = entity->getSubEntity(i)->getMaterial();
        if (!mat || mat->getNumTechniques() == 0
            || mat->getTechnique(0)->getNumPasses() == 0)
            continue;
        const Ogre::Any& any = mat->getTechnique(0)->getPass(0)
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
