#include "CloudCredentialStore.h"
#include "AppSettingsKeys.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSettings>

TEST(CloudCredentialStoreTest, RoundTripAndClear)
{
    CloudCredentialStore::clearSession();

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

TEST(CloudCredentialStoreTest, MigratesLegacyPlaintextSettings)
{
    CloudCredentialStore::clearSession();

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
    EXPECT_TRUE(settings.value(AppSettingsKeys::cloudUserEmail()).toString().isEmpty());

    CloudCredentialStore::clearSession();
}
