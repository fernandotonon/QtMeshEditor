#include "CloudCredentialStore.h"
#include "AppSettingsKeys.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
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

    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(file.write("not-json"), 8);
    file.close();

    const CloudSession loaded = CloudCredentialStore::loadSession();
    EXPECT_FALSE(loaded.hasToken());
    EXPECT_TRUE(loaded.email.isEmpty());
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
