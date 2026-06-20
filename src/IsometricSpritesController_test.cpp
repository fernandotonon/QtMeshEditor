#include <gtest/gtest.h>

#include <QSignalSpy>

#include "IsometricSpritesController.h"
#include "SelectionSet.h"

namespace {

void clearSelection()
{
    if (auto *sel = SelectionSet::getSingleton())
        sel->clear();
}

} // namespace

TEST(IsometricSpritesControllerStandalone, InstanceIsSingleton)
{
    auto *a = IsometricSpritesController::instance();
    auto *b = IsometricSpritesController::instance();
    EXPECT_EQ(a, b);
    EXPECT_NE(a, nullptr);
}

TEST(IsometricSpritesControllerStandalone, NoExportableSelectionWhenEmpty)
{
    clearSelection();
    auto *ctrl = IsometricSpritesController::instance();
    ctrl->refreshAnimations();
    EXPECT_FALSE(ctrl->hasExportableSelection());
    EXPECT_FALSE(ctrl->hasSkinnedSelection());
    EXPECT_TRUE(ctrl->availableAnimations().isEmpty());
}

TEST(IsometricSpritesControllerStandalone, ExportRefusedWithoutSelection)
{
    clearSelection();
    auto *ctrl = IsometricSpritesController::instance();
    QSignalSpy spy(ctrl, &IsometricSpritesController::exportFinished);
    const QVariantMap result = ctrl->exportSelected(
        QStringLiteral("/tmp/iso_test.png"), QString(), 8, 8, 64, 30.0, 1.25, 0.0, 0.0);
    EXPECT_FALSE(result.value(QStringLiteral("ok")).toBool());
    ASSERT_GE(spy.count(), 1);
    EXPECT_FALSE(spy.first().at(0).toBool());
}

TEST(IsometricSpritesControllerStandalone, ExportRefusedWithoutOutputPath)
{
    clearSelection();
    auto *ctrl = IsometricSpritesController::instance();
    const QVariantMap result =
        ctrl->exportSelected(QString(), QString(), 8, 1, 64, 30.0, 1.25, 0.0, 0.0);
    EXPECT_FALSE(result.value(QStringLiteral("ok")).toBool());
    EXPECT_TRUE(result.value(QStringLiteral("error")).toString().contains(QStringLiteral("Output")));
}

TEST(IsometricSpritesControllerStandalone, RequestOutputPathPickEmitsSignal)
{
    auto *ctrl = IsometricSpritesController::instance();
    QSignalSpy pickRequested(ctrl, &IsometricSpritesController::outputPathPickRequested);
    ctrl->requestOutputPathPick(QStringLiteral("/tmp/seed.png"));
    EXPECT_EQ(pickRequested.count(), 1);
    EXPECT_EQ(pickRequested.first().at(0).toString(), QStringLiteral("/tmp/seed.png"));
}
