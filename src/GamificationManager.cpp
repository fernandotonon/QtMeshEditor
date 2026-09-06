#include "GamificationManager.h"

#include "AppSettingsKeys.h"
#include "CloudCredentialStore.h"
#include "QtMeshCloudClient.h"
#include "SentryReporter.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaMethod>
#include <QMetaObject>
#include <QPointer>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <atomic>

namespace {

constexpr int kFlushDebounceMs = 5000;
constexpr int kFlushIntervalMs = 90 * 1000;
constexpr qint64 kMaxBackoffMs = 30 * 60 * 1000;

QString gamificationDir()
{
    // Same durable root as sd_models/ai_models/hdri (see AppStorage /
    // SDManager / AIAssistManager). Under Snap this is $SNAP_USER_COMMON.
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/gamification");
}

/// Operations whose op key doubles as (or maps onto) a discovery feature
/// cluster, so one noteOperation() call also counts as feature usage.
QString featureAliasForOp(const QString& opType)
{
    if (opType == QStringLiteral("optimize") || opType == QStringLiteral("decimate_lod"))
        return QStringLiteral("decimate_lod");
    if (opType == QStringLiteral("fix"))
        return {};  // fix is an op type only (feeds `issues_fixed`)
    if (Gamification::featureInfo(opType))
        return opType;
    return {};
}

/// Guardrail: metrics are content-free numeric aggregates only. Anything
/// non-numeric is dropped here so no caller can accidentally leak strings
/// (file names, prompts, ...) into the operations history.
QJsonObject numericMetricsOnly(const QVariantMap& metrics)
{
    QJsonObject out;
    for (auto it = metrics.constBegin(); it != metrics.constEnd(); ++it) {
        if (!Gamification::isValidEventKey(it.key()))
            continue;
        bool okInt = false;
        const qint64 asInt = it.value().toLongLong(&okInt);
        if (okInt && it.value().typeId() != QMetaType::Double) {
            out.insert(it.key(), static_cast<double>(asInt));
            continue;
        }
        bool okDouble = false;
        const double asDouble = it.value().toDouble(&okDouble);
        if (okDouble)
            out.insert(it.key(), asDouble);
    }
    return out;
}

std::atomic<bool> g_emissionSuspended{false};

}  // namespace

GamificationManager* GamificationManager::s_singleton = nullptr;

void GamificationManager::setEmissionSuspended(bool suspended)
{
    g_emissionSuspended.store(suspended);
}

bool GamificationManager::emissionSuspended()
{
    return g_emissionSuspended.load();
}

GamificationManager* GamificationManager::instance()
{
    if (!s_singleton)
        s_singleton = new GamificationManager();
    return s_singleton;
}

GamificationManager* GamificationManager::qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(scriptEngine)
    GamificationManager* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void GamificationManager::kill()
{
    delete s_singleton;
    s_singleton = nullptr;
}

GamificationManager::GamificationManager(QObject* parent)
    : QObject(parent)
    , m_queue(gamificationDir() + QStringLiteral("/queue.json"))
{
    loadCachedStats();

    // Rotate the welcome suggestion once per app run.
    {
        QSettings settings;
        const int cursor = settings.value(AppSettingsKeys::gamificationSuggestionCursor(), 0).toInt();
        settings.setValue(AppSettingsKeys::gamificationSuggestionCursor(), cursor + 1);
    }

    m_flushTimer = new QTimer(this);
    m_flushTimer->setSingleShot(true);
    connect(m_flushTimer, &QTimer::timeout, this, &GamificationManager::flushNow);
    if (!m_queue.isEmpty())
        m_flushTimer->start(kFlushDebounceMs);

    // Periodic retry/flush heartbeat (only does work when something is queued).
    auto* heartbeat = new QTimer(this);
    heartbeat->setInterval(kFlushIntervalMs);
    connect(heartbeat, &QTimer::timeout, this, &GamificationManager::flushNow);
    heartbeat->start();

    if (QCoreApplication::instance()) {
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this]() {
            // Best-effort graceful-shutdown flush; the queue persists anything
            // that doesn't make it out in time.
            if (!m_flushInFlight)
                flushBlocking(3000);
        });
    }

    // First stats refresh shortly after startup, off the critical path.
    QTimer::singleShot(8000, this, [this]() {
        if (signedIn() && syncEnabled())
            refreshStats();
    });
}

