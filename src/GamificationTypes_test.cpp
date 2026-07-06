#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "GamificationTypes.h"

using namespace Gamification;

namespace {

// Wire-shaped /v1/me/stats payload (NB: `stats`/`featureUsage` are
// snake_case, `progress`/`achievements` camelCase — matches qtmesh-cloud).
QJsonObject sampleStatsJson()
{
    const QByteArray raw = R"({
        "stats": { "xp": 820, "level": 3, "current_streak": 2,
                   "longest_streak": 5, "last_active_day": "2026-07-03",
                   "best_score": 100, "gate_green_streak": 7 },
        "progress": { "level": 3, "levelFloorXp": 400, "nextLevelXp": 900,
                      "intoLevel": 420, "span": 500, "fraction": 0.84 },
        "achievements": [
            { "key": "first_retopo", "earnedAt": 1783136400000,
              "category": "discovery", "title": "Retopologist",
              "description": "Ran quad retopology", "icon": "hexagon",
              "tier": "bronze", "xp": 25 },
            { "key": "streak_3", "earnedAt": 1783136400001,
              "category": "streak", "title": "Warming Up",
              "description": "3-day scan streak", "icon": "flame",
              "tier": "bronze", "xp": 20 }
        ],
        "featureUsage": [
            { "feature_key": "retopo", "first_used_at": 1783000000000, "use_count": 4 },
            { "feature_key": "uv_unwrap", "first_used_at": 1783000000001, "use_count": 1 }
        ],
        "counters": { "tris_optimized": 500000, "features_discovered": 2 },
        "recentlyEarned": [
            { "key": "first_retopo", "earnedAt": 1783136400000,
              "category": "discovery", "title": "Retopologist",
              "description": "Ran quad retopology", "icon": "hexagon",
              "tier": "bronze", "xp": 25 }
        ],
        "recentOperations": [
            { "op": "retopo", "at": 1783000000000,
              "metrics": { "tris_before": 42180, "tris_after": 8004 } }
        ]
    })";
    return QJsonDocument::fromJson(raw).object();
}

}  // namespace

TEST(GamificationTypes, FeatureCatalogMatchesCloudContract)
{
    const auto& catalog = featureCatalog();
    EXPECT_EQ(catalog.size(), 25);  // qtmesh-cloud DISCOVERY_FEATURES count
    for (const FeatureInfo& f : catalog) {
        EXPECT_TRUE(isValidEventKey(f.key)) << f.key.toStdString();
        EXPECT_FALSE(f.title.isEmpty());
        EXPECT_FALSE(f.blurb.isEmpty());
    }
    // Exact contract keys (cloud DISCOVERY_FEATURES) — spot-check ends.
    EXPECT_EQ(catalog.first().key, QStringLiteral("retopo"));
    EXPECT_EQ(catalog.last().key, QStringLiteral("lighting"));
    EXPECT_NE(featureInfo(QStringLiteral("mcp_server")), nullptr);
    EXPECT_NE(featureInfo(QStringLiteral("lighting")), nullptr);
    EXPECT_EQ(featureInfo(QStringLiteral("nope")), nullptr);
}

TEST(GamificationTypes, EventKeyValidation)
{
    EXPECT_TRUE(isValidEventKey(QStringLiteral("retopo")));
    EXPECT_TRUE(isValidEventKey(QStringLiteral("decimate_lod")));
    EXPECT_TRUE(isValidEventKey(QStringLiteral("a1_b2")));
    EXPECT_FALSE(isValidEventKey(QString()));
    EXPECT_FALSE(isValidEventKey(QStringLiteral("Retopo")));
    EXPECT_FALSE(isValidEventKey(QStringLiteral("has space")));
    EXPECT_FALSE(isValidEventKey(QStringLiteral("dash-ed")));
    EXPECT_FALSE(isValidEventKey(QString(65, QLatin1Char('a'))));
}

