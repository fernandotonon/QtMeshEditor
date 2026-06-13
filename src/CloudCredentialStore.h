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
 * Storage for QtMesh Cloud session secrets.
 *
 * Backed by QSettings (the per-user app preference store), the same as every
 * other setting. An earlier version used the OS secret store (Keychain /
 * Credential Manager / libsecret), but on macOS every Keychain read raised a
 * confirmation dialog and the session is read several times at startup, so the
 * user was prompted repeatedly on each launch. The token is a short-lived
 * cloud session bearer, so QSettings is an acceptable (non-prompting) home.
 * Non-secret profile fields (name, slug) also live in QSettings.
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
