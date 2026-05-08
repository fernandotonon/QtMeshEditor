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

TEST_F(EditorModeControllerTest, AvailableModesExposeModeBarMetadata)
{
    auto* ctrl = EditorModeController::instance();
    const QVariantList modes = ctrl->availableModes();

    ASSERT_EQ(modes.size(), 5);
    EXPECT_EQ(modes.at(0).toMap().value(QStringLiteral("mode")).toInt(),
              EditorModeController::ObjectMode);
    EXPECT_EQ(modes.at(0).toMap().value(QStringLiteral("label")).toString(),
              QStringLiteral("Object"));
    EXPECT_EQ(modes.at(0).toMap().value(QStringLiteral("tip")).toString(),
              ctrl->modeTooltipFor(EditorModeController::ObjectMode));

    EXPECT_EQ(modes.at(1).toMap().value(QStringLiteral("mode")).toInt(),
              EditorModeController::EditMode);
    EXPECT_EQ(modes.at(2).toMap().value(QStringLiteral("mode")).toInt(),
              EditorModeController::AnimationMode);
    EXPECT_EQ(modes.at(3).toMap().value(QStringLiteral("mode")).toInt(),
              EditorModeController::MaterialMode);
    EXPECT_EQ(modes.at(4).toMap().value(QStringLiteral("mode")).toInt(),
              EditorModeController::ValidationMode);
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

TEST_F(EditorModeControllerTest, ObjectModeRequestUpdatesStatusText)
{
    auto* ctrl = EditorModeController::instance();

    // Hop away first so the request actually transitions back to ObjectMode.
    ctrl->requestMode(EditorModeController::AnimationMode);
    ASSERT_EQ(ctrl->currentMode(), EditorModeController::AnimationMode);

    ctrl->requestMode(EditorModeController::ObjectMode);
    EXPECT_EQ(ctrl->currentMode(), EditorModeController::ObjectMode);
    EXPECT_EQ(ctrl->modeName(), QStringLiteral("Object"));
    EXPECT_EQ(ctrl->statusText(), QStringLiteral("Object mode"));
}

TEST_F(EditorModeControllerTest, EditModeRequestWithoutMeshFallsBackToHint)
{
    auto* ctrl = EditorModeController::instance();
    auto* edit = EditModeController::instance();
    ASSERT_FALSE(edit->isEditModeActive());
    ASSERT_FALSE(edit->canEnterEditMode());

    // No selection → enterEditMode() refuses, so EditorModeController stays
    // in ObjectMode and surfaces a hint via statusText().
    ctrl->requestMode(EditorModeController::EditMode);
    EXPECT_EQ(ctrl->currentMode(), EditorModeController::ObjectMode);
    EXPECT_EQ(ctrl->statusText(), QStringLiteral("Select one mesh to enter Edit mode"));
    EXPECT_FALSE(edit->isEditModeActive());
}
