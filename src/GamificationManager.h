#ifndef GAMIFICATION_MANAGER_H
#define GAMIFICATION_MANAGER_H

#include "GamificationEventQueue.h"
#include "GamificationTypes.h"

#include <QObject>
#include <QQmlEngine>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class QTimer;

/// Central gamification orchestrator (#796): consent + privacy gating (E-P6),
/// the offline event queue and cloud flush loop (E-P1), the `noteFeature` /
/// `noteOperation` instrumentation entry points (E-P2/E-P3), cached
/// `/v1/me/stats` for the in-app status surface (E-P4), and the welcome-screen
/// discovery suggestion (E-P5).
///
/// Privacy invariants (epic guardrails):
///  - Nothing is queued before the user acknowledges the consent prompt or
///    enables "Sync my QtMesh progress" in Preferences (default OFF).
///  - Zero network activity when logged out or opted out; events queued while
///    offline are held locally and flushed once authenticated + online.
///  - Only feature keys, timestamps and numeric metrics are ever sent —
///    never asset content or file names.
class GamificationManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool signedIn READ signedIn NOTIFY sessionChanged)
    Q_PROPERTY(bool consentAcknowledged READ consentAcknowledged NOTIFY prefsChanged)
    Q_PROPERTY(bool syncEnabled READ syncEnabled WRITE setSyncEnabled NOTIFY prefsChanged)
    Q_PROPERTY(bool usageEnabled READ usageEnabled WRITE setUsageEnabled NOTIFY prefsChanged)
    Q_PROPERTY(bool opsEnabled READ opsEnabled WRITE setOpsEnabled NOTIFY prefsChanged)
    Q_PROPERTY(bool nudgesEnabled READ nudgesEnabled WRITE setNudgesEnabled NOTIFY prefsChanged)
    Q_PROPERTY(bool statsAvailable READ statsAvailable NOTIFY statsChanged)
    Q_PROPERTY(int level READ level NOTIFY statsChanged)
    Q_PROPERTY(int xp READ xp NOTIFY statsChanged)
    Q_PROPERTY(int xpIntoLevel READ xpIntoLevel NOTIFY statsChanged)
    Q_PROPERTY(int xpSpan READ xpSpan NOTIFY statsChanged)
    Q_PROPERTY(double xpFraction READ xpFraction NOTIFY statsChanged)
    Q_PROPERTY(int currentStreak READ currentStreak NOTIFY statsChanged)
    Q_PROPERTY(int achievementsEarned READ achievementsEarned NOTIFY statsChanged)
    Q_PROPERTY(QVariantList nextUnlockables READ nextUnlockables NOTIFY statsChanged)
    Q_PROPERTY(QVariantMap suggestion READ suggestion NOTIFY suggestionChanged)
    Q_PROPERTY(QString profileUrl READ profileUrl NOTIFY sessionChanged)
    Q_PROPERTY(bool profilePublic READ profilePublic NOTIFY cloudPrefsChanged)

public:
    enum class Surface { Gui, Cli, Mcp };

    static GamificationManager* instance();
    static GamificationManager* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    // ---- Instrumentation entry points (one-liners at controller entry) ----

    /// Records use of a feature cluster (E-P2). Thread-safe; no-op when the
    /// user is opted out / has not consented. Deduped per session.
    static void noteFeature(const QString& featureKey, Surface surface = Surface::Gui);

    /// Records a completed operation with content-free numeric metrics
    /// (E-P3), e.g. noteOperation("retopo", {{"tris_before", 42180},
    /// {"tris_after", 8004}}). Also notes the matching feature cluster.
    static void noteOperation(const QString& opType, const QVariantMap& metrics,
                              Surface surface = Surface::Gui);

    /// Cloud project context attached to subsequent operations (both empty to
    /// clear). Set while browsing/uploading a cloud project.
    static void setProjectContext(const QString& ownerSlug, const QString& projectSlug);

    /// Process-wide kill switch: while suspended, noteFeature/noteOperation
    /// are no-ops at every call site (CLI --no-telemetry sets this before
    /// dispatching the subcommand).
    static void setEmissionSuspended(bool suspended);
    static bool emissionSuspended();

    // ---- Flush / stats ----

    /// Async flush of the pending queue (no-op logged-out/opted-out/backoff).
    Q_INVOKABLE void flushNow();

    /// Synchronous flush for one-shot CLI processes; returns events flushed.
    int flushBlocking(int timeoutMs = 5000);

    Q_INVOKABLE void refreshStats();
    void refreshStatsIfStale(qint64 maxAgeMs = 5 * 60 * 1000);

    /// Called by MainWindow after sign-in/out so the surface updates.
    void handleSessionChanged();

    const Gamification::StatsSnapshot& snapshot() const { return m_snapshot; }
    int pendingEventCount() const { return m_queue.size(); }

    // ---- Prefs / consent (E-P6) ----
    bool signedIn() const;
    bool consentAcknowledged() const;
    bool syncEnabled() const;
    bool usageEnabled() const;
    bool opsEnabled() const;
    bool nudgesEnabled() const;
    void setSyncEnabled(bool enabled);
    void setUsageEnabled(bool enabled);
    void setOpsEnabled(bool enabled);
    void setNudgesEnabled(bool enabled);

    /// True when usage events may be recorded right now.
    bool usageEmissionActive() const;
    /// True when operation events may be recorded right now.
    bool opsEmissionActive() const;

    Q_INVOKABLE void acceptConsent();
    Q_INVOKABLE void declineConsent();

    /// Example event payloads for the "What is shared?" preferences section.
    Q_INVOKABLE QString examplePayload() const;

    /// DELETE /v1/me/gamification + clears the local queue and cached stats.
    Q_INVOKABLE void deleteCloudData();

    /// PUT profilePublic to the account (fetches current prefs, merges).
    Q_INVOKABLE void setProfilePublic(bool isPublic);
    Q_INVOKABLE void refreshCloudPrefs();
    bool profilePublic() const { return m_profilePublic; }

    // ---- Status surface (E-P4) ----
    bool statsAvailable() const { return m_snapshot.valid; }
    int level() const { return m_snapshot.level; }
    int xp() const { return static_cast<int>(m_snapshot.xp); }
    int xpIntoLevel() const { return static_cast<int>(m_snapshot.intoLevel); }
    int xpSpan() const { return static_cast<int>(m_snapshot.span); }
    double xpFraction() const { return m_snapshot.fraction; }
    int currentStreak() const { return m_snapshot.currentStreak; }
    int achievementsEarned() const { return m_snapshot.achievements.size(); }
    QVariantList nextUnlockables() const;
    QString profileUrl() const;
    Q_INVOKABLE void openProfile();

    // ---- Discovery suggestion (E-P5) ----
    QVariantMap suggestion() const;
    Q_INVOKABLE void dismissSuggestion(const QString& featureKey);
    Q_INVOKABLE void advanceSuggestion();

