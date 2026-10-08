// Pure-data tests for the shared game-ready preset table (GUI / CLI / MCP
// all read it, so a wrong number here is wrong everywhere).

#include <gtest/gtest.h>

#include <QSet>

#include "GameReadyPresets.h"

TEST(GameReadyPresets, IdsAreUniqueLowercaseAndOrderedByBudget)
{
    QSet<QString> seen;
    int prevTris = -1;
    for (const auto& p : GameReady::presets()) {
        EXPECT_FALSE(seen.contains(p.id)) << p.id.toStdString();
        seen.insert(p.id);
        EXPECT_EQ(p.id, p.id.toLower());
        EXPECT_FALSE(p.label.isEmpty());
        // "max" (0) leads; every other entry is ascending so the picker
        // reads lightest-to-heaviest.
        if (p.id != QLatin1String("max")) {
            EXPECT_GT(p.targetTriangles, prevTris) << p.id.toStdString();
            prevTris = p.targetTriangles;
        }
    }
    EXPECT_EQ(GameReady::presets().front().id, QStringLiteral("max"));
    EXPECT_EQ(GameReady::ids().size(), static_cast<int>(GameReady::presets().size()));
}

TEST(GameReadyPresets, RobloxPresetsCarryTheUploadLimits)
{
    const auto* acc = GameReady::find(QStringLiteral("roblox-accessory"));
    ASSERT_NE(acc, nullptr);
    EXPECT_EQ(acc->targetTriangles, 4000);
    EXPECT_TRUE(acc->strictTriangles);
    EXPECT_EQ(acc->maxTextureSize, 1024);

    const auto* part = GameReady::find(QStringLiteral("roblox-meshpart"));
    ASSERT_NE(part, nullptr);
    EXPECT_EQ(part->targetTriangles, 20000);
    EXPECT_TRUE(part->strictTriangles);
    EXPECT_EQ(part->maxTextureSize, 1024);
}

TEST(GameReadyPresets, NonPlatformPresetsAreSoftBudgetsWithoutTextureCaps)
{
    for (const auto& p : GameReady::presets()) {
        if (p.id.startsWith(QLatin1String("roblox")))
            continue;
        EXPECT_FALSE(p.strictTriangles) << p.id.toStdString();
        EXPECT_EQ(p.maxTextureSize, 0) << p.id.toStdString();
    }
    const auto* med = GameReady::find(GameReady::defaultId());
    ASSERT_NE(med, nullptr);
    EXPECT_EQ(med->targetTriangles, 25000);
    const auto* max = GameReady::find(QStringLiteral("max"));
    ASSERT_NE(max, nullptr);
    EXPECT_EQ(max->targetTriangles, 0);
}

TEST(GameReadyPresets, FindIsForgivingAboutSpellingButRejectsUnknown)
{
    EXPECT_EQ(GameReady::find(QStringLiteral("ROBLOX_MESHPART"))->id,
              QStringLiteral("roblox-meshpart"));
    EXPECT_EQ(GameReady::find(QStringLiteral(" roblox "))->id,
              QStringLiteral("roblox-meshpart"));
    EXPECT_EQ(GameReady::find(QStringLiteral("Roblox Mesh Part"))->id,
              QStringLiteral("roblox-meshpart"));
    EXPECT_EQ(GameReady::find(QStringLiteral("accessory"))->id,
              QStringLiteral("roblox-accessory"));
    EXPECT_EQ(GameReady::find(QStringLiteral("original"))->id, QStringLiteral("max"));
    EXPECT_EQ(GameReady::find(QStringLiteral("ultra")), nullptr);
    EXPECT_EQ(GameReady::find(QString()), nullptr);
}

TEST(GameReadyPresets, IndexOfMatchesTableOrder)
{
    const auto& all = GameReady::presets();
    for (size_t i = 0; i < all.size(); ++i)
        EXPECT_EQ(GameReady::indexOf(all[i].id), static_cast<int>(i));
    EXPECT_EQ(GameReady::indexOf(QStringLiteral("nope")), -1);
    EXPECT_GE(GameReady::indexOf(GameReady::defaultId()), 0);
}