GamificationManager::~GamificationManager() = default;

QString GamificationManager::surfaceName(Surface surface)
{
    switch (surface) {
    case Surface::Cli: return QStringLiteral("cli");
    case Surface::Mcp: return QStringLiteral("mcp");
    case Surface::Gui: break;
    }
    return QStringLiteral("gui");
}

QString GamificationManager::newEventId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// ---- Static entry points -------------------------------------------------

void GamificationManager::noteFeature(const QString& featureKey, Surface surface)
{
    auto* app = QCoreApplication::instance();
    if (!app || emissionSuspended())
        return;
    if (QThread::currentThread() != app->thread()) {
        QMetaObject::invokeMethod(app, [featureKey, surface]() {
            noteFeature(featureKey, surface);
        }, Qt::QueuedConnection);
        return;
    }
    instance()->noteFeatureInternal(featureKey, surface);
}

void GamificationManager::noteOperation(const QString& opType, const QVariantMap& metrics,
                                        Surface surface)
{
    auto* app = QCoreApplication::instance();
    if (!app || emissionSuspended())
        return;
    if (QThread::currentThread() != app->thread()) {
        QMetaObject::invokeMethod(app, [opType, metrics, surface]() {
            noteOperation(opType, metrics, surface);
        }, Qt::QueuedConnection);
        return;
    }
    instance()->noteOperationInternal(opType, metrics, surface);
}

void GamificationManager::setProjectContext(const QString& ownerSlug, const QString& projectSlug)
{
    auto* app = QCoreApplication::instance();
    if (!app)
        return;
    GamificationManager* inst = instance();
    inst->m_activeOwnerSlug = ownerSlug.trimmed();
    inst->m_activeProjectSlug = projectSlug.trimmed();
}

// ---- Consent / gating ----------------------------------------------------

bool GamificationManager::signedIn() const
{
    return CloudCredentialStore::hasSession();
}

bool GamificationManager::consentAcknowledged() const
{
    return QSettings().value(AppSettingsKeys::gamificationConsentAcknowledged(), false).toBool();
}

bool GamificationManager::syncEnabled() const
{
    return QSettings().value(AppSettingsKeys::gamificationSyncEnabled(), false).toBool();
}

bool GamificationManager::usageEnabled() const
{
    return QSettings().value(AppSettingsKeys::gamificationUsageEnabled(), true).toBool();
}

bool GamificationManager::opsEnabled() const
{
    return QSettings().value(AppSettingsKeys::gamificationOpsEnabled(), true).toBool();
}

bool GamificationManager::nudgesEnabled() const
{
    return QSettings().value(AppSettingsKeys::gamificationNudgesEnabled(), true).toBool();
}

bool GamificationManager::usageEmissionActive() const
{
    return consentAcknowledged() && syncEnabled() && usageEnabled();
}

bool GamificationManager::opsEmissionActive() const
{
    return consentAcknowledged() && syncEnabled() && opsEnabled();
}

void GamificationManager::setSyncEnabled(bool enabled)
{
    QSettings settings;
    settings.setValue(AppSettingsKeys::gamificationSyncEnabled(), enabled);
    // Turning the master toggle on from Preferences is explicit consent.
    if (enabled)
        settings.setValue(AppSettingsKeys::gamificationConsentAcknowledged(), true);
    emit prefsChanged();
    SentryReporter::addBreadcrumb(QStringLiteral("gamify.prefs"),
                                  QStringLiteral("Progress sync %1")
                                      .arg(enabled ? QStringLiteral("enabled")
                                                   : QStringLiteral("disabled")));
    if (enabled) {
        scheduleFlushSoon();
        refreshStats();
    }
}

