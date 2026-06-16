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

#ifndef APP_SETTINGS_KEYS_H
#define APP_SETTINGS_KEYS_H

#include <QString>

namespace AppSettingsKeys
{

/** @brief Sentry on/off in Preferences (must match QML and SentryReporter). */
inline const QString& sentryEnabled()
{
    static const QString k(QStringLiteral("Sentry/enabled"));
    return k;
}

/** @brief Telemetry on/off. */
inline const QString& telemetryEnabled()
{
    static const QString k(QStringLiteral("Telemetry/enabled"));
    return k;
}

/** @brief Light/dark/system from Preferences. */
inline const QString& appearanceTheme()
{
    static const QString k(QStringLiteral("Appearance/theme"));
    return k;
}

/**
 * @brief Legacy / alternate key for theme (some paths write "palette").
 */
inline const QString& palette()
{
    static const QString k(QStringLiteral("palette"));
    return k;
}

/** @brief Legacy QSettings key — use CloudCredentialStore; migrated on startup. */
inline const QString& cloudToken()
{
    static const QString k(QStringLiteral("Cloud/token"));
    return k;
}

/** @brief Legacy QSettings key — use CloudCredentialStore; migrated on startup. */
inline const QString& cloudTokenExpiresAt()
{
    static const QString k(QStringLiteral("Cloud/tokenExpiresAt"));
    return k;
}

inline const QString& cloudUserName()
{
    static const QString k(QStringLiteral("Cloud/userName"));
    return k;
}

/** @brief Display-only email; stored in CloudCredentialStore with the bearer token. */
inline const QString& cloudUserEmail()
{
    static const QString k(QStringLiteral("Cloud/userEmail"));
    return k;
}

inline const QString& cloudUserSlug()
{
    static const QString k(QStringLiteral("Cloud/userSlug"));
    return k;
}

/** @brief Set once the one-time pre-QSettings secret-store migration has been
 *  attempted, so the OS keychain is never probed again (which would prompt on
 *  every launch). */
inline const QString& cloudLegacyMigrationDone()
{
    static const QString k(QStringLiteral("Cloud/legacyMigrationDone"));
    return k;
}

/** @brief Last platform profile id used for Validation-mode asset folder scan (issue #370). */
inline const QString& validationPlatformProfileId()
{
    static const QString k(QStringLiteral("Validation/platformProfileId"));
    return k;
}

/** @brief Bumped when Validation profile picker defaults or migration rules change. */
inline const QString& validationPlatformProfilePickerVersion()
{
    static const QString k(QStringLiteral("Validation/platformProfilePickerVersion"));
    return k;
}

} // namespace AppSettingsKeys

#endif // APP_SETTINGS_KEYS_H
