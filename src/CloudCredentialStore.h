/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#ifndef CLOUD_CREDENTIAL_STORE_H
#define CLOUD_CREDENTIAL_STORE_H

#include <QString>

/** Bearer token and expiry persisted outside plaintext QSettings. */
struct CloudSession {
    QString token;
    qint64 expiresAt = 0;
    QString email;
    bool hasToken() const { return !token.isEmpty(); }
};

/**
 * OS-backed storage for QtMesh Cloud session secrets.
 *
 * Uses Keychain (macOS), Credential Manager (Windows), or libsecret (Linux) when
 * available; otherwise a mode-0600 file under the app config directory.
 * Non-secret profile fields (name, slug) remain in QSettings.
 */
class CloudCredentialStore {
public:
    static bool saveSession(const CloudSession& session);
    static CloudSession loadSession();
    static void clearSession();
    static bool hasSession();

    /** Moves legacy Cloud/token* values from QSettings into secure storage. */
    static void migrateLegacySettingsIfNeeded();

    /**
     * Drops the in-process secret cache so the next read hits the backing
     * store again. The cache collapses the 3-4 startup reads into a single OS
     * access (each macOS keychain query can raise a confirmation prompt).
     * Only needed by tests that mutate the backing file directly.
     */
    static void resetCacheForTesting();
};

#endif // CLOUD_CREDENTIAL_STORE_H
