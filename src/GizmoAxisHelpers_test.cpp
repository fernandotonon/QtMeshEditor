#include <gtest/gtest.h>

#include <utility>
#include <vector>

#include "GizmoAxisHelpers.h"
#include "Manager.h"
#include "TestHelpers.h"

TEST(GizmoAxisHelpersTest, ForEachAxisVisitsAllAxesInOrder)
{
    auto* xAxis = reinterpret_cast<Ogre::ManualObject*>(0x1);
    auto* yAxis = reinterpret_cast<Ogre::ManualObject*>(0x2);
    auto* zAxis = reinterpret_cast<Ogre::ManualObject*>(0x3);

    std::vector<Ogre::ManualObject*> visitedAxes;
    GizmoAxisHelpers::forEachAxis(xAxis, yAxis, zAxis,
                                  [&visitedAxes](Ogre::ManualObject* axis) {
                                      visitedAxes.push_back(axis);
                                  });

    ASSERT_EQ(visitedAxes.size(), 3u);
    EXPECT_EQ(visitedAxes[0], xAxis);
    EXPECT_EQ(visitedAxes[1], yAxis);
    EXPECT_EQ(visitedAxes[2], zAxis);
}

TEST(GizmoAxisHelpersTest, ForEachAxisIndexedVisitsAllAxesWithIndices)
{
    auto* xAxis = reinterpret_cast<Ogre::ManualObject*>(0x10);
    auto* yAxis = reinterpret_cast<Ogre::ManualObject*>(0x20);
    auto* zAxis = reinterpret_cast<Ogre::ManualObject*>(0x30);

    std::vector<std::pair<GizmoAxisHelpers::Axis, Ogre::ManualObject*>> visitedAxes;
    GizmoAxisHelpers::forEachAxisIndexed(xAxis, yAxis, zAxis,
                                         [&visitedAxes](GizmoAxisHelpers::Axis axis, Ogre::ManualObject* object) {
                                             visitedAxes.emplace_back(axis, object);
                                         });

    ASSERT_EQ(visitedAxes.size(), 3u);
    EXPECT_EQ(visitedAxes[0].first, GizmoAxisHelpers::Axis::X);
    EXPECT_EQ(visitedAxes[0].second, xAxis);
    EXPECT_EQ(visitedAxes[1].first, GizmoAxisHelpers::Axis::Y);
    EXPECT_EQ(visitedAxes[1].second, yAxis);
    EXPECT_EQ(visitedAxes[2].first, GizmoAxisHelpers::Axis::Z);
    EXPECT_EQ(visitedAxes[2].second, zAxis);
}

TEST(GizmoAxisHelpersTest, AxisFromObjectIdentifiesMatchingAxis)
{
    ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";

    Manager* manager = Manager::getSingletonPtr();
    ASSERT_NE(manager, nullptr);
    Ogre::SceneManager* sceneMgr = manager->getSceneMgr();
    ASSERT_NE(sceneMgr, nullptr);

    static int counter = 0;
    const Ogre::String suffix = Ogre::StringConverter::toString(counter++);
    Ogre::ManualObject* xAxis = sceneMgr->createManualObject("GizmoAxisHelpersTestX_" + suffix);
    Ogre::ManualObject* yAxis = sceneMgr->createManualObject("GizmoAxisHelpersTestY_" + suffix);
    Ogre::ManualObject* zAxis = sceneMgr->createManualObject("GizmoAxisHelpersTestZ_" + suffix);
    ASSERT_NE(xAxis, nullptr);
    ASSERT_NE(yAxis, nullptr);
    ASSERT_NE(zAxis, nullptr);

    EXPECT_EQ(GizmoAxisHelpers::axisFromObject(xAxis, xAxis, yAxis, zAxis), GizmoAxisHelpers::Axis::X);
    EXPECT_EQ(GizmoAxisHelpers::axisFromObject(yAxis, xAxis, yAxis, zAxis), GizmoAxisHelpers::Axis::Y);
    EXPECT_EQ(GizmoAxisHelpers::axisFromObject(zAxis, xAxis, yAxis, zAxis), GizmoAxisHelpers::Axis::Z);
    EXPECT_EQ(GizmoAxisHelpers::axisFromObject(nullptr, xAxis, yAxis, zAxis), GizmoAxisHelpers::Axis::None);

    sceneMgr->destroyManualObject(xAxis);
    sceneMgr->destroyManualObject(yAxis);
    sceneMgr->destroyManualObject(zAxis);
}

