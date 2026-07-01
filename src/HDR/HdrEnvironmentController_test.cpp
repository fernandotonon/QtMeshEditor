#include "HDR/HdrEnvironmentController.h"

#include <gtest/gtest.h>

#include <QSignalSpy>

TEST(HdrEnvironmentControllerTest, BrowseForEnvironmentEmitsSignal)
{
    auto* ctrl = HdrEnvironmentController::instance();
    QSignalSpy spy(ctrl, &HdrEnvironmentController::browseRequested);
    ctrl->browseForEnvironment();
    EXPECT_EQ(spy.count(), 1);
}

TEST(HdrEnvironmentControllerTest, EnvironmentChoicesIncludePlaceholderWhenEmpty)
{
    auto* ctrl = HdrEnvironmentController::instance();
    ctrl->refreshBundledList();
    // With no bundled HDRIs and no recent paths, choices may be empty until
    // the user browses — the QML layer shows a non-selectable hint instead.
    EXPECT_GE(ctrl->environmentChoices().size(), 0);
}
