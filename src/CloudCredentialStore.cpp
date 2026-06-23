#include "CloudCredentialStore.h"
#include "AppSettingsKeys.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>

#include <optional>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincred.h>
#endif

#ifdef Q_OS_MACOS
#include <Security/Security.h>
#endif

// Session secrets are stored in QSettings (the same per-user backing store used
// for every other app preference). An earlier implementation kept the bearer
// token in the OS secret store (macOS Keychain / Windows Credential Manager /
// libsecret); on macOS every Keychain read raised a confirmation dialog, and
// the account control reads the session several times at startup — so the user
// was prompted repeatedly on every launch. QSettings does not prompt. The token
// is a short-lived cloud session bearer, not a high-value credential, so the
// platform-preference store is an acceptable home for it.

namespace {

// In-process cache so the account control's 3-4 startup reads collapse into a
// single backing-store read. Invalidated on save/clear so it never goes stale.
std::optional<CloudSession> g_sessionCache;
bool g_cacheValid = false;

void invalidateCache()
{
    g_sessionCache.reset();
    g_cacheValid = false;
}

CloudSession readFromSettings()
{
    QSettings settings;
    CloudSession session;
    session.token = settings.value(AppSettingsKeys::cloudToken()).toString();
    // No token → no session. Don't hydrate email/expiry from stray keys, or
    // loadSession() would return a partial session and surface stale identity
    // data in UI fallbacks that read loadSession().email.
    if (session.token.isEmpty())
        return {};
    session.expiresAt = settings.value(AppSettingsKeys::cloudTokenExpiresAt()).toLongLong();
    session.email = settings.value(AppSettingsKeys::cloudUserEmail()).toString();
    return session;
}

// Returns false if QSettings reported a write/sync error so callers can tell
// the user the login wasn't persisted instead of silently dropping it.
bool writeToSettings(const CloudSession& session)
{
    QSettings settings;
    settings.setValue(AppSettingsKeys::cloudToken(), session.token);
    settings.setValue(AppSettingsKeys::cloudTokenExpiresAt(), session.expiresAt);
    if (session.email.isEmpty())
        settings.remove(AppSettingsKeys::cloudUserEmail());
    else
        settings.setValue(AppSettingsKeys::cloudUserEmail(), session.email);
    settings.sync();
    return settings.status() == QSettings::NoError;
}

// One-shot read of the OLD OS secret store (Keychain / Credential Manager /
// libsecret) and the legacy mode-0600 fallback file, used only to migrate a
// user who signed in on a pre-QSettings build. Returns an empty session when
// nothing is found. The libsecret backend is intentionally NOT read back here:
// it required linking libsecret (now removed) and Linux desktop secret daemons
// don't prompt the way macOS Keychain does, so the cost of dropping that narrow
// upgrade path is low compared to re-introducing the dependency.
CloudSession readLegacySecretStore()
{
    QByteArray payload;

#if defined(Q_OS_MACOS)
    const QByteArray service = QByteArrayLiteral("QtMeshEditor");
    const QByteArray account = QByteArrayLiteral("QtMeshCloud");
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(dict, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(dict, kSecAttrService,
                         CFStringCreateWithCString(nullptr, service.constData(), kCFStringEncodingUTF8));
    CFDictionarySetValue(dict, kSecAttrAccount,
                         CFStringCreateWithCString(nullptr, account.constData(), kCFStringEncodingUTF8));
    CFDictionarySetValue(dict, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(dict, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef result = nullptr;
    const OSStatus status = SecItemCopyMatching(dict, &result);
    CFRelease(dict);
    if (status == errSecSuccess && result) {
        CFDataRef data = reinterpret_cast<CFDataRef>(result);
        payload = QByteArray(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                             static_cast<int>(CFDataGetLength(data)));
        CFRelease(result);
    }
#elif defined(Q_OS_WIN)
    PCREDENTIALW cred = nullptr;
    if (CredReadW(L"QtMeshEditor/QtMeshCloud", CRED_TYPE_GENERIC, 0, &cred) && cred) {
        payload = QByteArray(reinterpret_cast<const char*>(cred->CredentialBlob),
                             static_cast<int>(cred->CredentialBlobSize));
        CredFree(cred);
    }
#endif

    // Legacy mode-0600 fallback file (used when no OS store was available).
    if (payload.isEmpty()) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        QFile file(dir + QStringLiteral("/cloud_session.dat"));
        if (file.open(QIODevice::ReadOnly))
            payload = file.readAll();
    }

    CloudSession session;
    if (payload.isEmpty())
        return session;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return session;
    const QJsonObject obj = doc.object();
    session.token = obj.value(QStringLiteral("token")).toString();
    session.expiresAt = static_cast<qint64>(obj.value(QStringLiteral("expiresAt")).toDouble(0));
    session.email = obj.value(QStringLiteral("email")).toString();
    return session;
}

void eraseFromSettings()
{
    QSettings settings;
    settings.remove(AppSettingsKeys::cloudToken());
    settings.remove(AppSettingsKeys::cloudTokenExpiresAt());
    settings.remove(AppSettingsKeys::cloudUserEmail());
    settings.sync();
}

} // namespace

bool CloudCredentialStore::saveSession(const CloudSession& session)
{
    if (!session.hasToken())
        return false;
    if (!writeToSettings(session)) {
        // The write didn't persist (permissions / disk). Don't claim success or
        // prime the cache, so the caller can surface the failure.
        invalidateCache();
        return false;
    }
    g_sessionCache = session;
    g_cacheValid = true;
    return true;
}

CloudSession CloudCredentialStore::loadSession()
{
    if (g_cacheValid)
        return g_sessionCache.value_or(CloudSession());

    CloudSession session = readFromSettings();
    g_sessionCache = session;
    g_cacheValid = true;
    return session;
}

void CloudCredentialStore::clearSession()
{
    eraseFromSettings();
    g_sessionCache = CloudSession();
    g_cacheValid = true;
}

bool CloudCredentialStore::hasSession()
{
    return loadSession().hasToken();
}

void CloudCredentialStore::resetCacheForTesting()
{
    invalidateCache();
}

void CloudCredentialStore::migrateLegacySettingsIfNeeded()
{
    // The OS secret store must be probed AT MOST ONCE, ever. Each probe is a
    // SecItemCopyMatching (macOS Keychain) / CredRead (Windows) that can raise a
    // confirmation prompt — re-probing on every launch (which happens when the
    // user is signed out, since QSettings then has no token) is exactly the
    // repeated-keychain-prompt bug we set out to kill. Persist a "done" flag the
    // first time through and never touch the keychain again, signed in or not.
    {
        QSettings settings;
        if (settings.value(AppSettingsKeys::cloudLegacyMigrationDone(), false).toBool())
            return;
    }

    // If a session already lives in QSettings there is nothing to migrate, but
    // still mark the probe done so we don't keychain-probe on a later sign-out.
    if (!readFromSettings().hasToken()) {
        // One-time upgrade: a user who signed in on a pre-QSettings build has
        // their token in the OS secret store / legacy fallback file. Carry it
        // into QSettings so they stay signed in across the backend change.
        const CloudSession legacy = readLegacySecretStore();
        if (legacy.hasToken() && writeToSettings(legacy)) {
            g_sessionCache = legacy;
            g_cacheValid = true;
        }
    }

    QSettings settings;
    settings.setValue(AppSettingsKeys::cloudLegacyMigrationDone(), true);
    settings.sync();
}

void CloudCredentialStore::setLastUploadAt(const qint64 epochMs)
{
    QSettings settings;
    settings.setValue(AppSettingsKeys::cloudLastUploadAt(), epochMs);
    settings.sync();
}

qint64 CloudCredentialStore::lastUploadAt()
{
    QSettings settings;
    return settings.value(AppSettingsKeys::cloudLastUploadAt()).toLongLong();
}
