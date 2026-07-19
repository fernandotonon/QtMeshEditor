#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>
#include "Manager.h"
#include "SkeletonDebug.h"
#include "GlobalDefinitions.h"
#include <OgreException.h>
#include "TestHelpers.h"

class SkeletonDebugTests : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    std::unique_ptr<SkeletonDebug> skeletonDebug;

    void SetUp() override {
        Manager::kill();
        QThread::msleep(50);

        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();

        ASSERT_TRUE(canLoadMeshFiles()) << "mesh loading requires GL (Xvfb in CI)";

        Ogre::Entity* entity = createAnimatedTestEntity("SkeletonDebugTestEntity");
        ASSERT_NE(entity, nullptr);

        Ogre::SceneManager* sceneManager = Manager::getSingleton()->getSceneMgr();
        skeletonDebug = std::make_unique<SkeletonDebug>(entity, sceneManager);
    }

    void TearDown() override {
        skeletonDebug.reset();

        Manager::kill();

        if (app) {
            app->processEvents();
        }
        QThread::msleep(50);
    }
};

TEST_F(SkeletonDebugTests, ShowAxesTest)
{
    EXPECT_FALSE(skeletonDebug->axesShown());

    skeletonDebug->showAxes(true);
    EXPECT_TRUE(skeletonDebug->axesShown());

    skeletonDebug->showAxes(false);
    EXPECT_FALSE(skeletonDebug->axesShown());
}

TEST_F(SkeletonDebugTests, ShowNamesTest)
{
    EXPECT_FALSE(skeletonDebug->namesShown());

    skeletonDebug->showNames(true);
    EXPECT_TRUE(skeletonDebug->namesShown());

    skeletonDebug->showNames(false);
    EXPECT_FALSE(skeletonDebug->namesShown());
}

TEST_F(SkeletonDebugTests, ShowBonesTest)
{
    EXPECT_FALSE(skeletonDebug->bonesShown());

    skeletonDebug->showBones(true);
    EXPECT_TRUE(skeletonDebug->bonesShown());

    skeletonDebug->showBones(false);
    EXPECT_FALSE(skeletonDebug->bonesShown());
}

TEST_F(SkeletonDebugTests, SetAndGetAxesScaleTest)
{
    skeletonDebug->setAxesScale(0.5f);
    EXPECT_FLOAT_EQ(skeletonDebug->getAxesScale(), 0.5f);

    skeletonDebug->setAxesScale(1.0f);
    EXPECT_FLOAT_EQ(skeletonDebug->getAxesScale(), 1.0f);
}


TEST_F(SkeletonDebugTests, BoneNameForMovableReturnsEmptyForNullptr)
{
    EXPECT_TRUE(SkeletonDebug::boneNameForMovable(nullptr).empty());
}

TEST_F(SkeletonDebugTests, BoneNameForMovableReturnsEmptyForUntaggedEntity)
{
    Ogre::SceneManager* sm = Manager::getSingleton()->getSceneMgr();
    Ogre::Entity* plain = sm->createEntity(
        "BoneNameTest_Plain", Ogre::SceneManager::PT_CUBE);
    EXPECT_TRUE(SkeletonDebug::boneNameForMovable(plain).empty());
    sm->destroyEntity(plain);
}

TEST_F(SkeletonDebugTests, BoneNameForMovableReturnsBoneNameForTaggedVisuals)
{
    // After SkeletonDebug constructs visuals, every bone-mesh / axes entity
    // it creates carries the bone name as a UserAny tag and the
    // BONE_QUERY_FLAGS query mask. A ray-pick in the viewport relies on
    // both — the mask filters out scene meshes, the tag maps the hit
    // movable back to its bone.
    skeletonDebug->showBones(true);
    skeletonDebug->showAxes(true);

    Ogre::SceneManager* sm = Manager::getSingleton()->getSceneMgr();
    int taggedCount = 0;
    for (auto it = sm->getMovableObjectIterator("Entity").begin();
         it != sm->getMovableObjectIterator("Entity").end(); ++it)
    {
        auto* obj = it->second;
        Ogre::String tag = SkeletonDebug::boneNameForMovable(obj);
        if (!tag.empty())
        {
            ++taggedCount;
            EXPECT_EQ(obj->getQueryFlags(), static_cast<Ogre::uint32>(BONE_QUERY_FLAGS));
            EXPECT_EQ(SkeletonDebug::entityNameForMovable(obj),
                      Ogre::String("SkeletonDebugTestEntity"));
        }
    }
    EXPECT_GT(taggedCount, 0) << "no SkeletonDebug visuals were tagged";
}

TEST_F(SkeletonDebugTests, EntityNameForMovableReturnsEmptyForNullptr)
{
    EXPECT_TRUE(SkeletonDebug::entityNameForMovable(nullptr).empty());
}

TEST_F(SkeletonDebugTests, SetAxesScaleZero)
{
    skeletonDebug->setAxesScale(0.0f);
    EXPECT_FLOAT_EQ(skeletonDebug->getAxesScale(), 0.0f);
    skeletonDebug->showAxes(true);
    skeletonDebug->update();
}

TEST_F(SkeletonDebugTests, SetAxesScaleVeryLarge)
{
    skeletonDebug->setAxesScale(1000.0f);
    EXPECT_FLOAT_EQ(skeletonDebug->getAxesScale(), 1000.0f);
    skeletonDebug->showAxes(true);
    skeletonDebug->update();
}

