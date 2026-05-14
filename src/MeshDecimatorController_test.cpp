#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QThread>

#include "Manager.h"
#include "MeshDecimatorController.h"
#include "SelectionSet.h"
#include "TestHelpers.h"

// Most of the MeshDecimatorController is wrapped in LCOV_EXCL_START/STOP
// (the LOD-generator paths), so this suite focuses on the small portion
// that is *not* excluded: singleton lifecycle and selection-driven state.

class MeshDecimatorControllerTest : public ::testing::Test {
protected:
    void SetUp() override {
        Manager::kill();
        SelectionSet::kill();
        MeshDecimatorController::kill();
        QThread::msleep(20);
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        ASSERT_TRUE(tryInitOgre());
        ASSERT_TRUE(canLoadMeshFiles());
    }
    void TearDown() override {
        MeshDecimatorController::kill();
        SelectionSet::kill();
        Manager::kill();
        if (app) app->processEvents();
        QThread::msleep(20);
    }
    QApplication* app = nullptr;
};

TEST_F(MeshDecimatorControllerTest, InstanceReturnsSameSingleton)
{
    auto* a = MeshDecimatorController::instance();
    auto* b = MeshDecimatorController::instance();
    EXPECT_EQ(a, b);
}

TEST_F(MeshDecimatorControllerTest, NoSelectionMeansNoSelection)
{
    // Bring SelectionSet up (so the connect() inside the controller
    // resolves) but do not select anything.
    SelectionSet::getSingleton();
    auto* ctrl = MeshDecimatorController::instance();
    EXPECT_FALSE(ctrl->hasSelection());
    EXPECT_EQ(ctrl->baseTriangleCount(), 0);
    EXPECT_EQ(ctrl->previewTriangleCount(), 0);
    EXPECT_FALSE(ctrl->hasActivePreview());
}

TEST_F(MeshDecimatorControllerTest, PrimeBaselineEmitsBaseChangedEvenWithoutSelection)
{
    SelectionSet::getSingleton();
    auto* ctrl = MeshDecimatorController::instance();
    QSignalSpy spy(ctrl, &MeshDecimatorController::baseChanged);
    ctrl->primeBaseline();
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(MeshDecimatorControllerTest, ClearPreviewIsNoopWhenInactive)
{
    SelectionSet::getSingleton();
    auto* ctrl = MeshDecimatorController::instance();
    QSignalSpy spy(ctrl, &MeshDecimatorController::previewChanged);
    ctrl->clearPreview();
    // m_hasPreview is false from the constructor; clearPreview returns early.
    EXPECT_EQ(spy.count(), 0);
}
