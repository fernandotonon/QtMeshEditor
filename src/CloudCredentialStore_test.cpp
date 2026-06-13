#include "CloudCredentialStore.h"
#include "AppSettingsKeys.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSettings>

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
