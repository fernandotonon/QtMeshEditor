#include <gtest/gtest.h>

#include "EditorModeController.h"
#include "EditModeController.h"

class EditorModeControllerTest : public ::testing::Test
{
protected:
    void TearDown() override
    {
        EditorModeController::kill();
        EditModeController::kill();
    }
};

TEST_F(EditorModeControllerTest, ModeNamesCoverPublicModes)
{
    auto* ctrl = EditorModeController::instance();

    EXPECT_EQ(ctrl->modeNameFor(EditorModeController::ObjectMode), QStringLiteral("Object"));
    EXPECT_EQ(ctrl->modeNameFor(EditorModeController::EditMode), QStringLiteral("Edit"));
    EXPECT_EQ(ctrl->modeNameFor(EditorModeController::AnimationMode), QStringLiteral("Animation"));
    EXPECT_EQ(ctrl->modeNameFor(EditorModeController::MaterialMode), QStringLiteral("Material"));
    EXPECT_EQ(ctrl->modeNameFor(EditorModeController::ValidationMode), QStringLiteral("Validation"));
}

TEST_F(EditorModeControllerTest, NonEditModesUpdateModeAndStatus)
{
    auto* ctrl = EditorModeController::instance();

    ctrl->requestMode(EditorModeController::AnimationMode);
    EXPECT_EQ(ctrl->currentMode(), EditorModeController::AnimationMode);
    EXPECT_EQ(ctrl->modeName(), QStringLiteral("Animation"));
    EXPECT_EQ(ctrl->statusText(), QStringLiteral("Animation mode"));

    ctrl->requestMode(EditorModeController::MaterialMode);
    EXPECT_EQ(ctrl->currentMode(), EditorModeController::MaterialMode);
    EXPECT_EQ(ctrl->statusText(), QStringLiteral("Material mode"));
}

TEST_F(EditorModeControllerTest, InvalidModeIsIgnored)
{
    auto* ctrl = EditorModeController::instance();
    ctrl->requestMode(EditorModeController::ValidationMode);

    ctrl->requestMode(-1);
    EXPECT_EQ(ctrl->currentMode(), EditorModeController::ValidationMode);

    ctrl->requestMode(99);
    EXPECT_EQ(ctrl->currentMode(), EditorModeController::ValidationMode);
}
