#include "CloudCredentialStore.h"
#include "AppSettingsKeys.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>

class CloudCredentialStoreTest : public ::testing::Test {
protected:
    QString previousOrganizationName;
    QString previousApplicationName;

    void SetUp() override
    {
        previousOrganizationName = QCoreApplication::organizationName();
        previousApplicationName = QCoreApplication::applicationName();
        QCoreApplication::setOrganizationName(QStringLiteral("QtMeshEditorTests"));
        QCoreApplication::setApplicationName(QStringLiteral("CloudCredentialStoreTest"));
        QSettings().clear();
        CloudCredentialStore::clearSession();
    }

    void TearDown() override
    {
        CloudCredentialStore::clearSession();
        QSettings().clear();
        QCoreApplication::setOrganizationName(previousOrganizationName);
        QCoreApplication::setApplicationName(previousApplicationName);
    }
};

TEST_F(CloudCredentialStoreTest, RoundTripAndClear)
{
    CloudSession session;
    session.token = QStringLiteral("test-token");
    session.expiresAt = 1234567890;
    session.email = QStringLiteral("user@example.com");
    ASSERT_TRUE(CloudCredentialStore::saveSession(session));

    const CloudSession loaded = CloudCredentialStore::loadSession();
    EXPECT_EQ(loaded.token, session.token);
    EXPECT_EQ(loaded.expiresAt, session.expiresAt);
    EXPECT_EQ(loaded.email, session.email);
    EXPECT_TRUE(CloudCredentialStore::hasSession());

    CloudCredentialStore::clearSession();
    EXPECT_FALSE(CloudCredentialStore::hasSession());
}

TEST_F(CloudCredentialStoreTest, SaveSessionRejectsEmptyToken)
{
    CloudSession session;
    session.email = QStringLiteral("user@example.com");
    EXPECT_FALSE(CloudCredentialStore::saveSession(session));
    EXPECT_FALSE(CloudCredentialStore::hasSession());
}

TEST_F(CloudCredentialStoreTest, HasSessionFalseWhenCleared)
{
    EXPECT_FALSE(CloudCredentialStore::hasSession());
}

TEST_F(CloudCredentialStoreTest, LoadSessionReturnsEmptyWhenNoTokenStored)
{
    // No token in QSettings → no session, even if a stray email key lingers.
    QSettings settings;
    settings.setValue(AppSettingsKeys::cloudUserEmail(), QStringLiteral("orphan@example.com"));
    settings.sync();
    CloudCredentialStore::resetCacheForTesting();

    const CloudSession loaded = CloudCredentialStore::loadSession();
    EXPECT_FALSE(loaded.hasToken());
}

TEST_F(CloudCredentialStoreTest, CacheServesReadsWithoutHittingBackingStore)
{
    CloudSession session;
    session.token = QStringLiteral("cached-token");
    session.expiresAt = 7;
    ASSERT_TRUE(CloudCredentialStore::saveSession(session));

    // Wipe the backing store keys out from under the cache. Because
    // saveSession() primed the in-process cache, subsequent reads must still
    // succeed without re-reading QSettings — this is what collapses the
    // account control's repeated startup reads into one.
    QSettings settings;
    settings.remove(AppSettingsKeys::cloudToken());
    settings.sync();

    const CloudSession cached = CloudCredentialStore::loadSession();
    EXPECT_EQ(cached.token, QStringLiteral("cached-token"));
    EXPECT_TRUE(CloudCredentialStore::hasSession());

    // After an explicit cache reset the read falls through to the (wiped) store.
    CloudCredentialStore::resetCacheForTesting();
    EXPECT_FALSE(CloudCredentialStore::hasSession());
}

TEST_F(CloudCredentialStoreTest, MigrateIsNoOp)
{
    // Tokens already live in QSettings, so migration is a no-op and must leave
    // any pre-existing token in place.
    QSettings settings;
    settings.setValue(AppSettingsKeys::cloudToken(), QStringLiteral("existing-token"));
    settings.setValue(AppSettingsKeys::cloudTokenExpiresAt(), 42);
    settings.setValue(AppSettingsKeys::cloudUserEmail(), QStringLiteral("user@example.com"));
    settings.sync();
    CloudCredentialStore::resetCacheForTesting();

    CloudCredentialStore::migrateLegacySettingsIfNeeded();

    const CloudSession loaded = CloudCredentialStore::loadSession();
    EXPECT_EQ(loaded.token, QStringLiteral("existing-token"));
    EXPECT_EQ(loaded.expiresAt, 42);
    EXPECT_EQ(loaded.email, QStringLiteral("user@example.com"));
}

