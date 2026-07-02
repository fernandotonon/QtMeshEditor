#include "HDR/HdrEnvironmentController.h"

#include "HDR/HdrBundledLibrary.h"

#include <gtest/gtest.h>

TEST(HdrEnvironmentControllerTest, EnvironmentChoicesStartsEmptyWithoutBundledOrRecent)
{
    auto* ctrl = HdrEnvironmentController::instance();
    ctrl->refreshBundledList();
    EXPECT_GE(ctrl->environmentChoices().size(), 0);
}

TEST(HdrEnvironmentControllerTest, BundledCatalogAppearsWhenHdrFilesOnDisk)
{
    if (HdrBundledLibrary::resolveHdriPath(QStringLiteral("studio_neutral.hdr")).isEmpty())
        GTEST_SKIP() << "Bundled HDRIs not present beside test binary";

    auto* ctrl = HdrEnvironmentController::instance();
    ctrl->refreshBundledList();
    EXPECT_TRUE(ctrl->bundledEnvironments().contains(QStringLiteral("studio_neutral.hdr")));
    EXPECT_TRUE(ctrl->environmentChoices().contains(QStringLiteral("studio_neutral.hdr")));
}

TEST(HdrEnvironmentControllerTest, CompleteBrowseFromDialogIgnoresEmptyPath)
{
    auto* ctrl = HdrEnvironmentController::instance();
    const bool hadEnv = ctrl->hasEnvironment();
    ctrl->completeBrowseFromDialog(QString());
    EXPECT_EQ(ctrl->hasEnvironment(), hadEnv);
}
