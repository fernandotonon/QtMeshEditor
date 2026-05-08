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

    const QList<QPair<EditorModeController::Mode, QString>> expected = {
        {EditorModeController::ObjectMode,     QStringLiteral("Object")},
        {EditorModeController::EditMode,       QStringLiteral("Edit")},
        {EditorModeController::AnimationMode,  QStringLiteral("Animation")},
        {EditorModeController::MaterialMode,   QStringLiteral("Material")},
        {EditorModeController::ValidationMode, QStringLiteral("Validation")},
    };

    for (int i = 0; i < expected.size(); ++i) {
        const QVariantMap entry = modes.at(i).toMap();
        EXPECT_EQ(entry.value(QStringLiteral("mode")).toInt(),
                  static_cast<int>(expected[i].first))
            << "mode mismatch at index " << i;
        EXPECT_EQ(entry.value(QStringLiteral("label")).toString(),
                  expected[i].second)
            << "label mismatch at index " << i;
        // The map intentionally exposes only `mode` and `label`. `tip` was
        // dropped when ModeBar.qml stopped using tooltips.
        EXPECT_FALSE(entry.contains(QStringLiteral("tip")))
            << "tip should not be exposed at index " << i;
    }
}

TEST_F(EditorModeControllerTest, ModeTooltipsCoverAllModes)
{
    auto* ctrl = EditorModeController::instance();
    // Tooltip strings remain reachable through Q_INVOKABLE for accessibility
    // hooks even though ModeBar.qml no longer renders them.
    EXPECT_FALSE(ctrl->modeTooltipFor(EditorModeController::ObjectMode).isEmpty());
    EXPECT_FALSE(ctrl->modeTooltipFor(EditorModeController::EditMode).isEmpty());
    EXPECT_FALSE(ctrl->modeTooltipFor(EditorModeController::AnimationMode).isEmpty());
    EXPECT_FALSE(ctrl->modeTooltipFor(EditorModeController::MaterialMode).isEmpty());
    EXPECT_FALSE(ctrl->modeTooltipFor(EditorModeController::ValidationMode).isEmpty());
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