TEST_F(CloudCredentialStoreTest, MigratesLegacyFallbackFileIntoSettings)
{
    // Simulate an upgrade from a pre-QSettings build that stored the session in
    // the mode-0600 fallback file. No token in QSettings yet → migration must
    // read the legacy file once and copy it into QSettings so the user stays
    // signed in.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    ASSERT_FALSE(dir.isEmpty());
    QDir().mkpath(dir);
    const QString legacyPath = dir + QStringLiteral("/cloud_session.dat");
    QFile f(legacyPath);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(QByteArrayLiteral(
        "{\"token\":\"legacy-tok\",\"expiresAt\":123,\"email\":\"old@example.com\"}"));
    f.close();
    CloudCredentialStore::resetCacheForTesting();

    CloudCredentialStore::migrateLegacySettingsIfNeeded();

    const CloudSession loaded = CloudCredentialStore::loadSession();
    EXPECT_EQ(loaded.token, QStringLiteral("legacy-tok"));
    EXPECT_EQ(loaded.expiresAt, 123);
    EXPECT_EQ(loaded.email, QStringLiteral("old@example.com"));
    // It must have landed in QSettings (not just the cache).
    CloudCredentialStore::resetCacheForTesting();
    QSettings settings;
    EXPECT_EQ(settings.value(AppSettingsKeys::cloudToken()).toString(),
              QStringLiteral("legacy-tok"));

    QFile::remove(legacyPath);
}

TEST_F(CloudCredentialStoreTest, LegacyMigrationProbesOnlyOnce)
{
    // First call: nothing in the legacy store, so nothing migrates — but the
    // probe must be marked done so the OS keychain is never touched again
    // (the repeated-prompt bug).
    CloudCredentialStore::migrateLegacySettingsIfNeeded();
    {
        QSettings settings;
        EXPECT_TRUE(settings.value(AppSettingsKeys::cloudLegacyMigrationDone()).toBool());
    }

    // Now drop a legacy fallback file. A second migrate must IGNORE it (the
    // done-flag short-circuits before any legacy read), so no session appears.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    const QString legacyPath = dir + QStringLiteral("/cloud_session.dat");
    QFile f(legacyPath);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(QByteArrayLiteral("{\"token\":\"should-be-ignored\",\"expiresAt\":1}"));
    f.close();
    CloudCredentialStore::resetCacheForTesting();

    CloudCredentialStore::migrateLegacySettingsIfNeeded();
    EXPECT_FALSE(CloudCredentialStore::hasSession());

    QFile::remove(legacyPath);
}

TEST_F(CloudCredentialStoreTest, LoadSessionDropsEmailWhenTokenMissing)
{
    // A stray email/expiry key without a token must not surface as a session.
    QSettings settings;
    settings.setValue(AppSettingsKeys::cloudUserEmail(), QStringLiteral("stale@example.com"));
    settings.setValue(AppSettingsKeys::cloudTokenExpiresAt(), 999);
    settings.sync();
    CloudCredentialStore::resetCacheForTesting();

    const CloudSession loaded = CloudCredentialStore::loadSession();
    EXPECT_FALSE(loaded.hasToken());
    EXPECT_TRUE(loaded.email.isEmpty());
    EXPECT_EQ(loaded.expiresAt, 0);
}

TEST_F(CloudCredentialStoreTest, RoundTripWithoutEmail)
{
    CloudSession session;
    session.token = QStringLiteral("token-only");
    session.expiresAt = 99;
    ASSERT_TRUE(CloudCredentialStore::saveSession(session));

    const CloudSession loaded = CloudCredentialStore::loadSession();
    EXPECT_EQ(loaded.token, session.token);
    EXPECT_EQ(loaded.expiresAt, session.expiresAt);
    EXPECT_TRUE(loaded.email.isEmpty());
}
