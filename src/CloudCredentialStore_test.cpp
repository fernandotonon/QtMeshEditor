#include "CloudCredentialStore.h"
#include "AppSettingsKeys.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
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

    static QString sessionFilePath()
    {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        return dir + QStringLiteral("/cloud_session.dat");
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

TEST_F(CloudCredentialStoreTest, LoadSessionReturnsEmptyForCorruptFile)
{
    const QString path = sessionFilePath();
    ASSERT_FALSE(path.isEmpty());

    const QFileInfo info(path);
    if (QDir dir = info.dir(); !dir.exists())
        ASSERT_TRUE(dir.mkpath(QStringLiteral(".")));

    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(file.write("not-json"), 8);
    file.close();

    // We wrote the backing file directly, behind the in-process cache; drop it
    // so the read actually parses the corrupt file rather than returning the
    // cached "no session" state SetUp() primed via clearSession().
    CloudCredentialStore::resetCacheForTesting();

    const CloudSession loaded = CloudCredentialStore::loadSession();
    EXPECT_FALSE(loaded.hasToken());
    EXPECT_TRUE(loaded.email.isEmpty());
}

TEST_F(CloudCredentialStoreTest, CacheServesReadsWithoutHittingBackingStore)
{
    CloudSession session;
    session.token = QStringLiteral("cached-token");
    session.expiresAt = 7;
    ASSERT_TRUE(CloudCredentialStore::saveSession(session));

    // Delete the backing file out from under the store. Because saveSession()
    // primed the in-process cache, subsequent reads must still succeed without
    // touching the (now-missing) backing store — this is exactly what stops the
    // repeated macOS keychain prompts at startup.
    QFile::remove(sessionFilePath());

    const CloudSession cached = CloudCredentialStore::loadSession();
    EXPECT_EQ(cached.token, QStringLiteral("cached-token"));
    EXPECT_TRUE(CloudCredentialStore::hasSession());

    // After an explicit cache reset the read falls through to the (gone) file.
    CloudCredentialStore::resetCacheForTesting();
    EXPECT_FALSE(CloudCredentialStore::hasSession());
}

TEST_F(CloudCredentialStoreTest, MigrateNoOpWhenNoLegacyToken)
{
    QSettings settings;
    settings.setValue(AppSettingsKeys::cloudUserName(), QStringLiteral("still-here"));
    settings.sync();

    CloudCredentialStore::migrateLegacySettingsIfNeeded();

    EXPECT_FALSE(CloudCredentialStore::hasSession());
    EXPECT_EQ(settings.value(AppSettingsKeys::cloudUserName()).toString(),
              QStringLiteral("still-here"));
}

TEST_F(CloudCredentialStoreTest, MigratesLegacyPlaintextSettings)
{
    QSettings settings;
    settings.setValue(AppSettingsKeys::cloudToken(), QStringLiteral("legacy-token"));
    settings.setValue(AppSettingsKeys::cloudTokenExpiresAt(), 42);
    settings.setValue(AppSettingsKeys::cloudUserEmail(), QStringLiteral("legacy@example.com"));
    settings.sync();

    CloudCredentialStore::migrateLegacySettingsIfNeeded();

    const CloudSession loaded = CloudCredentialStore::loadSession();
    EXPECT_EQ(loaded.token, QStringLiteral("legacy-token"));
    EXPECT_EQ(loaded.expiresAt, 42);
    EXPECT_EQ(loaded.email, QStringLiteral("legacy@example.com"));
    EXPECT_TRUE(settings.value(AppSettingsKeys::cloudToken()).toString().isEmpty());
    EXPECT_TRUE(settings.value(AppSettingsKeys::cloudTokenExpiresAt()).toString().isEmpty());
    EXPECT_TRUE(settings.value(AppSettingsKeys::cloudUserEmail()).toString().isEmpty());
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