TEST_F(SkeletonDebugTests, HideAllThenShowAll)
{
    skeletonDebug->showBones(false);
    skeletonDebug->showAxes(false);
    skeletonDebug->showNames(false);
    EXPECT_FALSE(skeletonDebug->bonesShown());
    EXPECT_FALSE(skeletonDebug->axesShown());
    EXPECT_FALSE(skeletonDebug->namesShown());
    skeletonDebug->showBones(true);
    skeletonDebug->showAxes(true);
    skeletonDebug->showNames(true);
    EXPECT_TRUE(skeletonDebug->bonesShown());
    EXPECT_TRUE(skeletonDebug->axesShown());
    EXPECT_TRUE(skeletonDebug->namesShown());
}

// =============================================================================
// Additional tests
// =============================================================================

TEST_F(SkeletonDebugTests, BoneMaterialCreationVerification)
{
    // After SkeletonDebug construction, the bone and axis materials should exist
    // in the Ogre MaterialManager. Verify they have expected properties.
    auto& matMgr = Ogre::MaterialManager::getSingleton();

    // The bone material should exist (created in constructor via createBoneMaterial)
    Ogre::MaterialPtr boneMat = matMgr.getByName("SkeletonDebug/BoneMat");
    ASSERT_TRUE(boneMat) << "Expected SkeletonDebug/BoneMat to exist";
    {
        EXPECT_TRUE(boneMat->isLoaded() || boneMat->isPrepared());
        // Verify the material has at least one technique and pass
        EXPECT_GT(boneMat->getNumTechniques(), 0u);
        if (boneMat->getNumTechniques() > 0) {
            Ogre::Technique* tech = boneMat->getTechnique(0);
            EXPECT_GT(tech->getNumPasses(), 0u);
            if (tech->getNumPasses() > 0) {
                Ogre::Pass* pass = tech->getPass(0);
                // Bone material has lighting disabled (vertex colour tracking)
                EXPECT_FALSE(pass->getLightingEnabled());
            }
        }
    }

    // The axis material should also exist
    Ogre::MaterialPtr axisMat = matMgr.getByName("SkeletonDebug/AxesMat");
    ASSERT_TRUE(axisMat) << "Expected SkeletonDebug/AxesMat to exist";
    EXPECT_GT(axisMat->getNumTechniques(), 0u);

    // The selected bone material should exist
    Ogre::MaterialPtr selectedMat = matMgr.getByName("SkeletonDebug/BoneMatSelected");
    ASSERT_TRUE(selectedMat) << "Expected SkeletonDebug/BoneMatSelected to exist";
    EXPECT_GT(selectedMat->getNumTechniques(), 0u);
}

TEST_F(SkeletonDebugTests, ShowHideWithBonesVisibleAndUpdate)
{
    // Show bones, update, then hide bones, update again -- tests
    // the interaction of show/hide with actual scene hierarchy updates
    skeletonDebug->showBones(true);
    skeletonDebug->showAxes(true);
    skeletonDebug->showNames(true);
    skeletonDebug->update();

    EXPECT_TRUE(skeletonDebug->bonesShown());
    EXPECT_TRUE(skeletonDebug->axesShown());
    EXPECT_TRUE(skeletonDebug->namesShown());

    // Now hide everything and update
    skeletonDebug->showBones(false);
    skeletonDebug->showAxes(false);
    skeletonDebug->showNames(false);
    skeletonDebug->update();

    EXPECT_FALSE(skeletonDebug->bonesShown());
    EXPECT_FALSE(skeletonDebug->axesShown());
    EXPECT_FALSE(skeletonDebug->namesShown());

    // Show only bones, hide rest
    skeletonDebug->showBones(true);
    skeletonDebug->update();
    EXPECT_TRUE(skeletonDebug->bonesShown());
    EXPECT_FALSE(skeletonDebug->axesShown());
    EXPECT_FALSE(skeletonDebug->namesShown());
}

TEST_F(SkeletonDebugTests, RapidToggleAxesBonesNamesStability)
{
    // Rapidly toggle all three visualization types in succession
    // to verify stability -- no crashes, no state corruption
    for (int i = 0; i < 20; ++i) {
        skeletonDebug->showAxes(i % 2 == 0);
        skeletonDebug->showBones(i % 3 == 0);
        skeletonDebug->showNames(i % 5 == 0);
        skeletonDebug->update();
    }

    // After the loop, verify state is consistent with the last iteration (i=19)
    EXPECT_FALSE(skeletonDebug->axesShown());   // 19 % 2 != 0
    EXPECT_FALSE(skeletonDebug->bonesShown());   // 19 % 3 != 0
    EXPECT_FALSE(skeletonDebug->namesShown());   // 19 % 5 != 0

    SUCCEED();
}

TEST_F(SkeletonDebugTests, SetAxesScaleExtremeValues)
{
    // Test very small scale
    skeletonDebug->setAxesScale(0.001f);
    EXPECT_FLOAT_EQ(skeletonDebug->getAxesScale(), 0.001f);
    skeletonDebug->showAxes(true);
    skeletonDebug->update();

    // Test very large scale
    skeletonDebug->setAxesScale(10000.0f);
    EXPECT_FLOAT_EQ(skeletonDebug->getAxesScale(), 10000.0f);
    skeletonDebug->showAxes(true);
    skeletonDebug->update();

    // Test negative scale (should store the value even if visually meaningless)
    skeletonDebug->setAxesScale(-1.0f);
    EXPECT_FLOAT_EQ(skeletonDebug->getAxesScale(), -1.0f);
    skeletonDebug->update();

    SUCCEED();
}