TEST(GizmoAxisHelpersTest, AxisToUnitVectorMapsAllAxes)
{
    EXPECT_EQ(GizmoAxisHelpers::axisToUnitVector(GizmoAxisHelpers::Axis::X), Ogre::Vector3::UNIT_X);
    EXPECT_EQ(GizmoAxisHelpers::axisToUnitVector(GizmoAxisHelpers::Axis::Y), Ogre::Vector3::UNIT_Y);
    EXPECT_EQ(GizmoAxisHelpers::axisToUnitVector(GizmoAxisHelpers::Axis::Z), Ogre::Vector3::UNIT_Z);
    EXPECT_EQ(GizmoAxisHelpers::axisToUnitVector(GizmoAxisHelpers::Axis::None), Ogre::Vector3::ZERO);
}

TEST(GizmoAxisHelpersTest, MakeAxisBoundingBoxBuildsExpectedExtentsPerAxis)
{
    const Ogre::AxisAlignedBox xBox = GizmoAxisHelpers::makeAxisBoundingBox(
        GizmoAxisHelpers::Axis::X, 0.0f, 5.0f, 2.0f);
    EXPECT_EQ(xBox.getMinimum(), Ogre::Vector3(0.0f, -2.0f, -2.0f));
    EXPECT_EQ(xBox.getMaximum(), Ogre::Vector3(5.0f, 2.0f, 2.0f));

    const Ogre::AxisAlignedBox yBox = GizmoAxisHelpers::makeAxisBoundingBox(
        GizmoAxisHelpers::Axis::Y, 1.0f, 6.0f, 3.0f);
    EXPECT_EQ(yBox.getMinimum(), Ogre::Vector3(-3.0f, 1.0f, -3.0f));
    EXPECT_EQ(yBox.getMaximum(), Ogre::Vector3(3.0f, 6.0f, 3.0f));

    const Ogre::AxisAlignedBox zBox = GizmoAxisHelpers::makeAxisBoundingBox(
        GizmoAxisHelpers::Axis::Z, -2.0f, 4.0f, 1.5f);
    EXPECT_EQ(zBox.getMinimum(), Ogre::Vector3(-1.5f, -1.5f, -2.0f));
    EXPECT_EQ(zBox.getMaximum(), Ogre::Vector3(1.5f, 1.5f, 4.0f));

    const Ogre::AxisAlignedBox noneBox = GizmoAxisHelpers::makeAxisBoundingBox(
        GizmoAxisHelpers::Axis::None, 0.0f, 1.0f, 1.0f);
    EXPECT_TRUE(noneBox.isNull());
}

TEST(GizmoAxisHelpersTest, DispatchAxisRunsOnlySelectedBranch)
{
    int invokedBranch = -1;
    GizmoAxisHelpers::dispatchAxis(
        GizmoAxisHelpers::Axis::Y,
        [&invokedBranch]() { invokedBranch = 0; },
        [&invokedBranch]() { invokedBranch = 1; },
        [&invokedBranch]() { invokedBranch = 2; },
        [&invokedBranch]() { invokedBranch = 3; });
    EXPECT_EQ(invokedBranch, 1);

    GizmoAxisHelpers::dispatchAxis(
        GizmoAxisHelpers::Axis::None,
        [&invokedBranch]() { invokedBranch = 0; },
        [&invokedBranch]() { invokedBranch = 1; },
        [&invokedBranch]() { invokedBranch = 2; },
        [&invokedBranch]() { invokedBranch = 3; });
    EXPECT_EQ(invokedBranch, 3);
}