TEST(GamificationTypes, StatsSnapshotParsesWirePayload)
{
    const StatsSnapshot s = StatsSnapshot::fromJson(sampleStatsJson());
    ASSERT_TRUE(s.valid);
    EXPECT_EQ(s.xp, 820);
    EXPECT_EQ(s.level, 3);
    EXPECT_EQ(s.currentStreak, 2);
    EXPECT_EQ(s.longestStreak, 5);
    EXPECT_EQ(s.lastActiveDay, QStringLiteral("2026-07-03"));
    EXPECT_EQ(s.levelFloorXp, 400);
    EXPECT_EQ(s.nextLevelXp, 900);
    EXPECT_EQ(s.intoLevel, 420);
    EXPECT_EQ(s.span, 500);
    EXPECT_DOUBLE_EQ(s.fraction, 0.84);
    ASSERT_EQ(s.achievements.size(), 2);
    EXPECT_EQ(s.achievements.first().key, QStringLiteral("first_retopo"));
    EXPECT_EQ(s.achievements.first().xp, 25);
    EXPECT_EQ(s.achievements.first().earnedAt, 1783136400000LL);
    EXPECT_TRUE(s.hasEarned(QStringLiteral("streak_3")));
    EXPECT_FALSE(s.hasEarned(QStringLiteral("streak_7")));
    EXPECT_EQ(s.featureUseCounts.value(QStringLiteral("retopo")), 4);
    EXPECT_EQ(s.counters.value(QStringLiteral("tris_optimized")), 500000);
    ASSERT_EQ(s.recentlyEarned.size(), 1);
}

TEST(GamificationTypes, StatsSnapshotRoundTripsThroughCacheJson)
{
    const StatsSnapshot s = StatsSnapshot::fromJson(sampleStatsJson());
    const StatsSnapshot t = StatsSnapshot::fromJson(s.toJson());
    ASSERT_TRUE(t.valid);
    EXPECT_EQ(t.xp, s.xp);
    EXPECT_EQ(t.level, s.level);
    EXPECT_EQ(t.currentStreak, s.currentStreak);
    EXPECT_EQ(t.intoLevel, s.intoLevel);
    EXPECT_EQ(t.span, s.span);
    EXPECT_EQ(t.achievements.size(), s.achievements.size());
    EXPECT_EQ(t.featureUseCounts, s.featureUseCounts);
    EXPECT_EQ(t.counters, s.counters);
}

TEST(GamificationTypes, InvalidStatsJsonIsNotValid)
{
    EXPECT_FALSE(StatsSnapshot::fromJson(QJsonObject()).valid);
}

TEST(GamificationTypes, NextUnlockablesSortedByProgressAndSkipEarnedOrHidden)
{
    const StatsSnapshot s = StatsSnapshot::fromJson(sampleStatsJson());
    const QList<NextUnlockable> next = s.nextUnlockables(10);
    ASSERT_FALSE(next.isEmpty());
    // streak_3 is earned → not listed; discovery_all is hidden → not listed.
    for (const NextUnlockable& u : next) {
        EXPECT_NE(u.key, QStringLiteral("streak_3"));
        EXPECT_NE(u.key, QStringLiteral("discovery_all"));
    }
    // Sorted best-first.
    for (int i = 1; i < next.size(); ++i)
        EXPECT_GE(next.at(i - 1).fraction, next.at(i).fraction);
    // streak_7 progress = 2/7; tris 500k/1M = 0.5 should rank above it.
    EXPECT_EQ(next.first().key, QStringLiteral("optimized_1m_tris"));
    EXPECT_EQ(next.first().current, 500000);
    EXPECT_EQ(next.first().threshold, 1000000);
    // Cap honored.
    EXPECT_EQ(s.nextUnlockables(2).size(), 2);
}

TEST(GamificationTypes, UnusedFeatureKeysExcludesUsed)
{
    const StatsSnapshot s = StatsSnapshot::fromJson(sampleStatsJson());
    const QStringList unused = s.unusedFeatureKeys();
    EXPECT_EQ(unused.size(), featureCatalog().size() - 2);
    EXPECT_FALSE(unused.contains(QStringLiteral("retopo")));
    EXPECT_FALSE(unused.contains(QStringLiteral("uv_unwrap")));
    EXPECT_TRUE(unused.contains(QStringLiteral("auto_rig")));
}
