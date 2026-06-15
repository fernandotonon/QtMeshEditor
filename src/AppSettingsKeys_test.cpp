/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software
without restriction, including without limitation the rights to use, copy,
modify, merge, publish, distribute, sublicense, and/or sell copies of the
Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING WITHOUT LIMITATION THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.
-----------------------------------------------------------------------------------
*/

// Unit tests for the AppSettingsKeys namespace.
//
// AppSettingsKeys is a header-only namespace of inline accessors that each
// return a `const QString&` to a function-local static. These string literals
// are LOAD-BEARING: they must byte-for-byte match the keys used by QML,
// SentryReporter, CloudCredentialStore migration, and QSettings persistence.
// A silent regression in any of them would break settings persistence without
// any compile error, so we pin every literal exactly and also assert the
// reference-stability contract (each accessor returns the same object on
// repeated calls — the static-local singleton guarantee).
//
// Pure-data / pure-logic: no Ogre, no display, no QApplication required.

#include <gtest/gtest.h>

#include <QString>
#include <QSet>
#include <QStringList>

#include "AppSettingsKeys.h"

namespace
{

// ---------------------------------------------------------------------------
// Exact-literal contract: each accessor must return precisely the documented
// QSettings key. These are compared as QString so a trailing-space / casing
// regression is caught.
// ---------------------------------------------------------------------------

TEST(AppSettingsKeysCoverageTest, SentryEnabledLiteral)
{
    EXPECT_EQ(AppSettingsKeys::sentryEnabled(), QStringLiteral("Sentry/enabled"));
}

TEST(AppSettingsKeysCoverageTest, TelemetryEnabledLiteral)
{
    EXPECT_EQ(AppSettingsKeys::telemetryEnabled(), QStringLiteral("Telemetry/enabled"));
}

TEST(AppSettingsKeysCoverageTest, AppearanceThemeLiteral)
{
    EXPECT_EQ(AppSettingsKeys::appearanceTheme(), QStringLiteral("Appearance/theme"));
}

TEST(AppSettingsKeysCoverageTest, PaletteLiteral)
{
    EXPECT_EQ(AppSettingsKeys::palette(), QStringLiteral("palette"));
}

TEST(AppSettingsKeysCoverageTest, CloudTokenLiteral)
{
    EXPECT_EQ(AppSettingsKeys::cloudToken(), QStringLiteral("Cloud/token"));
}

TEST(AppSettingsKeysCoverageTest, CloudTokenExpiresAtLiteral)
{
    EXPECT_EQ(AppSettingsKeys::cloudTokenExpiresAt(), QStringLiteral("Cloud/tokenExpiresAt"));
}

TEST(AppSettingsKeysCoverageTest, CloudUserNameLiteral)
{
    EXPECT_EQ(AppSettingsKeys::cloudUserName(), QStringLiteral("Cloud/userName"));
}

TEST(AppSettingsKeysCoverageTest, CloudUserEmailLiteral)
{
    EXPECT_EQ(AppSettingsKeys::cloudUserEmail(), QStringLiteral("Cloud/userEmail"));
}

TEST(AppSettingsKeysCoverageTest, CloudUserSlugLiteral)
{
    EXPECT_EQ(AppSettingsKeys::cloudUserSlug(), QStringLiteral("Cloud/userSlug"));
}

TEST(AppSettingsKeysCoverageTest, ValidationPlatformProfileIdLiteral)
{
    EXPECT_EQ(AppSettingsKeys::validationPlatformProfileId(),
              QStringLiteral("Validation/platformProfileId"));
}

TEST(AppSettingsKeysCoverageTest, ValidationPlatformProfilePickerVersionLiteral)
{
    EXPECT_EQ(AppSettingsKeys::validationPlatformProfilePickerVersion(),
              QStringLiteral("Validation/platformProfilePickerVersion"));
}

// ---------------------------------------------------------------------------
// Reference-stability contract: each accessor returns a reference to a
// function-local static, so repeated calls must return the SAME object
// (same address). This is what lets callers compare keys by identity and
// guarantees no per-call allocation.
// ---------------------------------------------------------------------------

TEST(AppSettingsKeysCoverageTest, SentryEnabledReturnsSameReference)
{
    const QString& a = AppSettingsKeys::sentryEnabled();
    const QString& b = AppSettingsKeys::sentryEnabled();
    EXPECT_EQ(&a, &b);
}

TEST(AppSettingsKeysCoverageTest, TelemetryEnabledReturnsSameReference)
{
    EXPECT_EQ(&AppSettingsKeys::telemetryEnabled(), &AppSettingsKeys::telemetryEnabled());
}

TEST(AppSettingsKeysCoverageTest, AppearanceThemeReturnsSameReference)
{
    EXPECT_EQ(&AppSettingsKeys::appearanceTheme(), &AppSettingsKeys::appearanceTheme());
}

TEST(AppSettingsKeysCoverageTest, PaletteReturnsSameReference)
{
    EXPECT_EQ(&AppSettingsKeys::palette(), &AppSettingsKeys::palette());
}

TEST(AppSettingsKeysCoverageTest, CloudTokenReturnsSameReference)
{
    EXPECT_EQ(&AppSettingsKeys::cloudToken(), &AppSettingsKeys::cloudToken());
}

TEST(AppSettingsKeysCoverageTest, CloudTokenExpiresAtReturnsSameReference)
{
    EXPECT_EQ(&AppSettingsKeys::cloudTokenExpiresAt(), &AppSettingsKeys::cloudTokenExpiresAt());
}

TEST(AppSettingsKeysCoverageTest, CloudUserNameReturnsSameReference)
{
    EXPECT_EQ(&AppSettingsKeys::cloudUserName(), &AppSettingsKeys::cloudUserName());
}

TEST(AppSettingsKeysCoverageTest, CloudUserEmailReturnsSameReference)
{
    EXPECT_EQ(&AppSettingsKeys::cloudUserEmail(), &AppSettingsKeys::cloudUserEmail());
}

TEST(AppSettingsKeysCoverageTest, CloudUserSlugReturnsSameReference)
{
    EXPECT_EQ(&AppSettingsKeys::cloudUserSlug(), &AppSettingsKeys::cloudUserSlug());
}

TEST(AppSettingsKeysCoverageTest, ValidationPlatformProfileIdReturnsSameReference)
{
    EXPECT_EQ(&AppSettingsKeys::validationPlatformProfileId(),
              &AppSettingsKeys::validationPlatformProfileId());
}

TEST(AppSettingsKeysCoverageTest, ValidationPlatformProfilePickerVersionReturnsSameReference)
{
    EXPECT_EQ(&AppSettingsKeys::validationPlatformProfilePickerVersion(),
              &AppSettingsKeys::validationPlatformProfilePickerVersion());
}

// ---------------------------------------------------------------------------
// Structural invariants shared across all keys.
// ---------------------------------------------------------------------------

// Collect every accessor's value once for the structural sweep below.
static QStringList allKeys()
{
    return {
        AppSettingsKeys::sentryEnabled(),
        AppSettingsKeys::telemetryEnabled(),
        AppSettingsKeys::appearanceTheme(),
        AppSettingsKeys::palette(),
        AppSettingsKeys::cloudToken(),
        AppSettingsKeys::cloudTokenExpiresAt(),
        AppSettingsKeys::cloudUserName(),
        AppSettingsKeys::cloudUserEmail(),
        AppSettingsKeys::cloudUserSlug(),
        AppSettingsKeys::validationPlatformProfileId(),
        AppSettingsKeys::validationPlatformProfilePickerVersion(),
    };
}

TEST(AppSettingsKeysCoverageTest, NoKeyIsEmpty)
{
    for (const QString& k : allKeys())
        EXPECT_FALSE(k.isEmpty());
}

TEST(AppSettingsKeysCoverageTest, NoKeyHasLeadingOrTrailingWhitespace)
{
    for (const QString& k : allKeys())
        EXPECT_EQ(k, k.trimmed()) << "Key has stray whitespace: " << k.toStdString();
}

TEST(AppSettingsKeysCoverageTest, AllKeysAreUnique)
{
    const QStringList keys = allKeys();
    const QSet<QString> unique(keys.begin(), keys.end());
    EXPECT_EQ(unique.size(), keys.size())
        << "Duplicate QSettings key detected — keys must be distinct.";
}

TEST(AppSettingsKeysCoverageTest, CloudKeysShareCloudGroupPrefix)
{
    EXPECT_TRUE(AppSettingsKeys::cloudToken().startsWith(QStringLiteral("Cloud/")));
    EXPECT_TRUE(AppSettingsKeys::cloudTokenExpiresAt().startsWith(QStringLiteral("Cloud/")));
    EXPECT_TRUE(AppSettingsKeys::cloudUserName().startsWith(QStringLiteral("Cloud/")));
    EXPECT_TRUE(AppSettingsKeys::cloudUserEmail().startsWith(QStringLiteral("Cloud/")));
    EXPECT_TRUE(AppSettingsKeys::cloudUserSlug().startsWith(QStringLiteral("Cloud/")));
}

TEST(AppSettingsKeysCoverageTest, ValidationKeysShareValidationGroupPrefix)
{
    EXPECT_TRUE(AppSettingsKeys::validationPlatformProfileId()
                    .startsWith(QStringLiteral("Validation/")));
    EXPECT_TRUE(AppSettingsKeys::validationPlatformProfilePickerVersion()
                    .startsWith(QStringLiteral("Validation/")));
}

// The grouped keys use exactly one '/' separator (QSettings group/key form);
// the legacy "palette" key intentionally has none. Pin that distinction so a
// future rename to "Appearance/palette" is a deliberate, test-visible change.
TEST(AppSettingsKeysCoverageTest, PaletteIsUngroupedLegacyKey)
{
    EXPECT_FALSE(AppSettingsKeys::palette().contains(QLatin1Char('/')));
}

TEST(AppSettingsKeysCoverageTest, GroupedKeysHaveSingleSeparator)
{
    const QStringList grouped = {
        AppSettingsKeys::sentryEnabled(),
        AppSettingsKeys::telemetryEnabled(),
        AppSettingsKeys::appearanceTheme(),
        AppSettingsKeys::cloudToken(),
        AppSettingsKeys::cloudTokenExpiresAt(),
        AppSettingsKeys::cloudUserName(),
        AppSettingsKeys::cloudUserEmail(),
        AppSettingsKeys::cloudUserSlug(),
        AppSettingsKeys::validationPlatformProfileId(),
        AppSettingsKeys::validationPlatformProfilePickerVersion(),
    };
    for (const QString& k : grouped)
        EXPECT_EQ(k.count(QLatin1Char('/')), 1) << "Unexpected separator count in " << k.toStdString();
}

} // namespace
