#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>

#include "AppSettingsKeys.h"
#include "CloudCredentialStore.h"
#include "GamificationManager.h"

namespace {

/// Minimal HTTP mock for the gamification endpoints.
struct GamifyHttpMock {
    QTcpServer server;
    QStringList paths;
    QList<QJsonObject> bodies;
    QByteArray statsBody;

    bool listen()
    {
        statsBody = QByteArrayLiteral(
            R"({"stats":{"xp":25,"level":1,"current_streak":1,"longest_streak":1,)"
            R"("last_active_day":"2026-07-05"},)"
            R"("progress":{"level":1,"levelFloorXp":0,"nextLevelXp":100,)"
            R"("intoLevel":25,"span":100,"fraction":0.25},)"
            R"("achievements":[],"featureUsage":[],"counters":{},)"
            R"("recentlyEarned":[],"recentOperations":[]})");

        QObject::connect(&server, &QTcpServer::newConnection, [this]() {
            QTcpSocket* socket = server.nextPendingConnection();
            auto buffer = std::make_shared<QByteArray>();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer]() {
                buffer->append(socket->readAll());
                const int headerEnd = buffer->indexOf("\r\n\r\n");
                if (headerEnd < 0)
                    return;
                const QByteArray head = buffer->left(headerEnd);
                int contentLength = 0;
                for (const QByteArray& line : head.split('\n')) {
                    if (line.toLower().startsWith("content-length:"))
                        contentLength = line.mid(15).trimmed().toInt();
                }
                if (buffer->size() < headerEnd + 4 + contentLength)
                    return;

                const QString requestLine = QString::fromUtf8(head.left(head.indexOf('\r')));
                const QString method = requestLine.section(QLatin1Char(' '), 0, 0);
                const QString path = requestLine.section(QLatin1Char(' '), 1, 1);
                paths.append(method + QLatin1Char(' ') + path);
                const QJsonObject body =
                    QJsonDocument::fromJson(buffer->mid(headerEnd + 4)).object();
                bodies.append(body);

                QByteArray response;
                if (path == QStringLiteral("/v1/me/stats")) {
                    response = statsBody;
                } else if (path == QStringLiteral("/v1/events/editor")
                           || path == QStringLiteral("/v1/events/operations")) {
                    const QString field = path.endsWith(QStringLiteral("editor"))
                                              ? QStringLiteral("events")
                                              : QStringLiteral("operations");
                    QJsonObject r;
                    r.insert(QStringLiteral("accepted"),
                             body.value(field).toArray().size());
                    QJsonArray achievements;
                    if (field == QStringLiteral("events")) {
                        QJsonObject a;
                        a.insert(QStringLiteral("key"), QStringLiteral("first_retopo"));
                        a.insert(QStringLiteral("title"), QStringLiteral("Retopologist"));
                        a.insert(QStringLiteral("tier"), QStringLiteral("bronze"));
                        a.insert(QStringLiteral("xp"), 25);
                        achievements.append(a);
                    }
                    r.insert(QStringLiteral("newAchievements"), achievements);
                    response = QJsonDocument(r).toJson(QJsonDocument::Compact);
                } else if (path == QStringLiteral("/v1/me/gamification")) {
                    response = QByteArrayLiteral(R"({"ok":true})");
                } else {
                    response = QByteArrayLiteral(R"({"error":"not found"})");
                }

                QByteArray out = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n";
                out += "Content-Length: " + QByteArray::number(response.size());
                out += "\r\nConnection: close\r\n\r\n" + response;
                socket->write(out);
                socket->flush();
                socket->waitForBytesWritten(2000);
                socket->disconnectFromHost();
            });
        });
        return server.listen(QHostAddress::LocalHost);
    }

    QString baseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
    }
};

}  // namespace

class GamificationManagerTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        m_prevOrg = QCoreApplication::organizationName();
        m_prevApp = QCoreApplication::applicationName();
        QCoreApplication::setOrganizationName(QStringLiteral("QtMeshEditorTests"));
        QCoreApplication::setApplicationName(QStringLiteral("GamificationManagerTest"));
        QStandardPaths::setTestModeEnabled(true);
        QSettings().clear();
        CloudCredentialStore::resetCacheForTesting();
        cleanStorage();
        m_prevApiBase = qgetenv("QTMESH_API_BASE");
        GamificationManager::setEmissionSuspended(false);
        GamificationManager::kill();
    }

    void TearDown() override
    {
        GamificationManager::setEmissionSuspended(false);
        GamificationManager::kill();
        QSettings().clear();
        CloudCredentialStore::resetCacheForTesting();
        cleanStorage();
        if (m_prevApiBase.isEmpty())
            qunsetenv("QTMESH_API_BASE");
        else
            qputenv("QTMESH_API_BASE", m_prevApiBase);
        QStandardPaths::setTestModeEnabled(false);
        QCoreApplication::setOrganizationName(m_prevOrg);
        QCoreApplication::setApplicationName(m_prevApp);
    }

    static void cleanStorage()
    {
        const QString dir =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            + QStringLiteral("/gamification");
        QDir(dir).removeRecursively();
    }

    static void enableSync()
    {
        QSettings settings;
        settings.setValue(AppSettingsKeys::gamificationConsentAcknowledged(), true);
        settings.setValue(AppSettingsKeys::gamificationSyncEnabled(), true);
    }

    static void signIn()
    {
        CloudSession session;
        session.token = QStringLiteral("qtm_sess_test_token");
        session.expiresAt = 9999999999999LL;
        session.email = QStringLiteral("test@example.com");
        ASSERT_TRUE(CloudCredentialStore::saveSession(session));
    }

    QString m_prevOrg;
    QString m_prevApp;
    QByteArray m_prevApiBase;
};

TEST_F(GamificationManagerTest, NoConsentMeansNothingQueued)
{
    auto* gamify = GamificationManager::instance();
    EXPECT_FALSE(gamify->consentAcknowledged());
    EXPECT_FALSE(gamify->syncEnabled());
    GamificationManager::noteFeature(QStringLiteral("retopo"));
    GamificationManager::noteOperation(QStringLiteral("retopo"),
                                       {{QStringLiteral("tris_before"), 100}});
    EXPECT_EQ(gamify->pendingEventCount(), 0);
}

TEST_F(GamificationManagerTest, ConsentPromptRequestedOncePerInstallWhenSignedIn)
{
    signIn();
    auto* gamify = GamificationManager::instance();
    QSignalSpy spy(gamify, &GamificationManager::consentPromptRequested);
    GamificationManager::noteFeature(QStringLiteral("retopo"));
    EXPECT_EQ(spy.count(), 1);
    GamificationManager::noteFeature(QStringLiteral("uv_unwrap"));
    EXPECT_EQ(spy.count(), 1);  // once, ever
    EXPECT_EQ(gamify->pendingEventCount(), 0);
}

TEST_F(GamificationManagerTest, NoteFeatureQueuesOncePerSession)
{
    enableSync();
    auto* gamify = GamificationManager::instance();
    GamificationManager::noteFeature(QStringLiteral("retopo"));
    GamificationManager::noteFeature(QStringLiteral("retopo"));
    GamificationManager::noteFeature(QStringLiteral("uv_unwrap"));
    GamificationManager::noteFeature(QStringLiteral("NOT A KEY"));
    EXPECT_EQ(gamify->pendingEventCount(), 2);
}

TEST_F(GamificationManagerTest, NoteOperationFiltersNonNumericMetricsAndAliasesFeature)
{
    enableSync();
    auto* gamify = GamificationManager::instance();
    GamificationManager::noteOperation(
        QStringLiteral("retopo"),
        {{QStringLiteral("tris_before"), 42180},
         {QStringLiteral("tris_after"), 8004},
         {QStringLiteral("quad_ratio_after"), 0.82},
         {QStringLiteral("sneaky_path"), QStringLiteral("/Users/x/secret.fbx")},
         {QStringLiteral("Bad Key"), 3}});
    // 1 operation event + 1 aliased feature.used event.
    EXPECT_EQ(gamify->pendingEventCount(), 2);

    // Inspect the persisted queue: the operation body must be numeric-only.
    const QString queuePath =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/gamification/queue.json");
    QFile f(queuePath);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QJsonArray events =
        QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("events")).toArray();
    bool foundOp = false;
    for (const QJsonValue& v : events) {
        const QJsonObject entry = v.toObject();
        if (entry.value(QStringLiteral("kind")).toString() != QStringLiteral("operation"))
            continue;
        foundOp = true;
        const QJsonObject body = entry.value(QStringLiteral("body")).toObject();
        EXPECT_EQ(body.value(QStringLiteral("op")).toString(), QStringLiteral("retopo"));
        const QJsonObject metrics = body.value(QStringLiteral("metrics")).toObject();
        EXPECT_EQ(metrics.value(QStringLiteral("tris_before")).toDouble(), 42180.0);
        EXPECT_DOUBLE_EQ(metrics.value(QStringLiteral("quad_ratio_after")).toDouble(), 0.82);
        EXPECT_FALSE(metrics.contains(QStringLiteral("sneaky_path")));
        EXPECT_FALSE(metrics.contains(QStringLiteral("Bad Key")));
    }
    EXPECT_TRUE(foundOp);
}

