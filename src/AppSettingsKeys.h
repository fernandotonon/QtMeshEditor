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

/** @brief Opaque random telemetry installation UUID. Never machine-derived. */
inline const QString& anonymousInstallationId()
{
    static const auto k(QStringLiteral("telemetry/anonymousInstallationId"));
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

/** @brief Epoch-ms timestamp of the last successful QtMesh Cloud upload on this machine. */
inline const QString& cloudLastUploadAt()
{
    static const QString k(QStringLiteral("Cloud/lastUploadAt"));
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

inline const QString& updaterChannel()
{
    static const QString k(QStringLiteral("Updater/channel"));
    return k;
}

inline const QString& updaterCheckOnStartup()
{
    static const QString k(QStringLiteral("Updater/checkOnStartup"));
    return k;
}

inline const QString& updaterAutoDownload()
{
    static const QString k(QStringLiteral("Updater/autoDownload"));
    return k;
}

inline const QString& updaterLastCheckedAt()
{
    static const QString k(QStringLiteral("Updater/lastCheckedAt"));
    return k;
}

inline const QString& updaterSkippedVersion()
{
    static const QString k(QStringLiteral("Updater/skippedVersion"));
    return k;
}

// ---- Gamification / progress sync (#796) ----

/** @brief One-time consent prompt answered (accept OR decline). Nothing is
 *  ever queued or sent before this is true (E-P6: default off). */
inline const QString& gamificationConsentAcknowledged()
{
    static const QString k(QStringLiteral("Gamification/consentAcknowledged"));
    return k;
}

/** @brief Whether the one-time consent prompt has been shown. */
inline const QString& gamificationConsentPrompted()
{
    static const QString k(QStringLiteral("Gamification/consentPrompted"));
    return k;
}

/** @brief Master "Sync my QtMesh progress" toggle (default off). */
inline const QString& gamificationSyncEnabled()
{
    static const QString k(QStringLiteral("Gamification/syncEnabled"));
    return k;
}

/** @brief Sub-toggle: feature-usage (discovery) events. */
inline const QString& gamificationUsageEnabled()
{
    static const QString k(QStringLiteral("Gamification/usageEnabled"));
    return k;
}

/** @brief Sub-toggle: operations-history (before/after metrics) events. */
inline const QString& gamificationOpsEnabled()
{
    static const QString k(QStringLiteral("Gamification/opsEnabled"));
    return k;
}

/** @brief "Try this next" welcome-screen nudges on/off (E-P5). */
inline const QString& gamificationNudgesEnabled()
{
    static const QString k(QStringLiteral("Gamification/nudgesEnabled"));
    return k;
}

/** @brief Feature keys the user dismissed from the nudge card. */
inline const QString& gamificationDismissedSuggestions()
{
    static const QString k(QStringLiteral("Gamification/dismissedSuggestions"));
    return k;
}

/** @brief Rotation cursor so the nudge card cycles between app runs. */
inline const QString& gamificationSuggestionCursor()
{
    static const QString k(QStringLiteral("Gamification/suggestionCursor"));
    return k;
}

/** @brief Default preset rig for new empty scenes (Slice E #487). */
inline const QString& lightingDefaultRig()
{
    static const QString k(QStringLiteral("Lighting/defaultRig"));
    return k;
}

/** @brief Viewport background mode: 0=solid, 1=gradient, 2=HDR skybox. */
inline const QString& lightingBackgroundMode()
{
    static const QString k(QStringLiteral("Lighting/backgroundMode"));
    return k;
}

inline const QString& lightingSolidBackground()
{
    static const QString k(QStringLiteral("Lighting/solidBackground"));
    return k;
}

inline const QString& lightingGradientTop()
{
    static const QString k(QStringLiteral("Lighting/gradientTop"));
    return k;
}

inline const QString& lightingGradientBottom()
{
    static const QString k(QStringLiteral("Lighting/gradientBottom"));
    return k;
}

/** @brief Global shadow quality preset (Slice F #488): 0=Off, 1=Low, 2=Medium, 3=High. */
inline const QString& shadowQualityPreset()
{
    static const QString k(QStringLiteral("Lighting/shadowQualityPreset"));
    return k;
}

inline const QString& shadowCascadeCount()
{
    static const QString k(QStringLiteral("Lighting/shadowCascadeCount"));
    return k;
}

inline const QString& shadowSplitLambda()
{
    static const QString k(QStringLiteral("Lighting/shadowSplitLambda"));
    return k;
}

inline const QString& shadowSpotResolution()
{
    static const QString k(QStringLiteral("Lighting/shadowSpotResolution"));
    return k;
}

/** @brief Last-selected paint gradient ramp name (Paint v2 Slice A / #544). */
inline const QString& paintGradientRampName()
{
    static const QString k(QStringLiteral("Paint/gradientRampName"));
    return k;
}

/** @brief One-time migration: solid brush default for Paint v2 (#544). */
inline const QString& paintColorSourceSolidDefaultApplied()
{
    static const QString k(QStringLiteral("Paint/v544SolidDefaultApplied"));
    return k;
}

/** @brief Paint brush colour source: 0=solid, 1=gradient. */
inline const QString& paintColorSource()
{
    static const QString k(QStringLiteral("Paint/colorSource"));
    return k;
}

/** @brief Paint gradient mode: 0=linear, 1=radial, 2=angular. */
inline const QString& paintGradientMode()
{
    static const QString k(QStringLiteral("Paint/gradientMode"));
    return k;
}

/** @brief Paint footprint type (Paint v2 Slice B / #545). */
inline const QString& paintFootprintType()
{
    static const QString k(QStringLiteral("Paint/footprintType"));
    return k;
}

inline const QString& paintActiveStampName()
{
    static const QString k(QStringLiteral("Paint/activeStampName"));
    return k;
}

inline const QString& paintActiveTilingName()
{
    static const QString k(QStringLiteral("Paint/activeTilingName"));
    return k;
}

inline const QString& paintStampSpacing()
{
    static const QString k(QStringLiteral("Paint/stampSpacing"));
    return k;
}

inline const QString& paintStampScatter()
{
    static const QString k(QStringLiteral("Paint/stampScatter"));
    return k;
}

inline const QString& paintStampSizeJitter()
{
    static const QString k(QStringLiteral("Paint/stampSizeJitter"));
    return k;
}

inline const QString& paintStampOpacityJitter()
{
    static const QString k(QStringLiteral("Paint/stampOpacityJitter"));
    return k;
}

inline const QString& paintStampRotation()
{
    static const QString k(QStringLiteral("Paint/stampRotation"));
    return k;
}

inline const QString& paintStampFixedAngle()
{
    static const QString k(QStringLiteral("Paint/stampFixedAngle"));
    return k;
}

inline const QString& paintTilingScale()
{
    static const QString k(QStringLiteral("Paint/tilingScale"));
    return k;
}

inline const QString& paintTilingRotation()
{
    static const QString k(QStringLiteral("Paint/tilingRotation"));
    return k;
}

inline const QString& paintTilingOffsetU()
{
    static const QString k(QStringLiteral("Paint/tilingOffsetU"));
    return k;
}

inline const QString& paintTilingOffsetV()
{
    static const QString k(QStringLiteral("Paint/tilingOffsetV"));
    return k;
}

} // namespace AppSettingsKeys

#endif // APP_SETTINGS_KEYS_H
