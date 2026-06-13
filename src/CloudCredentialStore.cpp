#include "CloudCredentialStore.h"
#include "AppSettingsKeys.h"

#include <QSettings>

#include <optional>

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
    session.expiresAt = settings.value(AppSettingsKeys::cloudTokenExpiresAt()).toLongLong();
    session.email = settings.value(AppSettingsKeys::cloudUserEmail()).toString();
    return session;
}

void writeToSettings(const CloudSession& session)
{
    QSettings settings;
    settings.setValue(AppSettingsKeys::cloudToken(), session.token);
    settings.setValue(AppSettingsKeys::cloudTokenExpiresAt(), session.expiresAt);
    if (session.email.isEmpty())
        settings.remove(AppSettingsKeys::cloudUserEmail());
    else
        settings.setValue(AppSettingsKeys::cloudUserEmail(), session.email);
    settings.sync();
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
    writeToSettings(session);
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
    // Tokens already live in QSettings under the same keys, so there is nothing
    // to migrate. Retained as a no-op so existing callers (the account control's
    // refresh) keep compiling, and as the hook point should the storage backend
    // change again.
}
