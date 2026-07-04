#include <gtest/gtest.h>

#include "LightManager.h"
#include "LightVisualizer.h"
#include "GlobalDefinitions.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "TestHelpers.h"
#include "TransformOperator.h"

#include <OgreBillboardSet.h>
#include <OgreManualObject.h>

namespace
{
float gizmoExtent(Ogre::SceneNode* lightNode)
{
    if (!lightNode)
        return 0.0f;
    for (unsigned short i = 0; i < lightNode->numChildren(); ++i)
    {
        auto* child = static_cast<Ogre::SceneNode*>(lightNode->getChild(i));
        for (unsigned short j = 0; j < child->numAttachedObjects(); ++j)
        {
            auto* obj = child->getAttachedObject(j);
            if (obj->getMovableType() == "ManualObject")
                return static_cast<Ogre::ManualObject*>(obj)->getBoundingBox().getSize().length();
        }
    }
    return 0.0f;
}
} // namespace

class LightVisualizerOgreTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        LightManager::kill();
        TransformOperator::kill();
        SelectionSet::kill();
        Manager::kill();
        if (!canLoadMeshFiles()) {
            GTEST_SKIP() << "Light visualizer tests require GL mesh loading (Xvfb in CI)";
        }
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
        LightManager::getSingleton()->tryConnectToManager();
        SelectionSet::getSingleton();
        Manager::getSingleton()->CreateEmptyScene();
    }

    void TearDown() override
    {
        SelectionSet::kill();
        LightManager::kill();
        Manager::kill();
    }
};

TEST_F(LightVisualizerOgreTest, BuildsOverlayForCreatedPointLight)
{
    LightVisualizer visualizer(Manager::getSingleton()->getSceneMgr());
    LightHandle point = LightManager::getSingleton()->createLight(Ogre::Light::LT_POINT,
                                                                  QStringLiteral("Fill"));
    ASSERT_TRUE(point.isValid());

    Ogre::SceneNode* node = point.sceneNode;
    ASSERT_NE(node, nullptr);
    bool hasIcon = false;
    bool hasGizmo = false;
    for (unsigned short i = 0; i < node->numChildren(); ++i)
    {
        auto* child = static_cast<Ogre::SceneNode*>(node->getChild(i));
        for (unsigned short j = 0; j < child->numAttachedObjects(); ++j)
        {
            Ogre::MovableObject* obj = child->getAttachedObject(j);
            if (!obj)
                continue;
            if (obj->getMovableType() == "BillboardSet")
                hasIcon = true;
            if (obj->getMovableType() == "ManualObject")
                hasGizmo = true;
            EXPECT_EQ(obj->getQueryFlags(), static_cast<Ogre::uint32>(LIGHT_QUERY_FLAGS));
            EXPECT_EQ(LightVisualizer::lightNameForMovable(obj), point.name);
        }
    }
    EXPECT_TRUE(hasIcon);
    EXPECT_TRUE(hasGizmo);
    (void)visualizer;
}

TEST_F(LightVisualizerOgreTest, HidingIconsRemovesOverlays)
{
    LightVisualizer visualizer(Manager::getSingleton()->getSceneMgr());
    LightHandle point = LightManager::getSingleton()->createLight(Ogre::Light::LT_POINT,
                                                                  QStringLiteral("Fill"));
    ASSERT_TRUE(point.isValid());

    visualizer.setIconsVisible(false);
    EXPECT_FALSE(visualizer.iconsVisible());

    for (unsigned short i = 0; i < point.sceneNode->numChildren(); ++i)
    {
        auto* child = static_cast<Ogre::SceneNode*>(point.sceneNode->getChild(i));
        for (unsigned short j = 0; j < child->numAttachedObjects(); ++j)
        {
            const Ogre::String type = child->getAttachedObject(j)->getMovableType();
            EXPECT_NE(type, "BillboardSet");
            EXPECT_NE(type, "ManualObject");
        }
    }
}

TEST_F(LightVisualizerOgreTest, SelectedGizmosOnlyHidesUnselectedHelpers)
{
    LightVisualizer visualizer(Manager::getSingleton()->getSceneMgr());
    LightHandle point = LightManager::getSingleton()->createLight(Ogre::Light::LT_POINT,
                                                                  QStringLiteral("StageFill"));
    ASSERT_TRUE(point.isValid());

    SelectionSet::getSingleton()->selectOne(point.sceneNode);
    visualizer.setSelectedGizmosOnly(true);

    bool keyGizmoVisible = false;
    bool fillGizmoVisible = false;
    for (const LightHandle& handle : LightManager::getSingleton()->lights())
    {
        for (unsigned short i = 0; i < handle.sceneNode->numChildren(); ++i)
        {
            auto* child = static_cast<Ogre::SceneNode*>(handle.sceneNode->getChild(i));
            for (unsigned short j = 0; j < child->numAttachedObjects(); ++j)
            {
                auto* obj = child->getAttachedObject(j);
                if (obj->getMovableType() != "ManualObject")
                    continue;
                if (handle.name == point.name)
                    fillGizmoVisible = obj->getVisible();
                else
                    keyGizmoVisible = obj->getVisible();
            }
        }
    }

    EXPECT_FALSE(keyGizmoVisible);
    EXPECT_TRUE(fillGizmoVisible);
}

TEST_F(LightVisualizerOgreTest, LightChangedRebuildsSpotCone)
{
    LightVisualizer visualizer(Manager::getSingleton()->getSceneMgr());
    LightHandle spot = LightManager::getSingleton()->createLight(Ogre::Light::LT_SPOTLIGHT,
                                                                QStringLiteral("Stage"));
    ASSERT_TRUE(spot.isValid());

    const float extentBefore = gizmoExtent(spot.sceneNode);
    ASSERT_GT(extentBefore, 0.0f);

    LightSnapshot snapshot = LightSnapshot::fromHandle(*LightManager::getSingleton()->findLight(spot.name));
    snapshot.spotlightInnerAngleDeg = 10.0f;
    snapshot.spotlightOuterAngleDeg = 55.0f;
    snapshot.attenuationRange = 20.0f;
    LightManager::getSingleton()->applyProperties(spot.name, snapshot);

    const float extentAfter = gizmoExtent(spot.sceneNode);
    EXPECT_GT(extentAfter, extentBefore);
    (void)visualizer;
}
