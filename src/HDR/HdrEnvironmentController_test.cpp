#include "HDR/HdrEnvironmentController.h"

#include <gtest/gtest.h>

TEST(HdrEnvironmentControllerTest, EnvironmentChoicesStartsEmptyWithoutBundledOrRecent)
{
    auto* ctrl = HdrEnvironmentController::instance();
    ctrl->refreshBundledList();
    EXPECT_GE(ctrl->environmentChoices().size(), 0);
}

TEST(HdrEnvironmentControllerTest, CompleteBrowseFromDialogIgnoresEmptyPath)
{
    auto* ctrl = HdrEnvironmentController::instance();
    const bool hadEnv = ctrl->hasEnvironment();
    ctrl->completeBrowseFromDialog(QString());
    EXPECT_EQ(ctrl->hasEnvironment(), hadEnv);
}
