// Kimodo panel helpers: the prompt -> timeline segment parser and the
// variation clip naming (pure; no Ogre, no Python).

#include "KimodoController.h"
#include "MotionSceneApply.h"

#include <gtest/gtest.h>

using AIMotion::KimodoController;

TEST(KimodoController, SinglePromptUsesTheDefaultDuration)
{
    QStringList texts;
    QList<double> durations;
    QString err;
    ASSERT_TRUE(KimodoController::parsePromptSegments(QStringLiteral("  a person walks forward  "), 4.0, &texts, &durations, &err));
    ASSERT_EQ(texts.size(), 1);
    EXPECT_EQ(texts[0], QStringLiteral("a person walks forward"));
    EXPECT_DOUBLE_EQ(durations[0], 4.0);
}

TEST(KimodoController, OneSegmentPerLineWithOptionalTrailingSeconds)
{
    QStringList texts;
    QList<double> durations;
    QString err;
    ASSERT_TRUE(KimodoController::parsePromptSegments(
        QStringLiteral("walk forward @2\n\n  sit down@1.5s\nwave"), 5.0, &texts, &durations, &err))
        << err.toStdString();
    ASSERT_EQ(texts.size(), 3);
    EXPECT_EQ(texts[0], QStringLiteral("walk forward"));
    EXPECT_DOUBLE_EQ(durations[0], 2.0);
    EXPECT_EQ(texts[1], QStringLiteral("sit down"));
    EXPECT_DOUBLE_EQ(durations[1], 1.5);
    EXPECT_EQ(texts[2], QStringLiteral("wave"));
    EXPECT_DOUBLE_EQ(durations[2], 5.0);
}

TEST(KimodoController, AnAtSignInsideTheTextIsNotADuration)
{
    QStringList texts;
    QList<double> durations;
    QString err;
    ASSERT_TRUE(KimodoController::parsePromptSegments(QStringLiteral("wave @ the camera"), 3.0, &texts, &durations, &err));
    EXPECT_EQ(texts[0], QStringLiteral("wave @ the camera"));
    EXPECT_DOUBLE_EQ(durations[0], 3.0);
}

TEST(KimodoController, RejectsEmptyPromptsAndBadDurations)
{
    QStringList texts;
    QList<double> durations;
    QString err;
    EXPECT_FALSE(KimodoController::parsePromptSegments(QStringLiteral(" \n "), 5.0, &texts, &durations, &err));
    EXPECT_FALSE(err.isEmpty());
    EXPECT_FALSE(KimodoController::parsePromptSegments(QStringLiteral("@3"), 5.0, &texts, &durations, &err));
    EXPECT_FALSE(KimodoController::parsePromptSegments(QStringLiteral("walk @0"), 5.0, &texts, &durations, &err));
    EXPECT_FALSE(KimodoController::parsePromptSegments(QStringLiteral("walk @601"), 5.0, &texts, &durations, &err));
}

TEST(KimodoController, VariantClipNames)
{
    EXPECT_EQ(AIMotion::variantClipName(QStringLiteral("generated_kimodo"), 0), QStringLiteral("generated_kimodo"));
    EXPECT_EQ(AIMotion::variantClipName(QStringLiteral("generated_kimodo"), 1), QStringLiteral("generated_kimodo_v2"));
    EXPECT_EQ(AIMotion::variantClipName(QStringLiteral("walk"), 3), QStringLiteral("walk_v4"));
}

TEST(KimodoController, ApplyRefusesAMissingEntityOrNoMotion)
{
    const AIMotion::SceneApplyResult none = AIMotion::applyMotionsToEntity(nullptr, {});
    EXPECT_FALSE(none.ok);
    EXPECT_FALSE(none.error.isEmpty());
}