void GamificationManager::setUsageEnabled(bool enabled)
{
    QSettings().setValue(AppSettingsKeys::gamificationUsageEnabled(), enabled);
    // Disabling a stream also drops its already-queued events — otherwise a
    // later flush would still send data the user just opted out of.
    if (!enabled)
        m_queue.removeKind(QStringLiteral("feature"));
    emit prefsChanged();
}

void GamificationManager::setOpsEnabled(bool enabled)
{
    QSettings().setValue(AppSettingsKeys::gamificationOpsEnabled(), enabled);
    if (!enabled)
        m_queue.removeKind(QStringLiteral("operation"));
    emit prefsChanged();
}

void GamificationManager::setNudgesEnabled(bool enabled)
{
    QSettings().setValue(AppSettingsKeys::gamificationNudgesEnabled(), enabled);
    emit prefsChanged();
    emit suggestionChanged();
}

void GamificationManager::acceptConsent()
{
    QSettings settings;
    settings.setValue(AppSettingsKeys::gamificationConsentAcknowledged(), true);
    settings.setValue(AppSettingsKeys::gamificationSyncEnabled(), true);
    emit prefsChanged();
    SentryReporter::addBreadcrumb(QStringLiteral("gamify.prefs"),
                                  QStringLiteral("Progress-sync consent accepted"));
    refreshStats();
    scheduleFlushSoon();
}

void GamificationManager::declineConsent()
{
    QSettings settings;
    settings.setValue(AppSettingsKeys::gamificationConsentAcknowledged(), true);
    settings.setValue(AppSettingsKeys::gamificationSyncEnabled(), false);
    emit prefsChanged();
    SentryReporter::addBreadcrumb(QStringLiteral("gamify.prefs"),
                                  QStringLiteral("Progress-sync consent declined"));
}

bool GamificationManager::maybeRequestConsent()
{
    // One-time, non-blocking, and only meaningful when a cloud session exists.
    QSettings settings;
    if (settings.value(AppSettingsKeys::gamificationConsentPrompted(), false).toBool())
        return false;
    if (!signedIn())
        return false;
    // Only consume the one-time prompt when someone can actually show it —
    // a headless CLI/MCP process has no listener and must not burn the
    // user's only chance to see the GUI dialog.
    static const QMetaMethod promptSignal =
        QMetaMethod::fromSignal(&GamificationManager::consentPromptRequested);
    if (!isSignalConnected(promptSignal))
        return false;
    settings.setValue(AppSettingsKeys::gamificationConsentPrompted(), true);
    emit consentPromptRequested();
    return true;
}