signals:
    void prefsChanged();
    void sessionChanged();
    void statsChanged();
    void suggestionChanged();
    void cloudPrefsChanged();
    /// Newly-earned achievements from a flush — list of maps with
    /// key/title/description/tier/xp (E-P4 toast; coalesced, max 1 toast).
    void achievementsUnlocked(const QVariantList& achievements);
    /// One-time non-blocking consent prompt should be shown (E-P6).
    void consentPromptRequested();
    void deleteCloudDataFinished(bool ok, const QString& error);

private:
    explicit GamificationManager(QObject* parent = nullptr);
    ~GamificationManager() override;

    void noteFeatureInternal(const QString& featureKey, Surface surface);
    void noteOperationInternal(const QString& opType, const QVariantMap& metrics,
                               Surface surface);
    bool maybeRequestConsent();
    void scheduleFlushSoon();
    /// Owner slug for events queued right now (empty when logged out).
    QString currentEventOwner() const;
    /// Snapshot a flushable batch of @p kind for the current account, and
    /// collect (into @p dropIds) queued ids that belong to a DIFFERENT
    /// account — those are never sent and get dropped.
    QList<GamificationEventQueue::Entry> flushableBatch(const QString& kind,
                                                        QStringList* dropIds) const;
    /// Runs the server-side purge; local queue/cache were already cleared.
    void performCloudDelete(const QString& token);

    struct FlushOutcome {
        bool attempted = false;
        bool ok = false;
        int accepted = 0;
        QStringList ackedIds;
        QVariantList newAchievements;
        QString error;
    };
    /// Blocking network flush of pre-snapshotted batches (safe to run on a
    /// worker thread — does not touch the queue or any member state).
    static FlushOutcome performFlush(const QString& token,
                                     const QList<GamificationEventQueue::Entry>& featureBatch,
                                     const QList<GamificationEventQueue::Entry>& operationBatch,
                                     int timeoutMs = 30000);
    void applyFlushOutcome(const FlushOutcome& outcome);

    void loadCachedStats();
    void saveCachedStats(const QJsonObject& raw) const;
    QString statsCachePath() const;

    static QString surfaceName(Surface surface);
    static QString newEventId();

    GamificationEventQueue m_queue;
    Gamification::StatsSnapshot m_snapshot;
    QSet<QString> m_sessionNotedFeatures;
    QString m_activeOwnerSlug;
    QString m_activeProjectSlug;

    QTimer* m_flushTimer = nullptr;
    bool m_flushInFlight = false;
    bool m_statsRefreshInFlight = false;
    /// Set while a delete-my-data request is pending or in flight: blocks
    /// flushes so a racing flush can't recreate server-side data.
    bool m_deleteInFlight = false;
    bool m_deleteRequestedDuringFlush = false;
    int m_consecutiveFlushFailures = 0;
    qint64 m_nextFlushAllowedAt = 0;
    qint64 m_lastStatsRefreshAt = 0;
    bool m_profilePublic = false;

    static GamificationManager* s_singleton;
};

#endif  // GAMIFICATION_MANAGER_H
