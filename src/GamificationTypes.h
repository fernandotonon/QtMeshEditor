#ifndef GAMIFICATION_TYPES_H
#define GAMIFICATION_TYPES_H

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

/// Pure-data types for the QtMesh Cloud gamification contract (#796).
/// Mirrors the cloud side (qtmesh-cloud `src/gamification.ts`): the 24
/// discovery feature-cluster keys, the client-visible milestone catalog used
/// to compute "nearest unlockable" locally, and the parsed `GET /v1/me/stats`
/// snapshot. No Ogre / no network — unit-testable.
namespace Gamification {

/// One editor feature cluster (exact `feature` key for POST /v1/events/editor).
struct FeatureInfo {
    QString key;    ///< server key, [a-z0-9_]+ (e.g. "retopo")
    QString title;  ///< editor-facing name for nudges ("Quad Retopology")
    QString blurb;  ///< one-line "try this" description
};

/// All 24 discovery clusters, in the cloud catalog order.
const QList<FeatureInfo>& featureCatalog();

/// Catalog entry for @p key, or nullptr when unknown.
const FeatureInfo* featureInfo(const QString& key);

/// True when @p key is a valid server-side feature/op key ([a-z0-9_]+, <=64).
bool isValidEventKey(const QString& key);

/// An achievement as serialized by the cloud (`serializeAchievement`).
struct Achievement {
    QString key;
    QString category;  ///< streak | quality | discovery | milestone
    QString title;
    QString description;
    QString icon;
    QString tier;      ///< bronze | silver | gold | platinum
    int xp = 0;
    bool hidden = false;
    qint64 earnedAt = 0;  ///< epoch ms; 0 when not earned / not present

    static Achievement fromJson(const QJsonObject& o);
    QVariantMap toVariantMap() const;
};

/// Client-side mirror of a counter-driven (or streak) milestone so the editor
/// can compute progress toward locked achievements without a catalog endpoint.
struct MilestoneInfo {
    QString key;
    QString title;
    QString description;
    QString counter;   ///< user counter name, or "current_streak" for streaks
    qint64 threshold = 0;
    bool hidden = false;
};

/// Non-discovery milestones the editor knows how to show progress for.
const QList<MilestoneInfo>& milestoneCatalog();

/// A locked achievement with locally computed progress (for E-P4).
struct NextUnlockable {
    QString key;
    QString title;
    QString description;
    qint64 current = 0;
    qint64 threshold = 0;
    double fraction = 0.0;  ///< 0..1

    QVariantMap toVariantMap() const;
};

/// Parsed `GET /v1/me/stats` payload. NB the wire mixes snake_case (`stats`,
/// `featureUsage`) and camelCase (`progress`, `achievements`) — this struct
/// normalizes all of it.
struct StatsSnapshot {
    bool valid = false;

    // stats (snake_case on the wire)
    qint64 xp = 0;
    int level = 1;
    int currentStreak = 0;
    int longestStreak = 0;
    QString lastActiveDay;  ///< "YYYY-MM-DD" UTC or empty

    // progress (camelCase on the wire)
    qint64 levelFloorXp = 0;
    qint64 nextLevelXp = 100;
    qint64 intoLevel = 0;
    qint64 span = 100;
    double fraction = 0.0;

    QList<Achievement> achievements;   ///< earned, includes hidden
    QList<Achievement> recentlyEarned; ///< first 5
    QHash<QString, qint64> counters;   ///< counter name -> total
    QHash<QString, int> featureUseCounts;  ///< feature_key -> use_count

    static StatsSnapshot fromJson(const QJsonObject& root);
    /// Round-trip for the on-disk cache (offline last-known rendering).
    QJsonObject toJson() const;

    bool hasEarned(const QString& achievementKey) const;

    /// Locked, non-hidden milestones with progress, best-first (E-P4).
    QList<NextUnlockable> nextUnlockables(int maxCount = 2) const;

    /// Feature cluster keys the user has never used (E-P5 nudge candidates),
    /// in catalog order.
    QStringList unusedFeatureKeys() const;
};

}  // namespace Gamification

#endif  // GAMIFICATION_TYPES_H
