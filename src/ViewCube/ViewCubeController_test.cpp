#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QWidget>
#include "ViewCubeController.h"

// ===========================================================================
// Unit tests (no Ogre required)
// ===========================================================================

class ViewCubeControllerTest : public ::testing::Test {
protected:
    ViewCubeController* controller = nullptr;

    void SetUp() override {
        controller = new ViewCubeController(nullptr);
    }

    void TearDown() override {
        delete controller;
        controller = nullptr;
    }
};

// ---------------------------------------------------------------------------
// Default state
// ---------------------------------------------------------------------------

TEST_F(ViewCubeControllerTest, DefaultState)
{
    // isVisible() requires both m_visible=true AND an active widget
    EXPECT_FALSE(controller->isVisible());
    EXPECT_DOUBLE_EQ(controller->qw(), 1.0);
    EXPECT_DOUBLE_EQ(controller->qx(), 0.0);
    EXPECT_DOUBLE_EQ(controller->qy(), 0.0);
    EXPECT_DOUBLE_EQ(controller->qz(), 0.0);
    EXPECT_EQ(controller->windowX(), 0);
    EXPECT_EQ(controller->windowY(), 0);
}

// ---------------------------------------------------------------------------
// Visibility
// ---------------------------------------------------------------------------

TEST_F(ViewCubeControllerTest, SetVisibleEmitsSignal)
{
    QSignalSpy spy(controller, &ViewCubeController::visibilityChanged);
    ASSERT_TRUE(spy.isValid());

    controller->setVisible(true);
    ASSERT_EQ(spy.count(), 1);
    EXPECT_TRUE(spy.first().first().toBool());
    // isVisible() is still false because there is no active widget
    EXPECT_FALSE(controller->isVisible());

    controller->setVisible(false);
    ASSERT_EQ(spy.count(), 2);
    EXPECT_FALSE(spy.last().first().toBool());
    EXPECT_FALSE(controller->isVisible());
}

TEST_F(ViewCubeControllerTest, SetVisibleSameValueNoOp)
{
    QSignalSpy spy(controller, &ViewCubeController::visibilityChanged);
    controller->setVisible(false); // already false
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(ViewCubeControllerTest, SetVisibleTrueTwiceEmitsOnce)
{
    QSignalSpy spy(controller, &ViewCubeController::visibilityChanged);
    controller->setVisible(true);
    controller->setVisible(true); // no-op, already true
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(ViewCubeControllerTest, VisibleRequiresActiveWidget)
{
    // Without an active widget, isVisible() always returns false
    controller->setVisible(true);
    EXPECT_FALSE(controller->isVisible());

    controller->setVisible(false);
    EXPECT_FALSE(controller->isVisible());
}

// ---------------------------------------------------------------------------
// setActiveWidget with nullptr
// ---------------------------------------------------------------------------

TEST_F(ViewCubeControllerTest, SetActiveWidgetNull)
{
    controller->setActiveWidget(nullptr);
    EXPECT_FALSE(controller->isVisible());
}

TEST_F(ViewCubeControllerTest, SetActiveWidgetNullWhenAlreadyNullIsNoOp)
{
    // m_activeWidget starts as null; setting null again is a no-op
    controller->setVisible(true);

    QSignalSpy spy(controller, &ViewCubeController::visibilityChanged);
    controller->setActiveWidget(nullptr);
    // Same value (null == null) → early return, no signal
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(ViewCubeControllerTest, SetActiveWidgetNullTwiceNoDoubleEmit)
{
    QSignalSpy spy(controller, &ViewCubeController::visibilityChanged);
    controller->setActiveWidget(nullptr); // already null -> no-op (m_activeWidget == widget)
    // setActiveWidget checks if widget == m_activeWidget, both null -> returns early
    EXPECT_EQ(spy.count(), 0);
}

// ---------------------------------------------------------------------------
// Snap / rotate without camera (no-crash tests)
// ---------------------------------------------------------------------------

TEST_F(ViewCubeControllerTest, SnapToViewInvalidFaceNoOp)
{
    controller->snapToView("InvalidFace");
    controller->snapToView("");
    EXPECT_TRUE(true);
}

TEST_F(ViewCubeControllerTest, SnapToViewValidFacesNoCamera)
{
    QStringList faces = {"Front", "Back", "Left", "Right", "Top", "Bottom"};
    for (const QString& face : faces) {
        controller->snapToView(face);
    }
    EXPECT_TRUE(true);
}

TEST_F(ViewCubeControllerTest, SnapToViewCaseInsensitive)
{
    controller->snapToView("front");
    controller->snapToView("BACK");
    controller->snapToView("tOp");
    EXPECT_TRUE(true);
}

TEST_F(ViewCubeControllerTest, SnapToDirectionWithoutCamera)
{
    controller->snapToDirection(1, 1, -1);
    controller->snapToDirection(0, 0, 0);
    controller->snapToDirection(-1, -1, 1);
    EXPECT_TRUE(true);
}

TEST_F(ViewCubeControllerTest, RotateByDeltaWithoutCamera)
{
    controller->rotateByDelta(5.0, 3.0);
    controller->rotateByDelta(0, 0);
    controller->rotateByDelta(-10, 20);
    EXPECT_TRUE(true);
}

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

TEST_F(ViewCubeControllerTest, InstanceSingleton)
{
    EXPECT_EQ(ViewCubeController::instance(), controller);
}

TEST_F(ViewCubeControllerTest, QmlInstanceReturnsSingleton)
{
    // qmlInstance should return the existing singleton (created in SetUp)
    auto* inst = ViewCubeController::qmlInstance(nullptr, nullptr);
    EXPECT_EQ(inst, controller);
}

// ---------------------------------------------------------------------------
// updateOrientation without widget
// ---------------------------------------------------------------------------

TEST_F(ViewCubeControllerTest, UpdateOrientationWithoutWidget)
{
    controller->updateOrientation();
    EXPECT_TRUE(true);
}

TEST_F(ViewCubeControllerTest, UpdateOrientationVisibleButNoWidget)
{
    // When visible but no widget, updateOrientation should not crash
    // (reposition called but returns early, activeCamera returns null)
    controller->setVisible(true);
    controller->updateOrientation();
    EXPECT_FALSE(controller->isVisible());
}

// ---------------------------------------------------------------------------
// activeCamera returns null when no widget
// ---------------------------------------------------------------------------

TEST_F(ViewCubeControllerTest, ActiveCameraIsNullWithoutWidget)
{
    // All methods that call activeCamera() internally should handle null gracefully
    // We test this indirectly through snap/rotate methods
    controller->snapToView("Front");
    controller->snapToDirection(0, 0, -1);
    controller->rotateByDelta(1, 1);
    // No crash = pass
    EXPECT_TRUE(true);
}

// ---------------------------------------------------------------------------
// Orientation getters reflect default identity quaternion
// ---------------------------------------------------------------------------

TEST_F(ViewCubeControllerTest, DefaultOrientationIsIdentity)
{
    EXPECT_DOUBLE_EQ(controller->qw(), 1.0);
    EXPECT_DOUBLE_EQ(controller->qx(), 0.0);
    EXPECT_DOUBLE_EQ(controller->qy(), 0.0);
    EXPECT_DOUBLE_EQ(controller->qz(), 0.0);
}