TEST_F(GamificationManagerTest, StreamOptOutsBlockTheirEvents)
{
    enableSync();
    QSettings().setValue(AppSettingsKeys::gamificationUsageEnabled(), false);
    auto* gamify = GamificationManager::instance();
    GamificationManager::noteFeature(QStringLiteral("retopo"));
    EXPECT_EQ(gamify->pendingEventCount(), 0);

    QSettings().setValue(AppSettingsKeys::gamificationOpsEnabled(), false);
    GamificationManager::noteOperation(QStringLiteral("auto_rig"),
                                       {{QStringLiteral("bones_created"), 19}});
    EXPECT_EQ(gamify->pendingEventCount(), 0);
}

TEST_F(GamificationManagerTest, FlushBlockingIsNoOpWhenLoggedOut)
{
    enableSync();
    auto* gamify = GamificationManager::instance();
    GamificationManager::noteFeature(QStringLiteral("retopo"));
    EXPECT_EQ(gamify->pendingEventCount(), 1);
    // Logged out: zero network, events held.
    EXPECT_EQ(gamify->flushBlocking(), 0);
    EXPECT_EQ(gamify->pendingEventCount(), 1);
}

TEST_F(GamificationManagerTest, FlushBlockingPostsBatchesAndAcks)
{
    GamifyHttpMock mock;
    ASSERT_TRUE(mock.listen());
    qputenv("QTMESH_API_BASE", mock.baseUrl().toUtf8());

    enableSync();
    signIn();
    auto* gamify = GamificationManager::instance();
    GamificationManager::noteFeature(QStringLiteral("retopo"));
    GamificationManager::noteOperation(QStringLiteral("retopo"),
                                       {{QStringLiteral("tris_before"), 100},
                                        {QStringLiteral("tris_after"), 50}});
    // retopo op also aliases the retopo feature — already noted, so: 1
    // feature event + 1 operation event.
    EXPECT_EQ(gamify->pendingEventCount(), 2);

    QSignalSpy unlocked(gamify, &GamificationManager::achievementsUnlocked);
    const int accepted = gamify->flushBlocking();
    EXPECT_EQ(accepted, 2);
    EXPECT_EQ(gamify->pendingEventCount(), 0);
    ASSERT_EQ(unlocked.count(), 1);
    const QVariantList achievements = unlocked.first().first().toList();
    ASSERT_EQ(achievements.size(), 1);
    EXPECT_EQ(achievements.first().toMap().value(QStringLiteral("title")).toString(),
              QStringLiteral("Retopologist"));

    // Both endpoints hit, with the exact contract paths.
    EXPECT_TRUE(mock.paths.contains(QStringLiteral("POST /v1/events/editor")));
    EXPECT_TRUE(mock.paths.contains(QStringLiteral("POST /v1/events/operations")));

    // Event bodies carry id/feature/at/surface per the wire contract.
    for (int i = 0; i < mock.paths.size(); ++i) {
        if (mock.paths.at(i) == QStringLiteral("POST /v1/events/editor")) {
            const QJsonArray events = mock.bodies.at(i).value(QStringLiteral("events")).toArray();
            ASSERT_EQ(events.size(), 1);
            const QJsonObject e = events.first().toObject();
            EXPECT_FALSE(e.value(QStringLiteral("id")).toString().isEmpty());
            EXPECT_EQ(e.value(QStringLiteral("feature")).toString(), QStringLiteral("retopo"));
            EXPECT_GT(e.value(QStringLiteral("at")).toDouble(), 0.0);
            EXPECT_EQ(e.value(QStringLiteral("surface")).toString(), QStringLiteral("gui"));
        }
    }
}

TEST_F(GamificationManagerTest, DeleteCloudDataClearsLocallyWhenLoggedOut)
{
    enableSync();
    auto* gamify = GamificationManager::instance();
    GamificationManager::noteFeature(QStringLiteral("retopo"));
    EXPECT_EQ(gamify->pendingEventCount(), 1);
    QSignalSpy done(gamify, &GamificationManager::deleteCloudDataFinished);
    gamify->deleteCloudData();
    ASSERT_EQ(done.count(), 1);
    EXPECT_TRUE(done.first().first().toBool());
    EXPECT_EQ(gamify->pendingEventCount(), 0);
}

