#include <gtest/gtest.h>
#include <cmath>
#include <OgreCamera.h>
#include <OgreManualObject.h>
#include <OgreRay.h>
#include <QApplication>
#include <QCoreApplication>
#include <QThread>

#include "BevelGizmo.h"
#include "Manager.h"
#include "TestHelpers.h"

TEST(BevelGizmoStandalone, NullSceneManagerIsSafe)
{
    BevelGizmo gizmo(nullptr, "NullBevel");
    EXPECT_FALSE(gizmo.isVisible());
    EXPECT_FALSE(gizmo.isHandle(nullptr));
    Ogre::Ray ray(Ogre::Vector3::ZERO, Ogre::Vector3::UNIT_X);
    EXPECT_FLOAT_EQ(gizmo.distanceAlongAxis(ray), 0.0f);
    gizmo.setAxis(Ogre::Vector3(1, 2, 3), Ogre::Vector3::ZERO);
    EXPECT_TRUE(gizmo.axis().positionEquals(Ogre::Vector3::UNIT_Y, 1e-5f));
}

class BevelGizmoTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        Manager::kill();
        QThread::msleep(50);
        ASSERT_NE(qobject_cast<QApplication*>(QCoreApplication::instance()), nullptr);
        ASSERT_TRUE(tryInitOgre()) << "Ogre init failed (Xvfb/GL required in CI)";
        createStandardOgreMaterials();
    }
};

TEST_F(BevelGizmoTest, AxisVisibilityScaleAndPick)
{
    auto* sm = Manager::getSingleton()->getSceneMgr();
    ASSERT_NE(sm, nullptr);

    BevelGizmo gizmo(sm, "BevelGizmoUT");

    gizmo.setAxis(Ogre::Vector3(1, 0, 0), Ogre::Vector3(0, 0, 1));
    EXPECT_TRUE(gizmo.origin().positionEquals(Ogre::Vector3(1, 0, 0), 1e-5f));
    EXPECT_TRUE(gizmo.axis().positionEquals(Ogre::Vector3::UNIT_Z, 1e-5f));

    gizmo.setVisible(true);
    EXPECT_TRUE(gizmo.isVisible());

    Ogre::ManualObject* handle = sm->getManualObject("BevelGizmoUT_Handle_Geom");
    ASSERT_NE(handle, nullptr);
    EXPECT_TRUE(gizmo.isHandle(handle));

    gizmo.setHandleOffset(0.2f);
    gizmo.setScale(0.0f);
    EXPECT_NEAR(gizmo.origin().x, 1.0f, 1e-5f);

    auto* cam = sm->createCamera("BevelGizmoUT_Cam");
    cam->setPosition(0, 0, 10);
    cam->lookAt(Ogre::Vector3(0, 0, 0));
    gizmo.updateScreenSpaceScale(cam);
    gizmo.updateScreenSpaceScale(nullptr);

    gizmo.setVisible(false);
    EXPECT_FALSE(gizmo.isVisible());
}

TEST_F(BevelGizmoTest, DistanceAlongAxisParallelRayReturnsZero)
{
    auto* sm = Manager::getSingleton()->getSceneMgr();
    BevelGizmo gizmo(sm, "BevelGizmoUTParallel");
    gizmo.setAxis(Ogre::Vector3::ZERO, Ogre::Vector3::UNIT_Y);
    Ogre::Ray ray(Ogre::Vector3(5, 0, 0), Ogre::Vector3::UNIT_Y);
    EXPECT_NEAR(gizmo.distanceAlongAxis(ray), 0.0f, 1e-5f);

    Ogre::Ray skew(Ogre::Vector3::ZERO, Ogre::Vector3::UNIT_X);
    float t = gizmo.distanceAlongAxis(skew);
    EXPECT_TRUE(std::isfinite(t));
}
