#ifndef QTMESH_ID_H
#define QTMESH_ID_H

#include <QObject>
#include <QString>
#include <QUrl>

class QTimer;

class QWidget;

/**
 * @brief The QtMesh ID account as one shared, observable piece of state.
 *
 * QtMesh ID is the account used by QtMeshEditor, QtMesh Cloud and QtMesh
 * Games. Signing in is OPTIONAL: nothing local depends on it.
 *
 * This class owns no credentials of its own. The session lives in
 * CloudCredentialStore (as before), the profile fields in QSettings, and
 * sign-in is the existing browser device-code flow (POST /v1/oauth/device/code,
 * approve on qtmesh.dev, poll for the token). Every surface (the account
 * button, the Welcome dialog, the Welcome overlay, Cloud dialogs) goes through
 * here, so a sign-in or sign-out anywhere updates all of them via
 * sessionChanged().
 *
 * New accounts are created on the hosted QtMesh ID page during that same
 * sign-in (GitHub / email), so "Create a free account" starts it too.
 */
class QtMeshId : public QObject
{
    Q_OBJECT
public:
    enum class State {
        SignedOut,   ///< no session stored
        Connecting,  ///< a sign-in is in progress (duplicate attempts refused)
        SignedIn,    ///< a session is stored and not past its expiry
        Expired,     ///< a session is stored but past its expiry
    };
    Q_ENUM(State)

    static QtMeshId* instance();

    State state() const;
    /// Name, else slug, else email of the signed-in account ("" when signed out).
    QString displayName() const;
    /// One or two letters for the default avatar.
    QString initials() const;
    /// Stable string for QML: "signedOut" | "connecting" | "signedIn" | "expired".
    static QString stateId(State state);

    /// Runs the browser device-code sign-in. Blocks in a window-modal prompt
    /// (Cancel closes it) and returns true when a session was saved. Returns
    /// false at once if a sign-in is already running. Shows the reason when
    /// starting or completing the sign-in fails; a cancel shows nothing.
    bool signIn(QWidget* parent = nullptr);
    /// Revokes the session on the server (best effort) and clears it locally.
    void signOut();

    /// Account settings on qtmesh.dev.
    static QUrl manageAccountUrl();
    /// Opens manageAccountUrl() in the browser (with a breadcrumb).
    static bool openManageAccount(const QString& surface);

    /// A session whose expiry (epoch ms, 0 = unknown) has passed.
    static bool isExpired(qint64 expiresAtMs, qint64 nowMs);

    /// Re-reads the stored session and emits sessionChanged() if anything
    /// shown to the user changed. Call after code outside this class edits
    /// the credential store.
    void refresh();

signals:
    void sessionChanged();

private:
    explicit QtMeshId(QObject* parent = nullptr);
    void setConnecting(bool connecting);
    /// Arms m_expiryTimer for the stored session's expiry, so a screen left
    /// open switches to "expired" without anyone calling refresh().
    void scheduleExpiryCheck();

    bool m_connecting = false;
    QTimer* m_expiryTimer = nullptr;
    QString m_lastSnapshot;
};

#endif // QTMESH_ID_H