TEST_F(GamificationManagerTest, SuggestionRotationAndDismissal)
{
    auto* gamify = GamificationManager::instance();
    const QVariantMap first = gamify->suggestion();
    ASSERT_TRUE(first.contains(QStringLiteral("featureKey")));  // generic, logged out
    const QString firstKey = first.value(QStringLiteral("featureKey")).toString();

    gamify->dismissSuggestion(firstKey);
    const QVariantMap second = gamify->suggestion();
    EXPECT_NE(second.value(QStringLiteral("featureKey")).toString(), firstKey);

    gamify->setNudgesEnabled(false);
    EXPECT_TRUE(gamify->suggestion().isEmpty());
    gamify->setNudgesEnabled(true);
    EXPECT_FALSE(gamify->suggestion().isEmpty());
}

TEST_F(GamificationManagerTest, ConsentPromptNotConsumedWithoutListener)
{
    // Headless contexts (CLI/MCP) have no MainWindow connected to
    // consentPromptRequested — they must not burn the one-time GUI prompt.
    signIn();
    GamificationManager::instance();  // create WITHOUT a QSignalSpy attached
    GamificationManager::noteFeature(QStringLiteral("retopo"));
    EXPECT_FALSE(QSettings()
                     .value(AppSettingsKeys::gamificationConsentPrompted(), false)
                     .toBool());
}

TEST_F(GamificationManagerTest, EmissionSuspendedBlocksAllNotes)
{
    enableSync();
    auto* gamify = GamificationManager::instance();
    GamificationManager::setEmissionSuspended(true);
    GamificationManager::noteFeature(QStringLiteral("retopo"));
    GamificationManager::noteOperation(QStringLiteral("retopo"),
                                       {{QStringLiteral("tris_before"), 10}});
    EXPECT_EQ(gamify->pendingEventCount(), 0);
    GamificationManager::setEmissionSuspended(false);
    GamificationManager::noteFeature(QStringLiteral("retopo"));
    EXPECT_EQ(gamify->pendingEventCount(), 1);
}

TEST_F(GamificationManagerTest, DisablingStreamDropsItsQueuedEvents)
{
    enableSync();
    auto* gamify = GamificationManager::instance();
    GamificationManager::noteFeature(QStringLiteral("retopo"));
    GamificationManager::noteOperation(QStringLiteral("auto_rig"),
                                       {{QStringLiteral("bones_created"), 19}});
    // retopo feature + auto_rig feature alias + auto_rig operation
    EXPECT_EQ(gamify->pendingEventCount(), 3);
    gamify->setOpsEnabled(false);
    EXPECT_EQ(gamify->pendingEventCount(), 2);  // operation dropped
    gamify->setUsageEnabled(false);
    EXPECT_EQ(gamify->pendingEventCount(), 0);  // features dropped
}

TEST_F(GamificationManagerTest, EventsFromAnotherAccountAreDroppedNotSent)
{
    GamifyHttpMock mock;
    ASSERT_TRUE(mock.listen());
    qputenv("QTMESH_API_BASE", mock.baseUrl().toUtf8());

    enableSync();
    signIn();
    QSettings().setValue(AppSettingsKeys::cloudUserSlug(), QStringLiteral("user-a"));
    auto* gamify = GamificationManager::instance();
    GamificationManager::noteFeature(QStringLiteral("retopo"));
    EXPECT_EQ(gamify->pendingEventCount(), 1);

    // Account switch: same machine, different user.
    QSettings().setValue(AppSettingsKeys::cloudUserSlug(), QStringLiteral("user-b"));
    EXPECT_EQ(gamify->flushBlocking(), 0);
    // user-a's event was dropped, not posted to user-b's account.
    EXPECT_EQ(gamify->pendingEventCount(), 0);
    EXPECT_TRUE(mock.paths.isEmpty());
}

TEST_F(GamificationManagerTest, AcceptAndDeclineConsent)
{
    auto* gamify = GamificationManager::instance();
    gamify->acceptConsent();
    EXPECT_TRUE(gamify->consentAcknowledged());
    EXPECT_TRUE(gamify->syncEnabled());
    gamify->declineConsent();
    EXPECT_TRUE(gamify->consentAcknowledged());
    EXPECT_FALSE(gamify->syncEnabled());
}

TEST_F(GamificationManagerTest, ExamplePayloadIsContentFreeJson)
{
    auto* gamify = GamificationManager::instance();
    const QString example = gamify->examplePayload();
    const QJsonObject root = QJsonDocument::fromJson(example.toUtf8()).object();
    EXPECT_TRUE(root.contains(QStringLiteral("feature_usage_event")));
    EXPECT_TRUE(root.contains(QStringLiteral("operation_event")));
}