QString GamificationManager::examplePayload() const
{
    QJsonObject featureEvent;
    featureEvent.insert(QStringLiteral("id"), QStringLiteral("2f0b7c1e-…"));
    featureEvent.insert(QStringLiteral("feature"), QStringLiteral("retopo"));
    featureEvent.insert(QStringLiteral("at"), 1783000000000.0);
    featureEvent.insert(QStringLiteral("surface"), QStringLiteral("gui"));

    QJsonObject metrics;
    metrics.insert(QStringLiteral("tris_before"), 42180);
    metrics.insert(QStringLiteral("tris_after"), 8004);
    QJsonObject opEvent;
    opEvent.insert(QStringLiteral("id"), QStringLiteral("9a41d3fa-…"));
    opEvent.insert(QStringLiteral("op"), QStringLiteral("retopo"));
    opEvent.insert(QStringLiteral("at"), 1783000000000.0);
    opEvent.insert(QStringLiteral("surface"), QStringLiteral("gui"));
    opEvent.insert(QStringLiteral("metrics"), metrics);

    QJsonObject root;
    root.insert(QStringLiteral("feature_usage_event"), featureEvent);
    root.insert(QStringLiteral("operation_event"), opEvent);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

// ---- Instrumentation -----------------------------------------------------

void GamificationManager::noteFeatureInternal(const QString& featureKey, Surface surface)
{
    if (!Gamification::isValidEventKey(featureKey))
        return;
    if (!consentAcknowledged()) {
        maybeRequestConsent();
        return;
    }
    if (!syncEnabled() || !usageEnabled())
        return;
    if (m_sessionNotedFeatures.contains(featureKey))
        return;  // once per feature per session
    m_sessionNotedFeatures.insert(featureKey);

    QJsonObject body;
    body.insert(QStringLiteral("id"), newEventId());
    body.insert(QStringLiteral("feature"), featureKey);
    body.insert(QStringLiteral("at"),
                static_cast<double>(QDateTime::currentMSecsSinceEpoch()));
    body.insert(QStringLiteral("surface"), surfaceName(surface));

    GamificationEventQueue::Entry entry;
    entry.id = body.value(QStringLiteral("id")).toString();
    entry.kind = QStringLiteral("feature");
    entry.owner = currentEventOwner();
    entry.body = body;
    entry.queuedAt = QDateTime::currentMSecsSinceEpoch();
    m_queue.append(entry);

    SentryReporter::addBreadcrumb(QStringLiteral("gamify.feature"),
                                  QStringLiteral("feature.used %1 (%2)")
                                      .arg(featureKey, surfaceName(surface)));
    emit suggestionChanged();  // the used feature may have been the nudge
    scheduleFlushSoon();
}

void GamificationManager::noteOperationInternal(const QString& opType,
                                                const QVariantMap& metrics, Surface surface)
{
    if (!Gamification::isValidEventKey(opType))
        return;

    // An operation is also feature usage of its cluster (single call site).
    const QString featureKey = featureAliasForOp(opType);
    if (!featureKey.isEmpty())
        noteFeatureInternal(featureKey, surface);

    if (!consentAcknowledged()) {
        maybeRequestConsent();
        return;
    }
    if (!syncEnabled() || !opsEnabled())
        return;

    QJsonObject body;
    body.insert(QStringLiteral("id"), newEventId());
    body.insert(QStringLiteral("op"), opType);
    body.insert(QStringLiteral("at"),
                static_cast<double>(QDateTime::currentMSecsSinceEpoch()));
    body.insert(QStringLiteral("surface"), surfaceName(surface));
    body.insert(QStringLiteral("metrics"), numericMetricsOnly(metrics));
    if (!m_activeOwnerSlug.isEmpty() && !m_activeProjectSlug.isEmpty()) {
        body.insert(QStringLiteral("ownerSlug"), m_activeOwnerSlug);
        body.insert(QStringLiteral("projectSlug"), m_activeProjectSlug);
    }

    GamificationEventQueue::Entry entry;
    entry.id = body.value(QStringLiteral("id")).toString();
    entry.kind = QStringLiteral("operation");
    entry.owner = currentEventOwner();
    entry.body = body;
    entry.queuedAt = QDateTime::currentMSecsSinceEpoch();
    m_queue.append(entry);

    SentryReporter::addBreadcrumb(QStringLiteral("gamify.operation"),
                                  QStringLiteral("operation.completed %1 (%2)")
                                      .arg(opType, surfaceName(surface)));
    scheduleFlushSoon();
}

// ---- Flush ----------------------------------------------------------------

void GamificationManager::scheduleFlushSoon()
{
    if (m_flushTimer && !m_flushTimer->isActive())
        m_flushTimer->start(kFlushDebounceMs);
}

QString GamificationManager::currentEventOwner() const
{
    if (!signedIn())
        return {};
    return QSettings().value(AppSettingsKeys::cloudUserSlug()).toString().trimmed();
}

QList<GamificationEventQueue::Entry> GamificationManager::flushableBatch(
    const QString& kind, QStringList* dropIds) const
{
    // Only entries recorded for the CURRENT account (or logged-out
    // "unclaimed" ones) may flush; entries stamped for a different account
    // must never post to this one — they get dropped instead.
    const QString owner = currentEventOwner();
    QList<GamificationEventQueue::Entry> out;
    const QList<GamificationEventQueue::Entry> all =
        m_queue.peek(kind, -1);
    for (const GamificationEventQueue::Entry& e : all) {
        if (e.owner.isEmpty() || e.owner == owner) {
            if (out.size() < QtMeshCloudClient::kGamificationMaxBatch)
                out.append(e);
        } else if (dropIds) {
            dropIds->append(e.id);
        }
    }
    return out;
}

GamificationManager::FlushOutcome GamificationManager::performFlush(
    const QString& token,
    const QList<GamificationEventQueue::Entry>& featureBatch,
    const QList<GamificationEventQueue::Entry>& operationBatch,
    int timeoutMs)
{
    FlushOutcome outcome;
    outcome.attempted = true;
    outcome.ok = true;

    const auto flushBatch = [&](const QList<GamificationEventQueue::Entry>& batch,
                                auto poster) {
        if (batch.isEmpty())
            return;
        QJsonArray items;
        for (const GamificationEventQueue::Entry& e : batch)
            items.append(e.body);
        const QtMeshCloudClient::GamificationEventsResult result =
            poster(token, items, timeoutMs);
        if (result.ok) {
            outcome.accepted += result.accepted;
            for (const GamificationEventQueue::Entry& e : batch)
                outcome.ackedIds.append(e.id);
            for (const QJsonValue& v : result.newAchievements)
                outcome.newAchievements.append(
                    Gamification::Achievement::fromJson(v.toObject()).toVariantMap());
        } else {
            outcome.ok = false;
            outcome.error = result.errorString;
        }
    };

    flushBatch(featureBatch, [](const QString& t, const QJsonArray& items, int timeout) {
        return QtMeshCloudClient::postEditorEvents(t, items, timeout);
    });
    flushBatch(operationBatch, [](const QString& t, const QJsonArray& items, int timeout) {
        return QtMeshCloudClient::postOperationEvents(t, items, timeout);
    });
    return outcome;
}

void GamificationManager::applyFlushOutcome(const FlushOutcome& outcome)
{
    if (!outcome.attempted)
        return;
    if (!outcome.ackedIds.isEmpty())
        m_queue.acknowledge(outcome.ackedIds);

    if (outcome.ok) {
        m_consecutiveFlushFailures = 0;
        m_nextFlushAllowedAt = 0;
    } else {
        ++m_consecutiveFlushFailures;
        const qint64 backoff = qMin<qint64>(
            kMaxBackoffMs, 60000LL * (1LL << qMin(m_consecutiveFlushFailures - 1, 5)));
        m_nextFlushAllowedAt = QDateTime::currentMSecsSinceEpoch() + backoff;
        SentryReporter::addBreadcrumb(QStringLiteral("gamify.flush"),
                                      QStringLiteral("flush failed (%1), backoff %2 ms")
                                          .arg(outcome.error)
                                          .arg(backoff),
                                      QStringLiteral("warning"));
    }

    if (!outcome.newAchievements.isEmpty())
        emit achievementsUnlocked(outcome.newAchievements);
    if (outcome.accepted > 0 || !outcome.newAchievements.isEmpty())
        refreshStats();
}

void GamificationManager::flushNow()
{
    if (m_flushInFlight || m_deleteInFlight || m_queue.isEmpty())
        return;
    if (!consentAcknowledged() || !syncEnabled())
        return;
    if (!signedIn())
        return;  // zero network when logged out; queue holds
    if (QDateTime::currentMSecsSinceEpoch() < m_nextFlushAllowedAt)
        return;

    const QString token = CloudCredentialStore::loadSession().token;
    if (token.isEmpty())
        return;

    // Snapshot the batches on the main thread; the worker only does network.
    // Streams disabled since queueing are NOT sent (their queued entries were
    // dropped by the toggle, but gate again in case the setting changed
    // out-of-band); entries stamped for a different account are dropped.
    QStringList foreignIds;
    const QList<GamificationEventQueue::Entry> featureBatch =
        usageEnabled() ? flushableBatch(QStringLiteral("feature"), &foreignIds)
                       : QList<GamificationEventQueue::Entry>();
    const QList<GamificationEventQueue::Entry> operationBatch =
        opsEnabled() ? flushableBatch(QStringLiteral("operation"), &foreignIds)
                     : QList<GamificationEventQueue::Entry>();
    if (!foreignIds.isEmpty())
        m_queue.acknowledge(foreignIds);
    if (featureBatch.isEmpty() && operationBatch.isEmpty())
        return;

    m_flushInFlight = true;
    QPointer<GamificationManager> self(this);
    QThread* worker = QThread::create([self, token, featureBatch, operationBatch]() {
        const FlushOutcome outcome = performFlush(token, featureBatch, operationBatch);
        auto* app = QCoreApplication::instance();
        if (!app)
            return;  // app teardown raced the worker; nothing to deliver to
        QMetaObject::invokeMethod(app, [self, outcome]() {
            if (!self)
                return;
            self->m_flushInFlight = false;
            self->applyFlushOutcome(outcome);
            // A delete-my-data request arrived while this flush was in
            // flight: run it now that the flush can no longer race it.
            if (self->m_deleteRequestedDuringFlush) {
                self->m_deleteRequestedDuringFlush = false;
                self->deleteCloudData();
            }
        });
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

int GamificationManager::flushBlocking(int timeoutMs)
{
    if (m_flushInFlight || m_deleteInFlight || m_queue.isEmpty())
        return 0;
    if (!consentAcknowledged() || !syncEnabled() || !signedIn())
        return 0;
    const QString token = CloudCredentialStore::loadSession().token;
    if (token.isEmpty())
        return 0;

    QStringList foreignIds;
    const QList<GamificationEventQueue::Entry> featureBatch =
        usageEnabled() ? flushableBatch(QStringLiteral("feature"), &foreignIds)
                       : QList<GamificationEventQueue::Entry>();
    const QList<GamificationEventQueue::Entry> operationBatch =
        opsEnabled() ? flushableBatch(QStringLiteral("operation"), &foreignIds)
                     : QList<GamificationEventQueue::Entry>();
    if (!foreignIds.isEmpty())
        m_queue.acknowledge(foreignIds);
    if (featureBatch.isEmpty() && operationBatch.isEmpty())
        return 0;

    m_flushInFlight = true;
    const FlushOutcome outcome =
        performFlush(token, featureBatch, operationBatch, qMax(500, timeoutMs));
    m_flushInFlight = false;
    if (!outcome.ackedIds.isEmpty())
        m_queue.acknowledge(outcome.ackedIds);
    if (!outcome.newAchievements.isEmpty())
        emit achievementsUnlocked(outcome.newAchievements);
    return outcome.accepted;
}

// ---- Stats -----------------------------------------------------------------

QString GamificationManager::statsCachePath() const
{
    return gamificationDir() + QStringLiteral("/stats_cache.json");
}

void GamificationManager::loadCachedStats()
{
    QFile file(statsCachePath());
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (doc.isObject())
        m_snapshot = Gamification::StatsSnapshot::fromJson(doc.object());
}

void GamificationManager::saveCachedStats(const QJsonObject& raw) const
{
    QDir().mkpath(gamificationDir());
    QFile file(statsCachePath());
    if (file.open(QIODevice::WriteOnly))
        file.write(QJsonDocument(raw).toJson(QJsonDocument::Compact));
}

void GamificationManager::refreshStats()
{
    if (m_statsRefreshInFlight)
        return;
    if (!signedIn() || !syncEnabled())
        return;
    const QString token = CloudCredentialStore::loadSession().token;
    if (token.isEmpty())
        return;

    m_statsRefreshInFlight = true;
    QPointer<GamificationManager> self(this);
    QThread* worker = QThread::create([self, token]() {
        const QtMeshCloudClient::GamificationStatsResult result =
            QtMeshCloudClient::fetchGamificationStats(token);
        auto* app = QCoreApplication::instance();
        if (!app)
            return;  // app teardown raced the worker; nothing to deliver to
        QMetaObject::invokeMethod(app, [self, result]() {
            if (!self)
                return;
            self->m_statsRefreshInFlight = false;
            if (!result.ok)
                return;
            self->m_lastStatsRefreshAt = QDateTime::currentMSecsSinceEpoch();
            self->m_snapshot = Gamification::StatsSnapshot::fromJson(result.stats);
            self->saveCachedStats(result.stats);
            emit self->statsChanged();
            emit self->suggestionChanged();
        });
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void GamificationManager::refreshStatsIfStale(qint64 maxAgeMs)
{
    if (QDateTime::currentMSecsSinceEpoch() - m_lastStatsRefreshAt >= maxAgeMs)
        refreshStats();
}

void GamificationManager::handleSessionChanged()
{
    if (!signedIn()) {
        // Keep the queue (it flushes after the next sign-in) but drop the
        // previous account's stats so nothing leaks across users.
        m_snapshot = Gamification::StatsSnapshot();
        QFile::remove(statsCachePath());
        emit statsChanged();
    } else {
        refreshStats();
        scheduleFlushSoon();
    }
    emit sessionChanged();
    emit suggestionChanged();
}

// ---- Status surface --------------------------------------------------------

QVariantList GamificationManager::nextUnlockables() const
{
    QVariantList out;
    if (!m_snapshot.valid)
        return out;
    const QList<Gamification::NextUnlockable> next = m_snapshot.nextUnlockables(2);
    for (const Gamification::NextUnlockable& u : next)
        out.append(u.toVariantMap());
    return out;
}

QString GamificationManager::profileUrl() const
{
    const QString slug = QSettings().value(AppSettingsKeys::cloudUserSlug()).toString().trimmed();
    return QtMeshCloudClient::profileUrl(slug);
}

void GamificationManager::openProfile()
{
    const QString url = profileUrl();
    if (url.isEmpty())
        return;
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("Gamification: open web profile"));
    QDesktopServices::openUrl(QUrl(url));
}

// ---- Discovery suggestion ---------------------------------------------------

QVariantMap GamificationManager::suggestion() const
{
    QVariantMap out;
    if (!nudgesEnabled())
        return out;

    const QStringList dismissed =
        QSettings().value(AppSettingsKeys::gamificationDismissedSuggestions()).toStringList();

    // Personalized (unused clusters) when stats are known; otherwise a generic
    // rotation through the catalog for logged-out users.
    QStringList candidates = m_snapshot.valid
                                 ? m_snapshot.unusedFeatureKeys()
                                 : [] {
                                       QStringList all;
                                       for (const auto& f : Gamification::featureCatalog())
                                           all.append(f.key);
                                       return all;
                                   }();
    for (const QString& key : dismissed)
        candidates.removeAll(key);
    for (const QString& key : m_sessionNotedFeatures)
        candidates.removeAll(key);
    if (candidates.isEmpty())
        return out;

    const int cursor =
        QSettings().value(AppSettingsKeys::gamificationSuggestionCursor(), 0).toInt();
    const QString key = candidates.at(qAbs(cursor) % candidates.size());
    const Gamification::FeatureInfo* info = Gamification::featureInfo(key);
    if (!info)
        return out;

    out.insert(QStringLiteral("featureKey"), info->key);
    out.insert(QStringLiteral("title"), info->title);
    out.insert(QStringLiteral("blurb"), info->blurb);
    out.insert(QStringLiteral("personalized"), m_snapshot.valid);
    return out;
}

void GamificationManager::dismissSuggestion(const QString& featureKey)
{
    QSettings settings;
    QStringList dismissed =
        settings.value(AppSettingsKeys::gamificationDismissedSuggestions()).toStringList();
    if (!dismissed.contains(featureKey))
        dismissed.append(featureKey);
    settings.setValue(AppSettingsKeys::gamificationDismissedSuggestions(), dismissed);
    emit suggestionChanged();
}

void GamificationManager::advanceSuggestion()
{
    QSettings settings;
    const int cursor = settings.value(AppSettingsKeys::gamificationSuggestionCursor(), 0).toInt();
    settings.setValue(AppSettingsKeys::gamificationSuggestionCursor(), cursor + 1);
    emit suggestionChanged();
}

// ---- Privacy ---------------------------------------------------------------

void GamificationManager::deleteCloudData()
{
    // Serialize with flushing: a flush already in flight could recreate
    // server-side rows right after the purge, so defer until it settles;
    // m_deleteInFlight blocks any NEW flush from starting meanwhile.
    m_deleteInFlight = true;
    if (m_flushInFlight) {
        m_deleteRequestedDuringFlush = true;
        return;
    }
    if (m_flushTimer)
        m_flushTimer->stop();

    // Local half first — queued events must stop being upload-eligible the
    // moment the user asks, regardless of how the server call goes.
    const bool localCleared = m_queue.clear();
    m_sessionNotedFeatures.clear();
    m_snapshot = Gamification::StatsSnapshot();
    QFile::remove(statsCachePath());
    emit statsChanged();
    emit suggestionChanged();

    const QString token = CloudCredentialStore::loadSession().token;
    if (token.isEmpty()) {
        m_deleteInFlight = false;
        emit deleteCloudDataFinished(
            localCleared, localCleared
                              ? QString()
                              : tr("Could not clear the local event queue — try again."));
        return;
    }

    performCloudDelete(token);
}

void GamificationManager::performCloudDelete(const QString& token)
{
    QPointer<GamificationManager> self(this);
    QThread* worker = QThread::create([self, token]() {
        const QtMeshCloudClient::UploadResult result =
            QtMeshCloudClient::deleteGamificationData(token);
        auto* app = QCoreApplication::instance();
        if (!app)
            return;  // app teardown raced the worker; nothing to deliver to
        QMetaObject::invokeMethod(app, [self, result]() {
            if (!self)
                return;
            self->m_deleteInFlight = false;
            SentryReporter::addBreadcrumb(QStringLiteral("gamify.prefs"),
                                          result.ok
                                              ? QStringLiteral("Gamification data deleted")
                                              : QStringLiteral("Gamification data delete failed"));
            emit self->deleteCloudDataFinished(result.ok, result.errorString);
        });
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void GamificationManager::refreshCloudPrefs()
{
    // Opted-out users get zero gamification network traffic, including the
    // prefs fetch the Preferences pane triggers.
    if (!syncEnabled())
        return;
    const QString token = CloudCredentialStore::loadSession().token;
    if (token.isEmpty())
        return;
    QPointer<GamificationManager> self(this);
    QThread* worker = QThread::create([self, token]() {
        const QtMeshCloudClient::GamificationPrefsResult result =
            QtMeshCloudClient::fetchGamificationPrefs(token);
        auto* app = QCoreApplication::instance();
        if (!app)
            return;  // app teardown raced the worker; nothing to deliver to
        QMetaObject::invokeMethod(app, [self, result]() {
            if (!self || !result.ok)
                return;
            self->m_profilePublic = result.profilePublic;
            emit self->cloudPrefsChanged();
        });
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void GamificationManager::setProfilePublic(bool isPublic)
{
    if (!syncEnabled())
        return;
    const QString token = CloudCredentialStore::loadSession().token;
    if (token.isEmpty())
        return;
    QPointer<GamificationManager> self(this);
    QThread* worker = QThread::create([self, token, isPublic]() {
        // The cloud PUT merges partial bodies onto the stored prefs, so only
        // the field being changed is sent.
        QJsonObject patch;
        patch.insert(QStringLiteral("profilePublic"), isPublic);
        const QtMeshCloudClient::GamificationPrefsResult result =
            QtMeshCloudClient::setGamificationPrefs(token, patch);
        auto* app = QCoreApplication::instance();
        if (!app)
            return;  // app teardown raced the worker; nothing to deliver to
        QMetaObject::invokeMethod(app, [self, result]() {
            GamificationManager* mgr = self.data();
            if (!mgr)
                return;
            if (result.ok)
                mgr->m_profilePublic = result.profilePublic;
            emit mgr->cloudPrefsChanged();
        });
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}
