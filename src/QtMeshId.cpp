#include "QtMeshId.h"

#include "AppSettingsKeys.h"
#include "CloudAccountMenuButton.h"
#include "CloudCredentialStore.h"
#include "QtMeshCloudClient.h"
#include "SentryReporter.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

QtMeshId* QtMeshId::instance()
{
    static QtMeshId* self = new QtMeshId(qApp);
    return self;
}

QtMeshId::QtMeshId(QObject* parent)
    : QObject(parent)
{
    m_lastSnapshot = stateId(state()) + QLatin1Char('|') + displayName();
}

bool QtMeshId::isExpired(qint64 expiresAtMs, qint64 nowMs)
{
    return expiresAtMs > 0 && expiresAtMs <= nowMs;
}

QtMeshId::State QtMeshId::state() const
{
    if (m_connecting)
        return State::Connecting;
    const CloudSession session = CloudCredentialStore::loadSession();
    if (!session.hasToken())
        return State::SignedOut;
    if (isExpired(session.expiresAt, QDateTime::currentMSecsSinceEpoch()))
        return State::Expired;
    return State::SignedIn;
}

QString QtMeshId::stateId(State state)
{
    switch (state) {
    case State::Connecting: return QStringLiteral("connecting");
    case State::SignedIn:   return QStringLiteral("signedIn");
    case State::Expired:    return QStringLiteral("expired");
    case State::SignedOut:  break;
    }
    return QStringLiteral("signedOut");
}

QString QtMeshId::displayName() const
{
    if (!CloudCredentialStore::hasSession())
        return QString();
    QSettings settings;
    QString display = settings.value(AppSettingsKeys::cloudUserName()).toString().trimmed();
    if (display.isEmpty())
        display = settings.value(AppSettingsKeys::cloudUserSlug()).toString().trimmed();
    if (display.isEmpty())
        display = CloudCredentialStore::loadSession().email.trimmed();
    return display;
}

QString QtMeshId::initials() const
{
    return CloudAccountMenuButton::initialsFromDisplayName(displayName());
}

QUrl QtMeshId::manageAccountUrl()
{
    return QUrl(QStringLiteral("https://qtmesh.dev/#/settings"));
}

bool QtMeshId::openManageAccount(const QString& surface)
{
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        QStringLiteral("QtMesh ID: manage account (%1)").arg(surface));
    return QDesktopServices::openUrl(manageAccountUrl());
}

void QtMeshId::refresh()
{
    const QString snapshot = stateId(state()) + QLatin1Char('|') + displayName();
    if (snapshot == m_lastSnapshot)
        return;
    m_lastSnapshot = snapshot;
    emit sessionChanged();
}

void QtMeshId::setConnecting(bool connecting)
{
    m_connecting = connecting;
    refresh();
}

bool QtMeshId::signIn(QWidget* parent)
{
    if (m_connecting)
        return false;
    if (!parent)
        parent = QApplication::activeWindow();

    SentryReporter::addBreadcrumb(QStringLiteral("cloud.auth"),
                                  QStringLiteral("QtMesh ID sign-in started"));
    setConnecting(true);

    const auto code = QtMeshCloudClient::requestDeviceCode();
    if (!code.ok) {
        setConnecting(false);
        QMessageBox::warning(parent, tr("Sign in with QtMesh ID"),
                             tr("Could not start sign in.\n\n%1").arg(code.errorString));
        return false;
    }

    QDialog prompt(parent);
    prompt.setWindowTitle(tr("Sign in with QtMesh ID"));
    prompt.setWindowModality(Qt::WindowModal);
    auto* layout = new QVBoxLayout(&prompt);
    auto* label = new QLabel(
        tr("Enter this code on qtmesh.dev, or click Open Browser.\n"
           "New here? You can create your free account on the same page.\n\n"
           "Code: %1")
            .arg(code.userCode),
        &prompt);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(label);

    auto* statusLabel = new QLabel(tr("Waiting for approval…"), &prompt);
    statusLabel->setWordWrap(true);
    layout->addWidget(statusLabel);

    auto* buttons = new QHBoxLayout();
    auto* copyButton = new QPushButton(tr("Copy Code"), &prompt);
    auto* openButton = new QPushButton(tr("Open Browser"), &prompt);
    auto* cancelButton = new QPushButton(tr("Cancel"), &prompt);
    buttons->addWidget(copyButton);
    buttons->addStretch();
    buttons->addWidget(cancelButton);
    buttons->addWidget(openButton);
    layout->addLayout(buttons);

    QTimer pollTimer(&prompt);
    bool signedIn = false;
    int intervalMs = qMax(1, code.intervalSeconds) * 1000;
    const int maxAttempts = qMax(1, code.expiresInSeconds / qMax(1, code.intervalSeconds) + 2);
    int attempts = 0;

    const auto applyToken = [&](const QtMeshCloudClient::DeviceTokenResult& token) -> bool {
        CloudSession session;
        session.token = token.token;
        session.expiresAt = token.expiresAt;
        session.email = token.user.value(QStringLiteral("email")).toString();
        if (!CloudCredentialStore::saveSession(session)) {
            QMessageBox::warning(&prompt, tr("Sign in with QtMesh ID"),
                                 tr("Signed in, but the session could not be saved securely."));
            return false;
        }
        QSettings settings;
        settings.setValue(AppSettingsKeys::cloudUserName(),
                          token.user.value(QStringLiteral("name")).toString());
        settings.setValue(AppSettingsKeys::cloudUserSlug(),
                          token.user.value(QStringLiteral("slug")).toString());
        // NB: do NOT remove the cloudToken / cloudTokenExpiresAt / cloudUserEmail
        // keys here. CloudCredentialStore persists the session in exactly those
        // QSettings keys, so removing them would log the user out next launch.
        settings.sync();
        return true;
    };

    const auto failSignIn = [&](const QString& message) {
        pollTimer.stop();
        if (!prompt.isVisible())
            return;
        QMessageBox::warning(&prompt, tr("Sign in with QtMesh ID"), message);
        prompt.reject();
    };

    const auto pollOnce = [&]() {
        if (attempts >= maxAttempts) {
            failSignIn(tr("The sign-in request expired. Start sign in again."));
            return;
        }
        ++attempts;

        const auto token = QtMeshCloudClient::pollDeviceToken(code.deviceCode);
        if (token.ok) {
            pollTimer.stop();
            if (applyToken(token)) {
                signedIn = true;
                prompt.accept();
            } else {
                prompt.reject();
            }
            return;
        }

        if (token.errorCode == QStringLiteral("authorization_pending")) {
            statusLabel->setText(tr("Waiting for approval…"));
            return;
        }
        if (token.errorCode == QStringLiteral("slow_down")) {
            intervalMs = qMax(intervalMs + 2000, qMax(1, token.intervalSeconds) * 1000);
            pollTimer.setInterval(intervalMs);
            statusLabel->setText(tr("Waiting for approval…"));
            return;
        }
        if (token.errorCode == QStringLiteral("access_denied")) {
            failSignIn(tr("Sign in was denied in the browser."));
            return;
        }
        if (token.errorCode == QStringLiteral("expired_token")) {
            failSignIn(tr("The sign-in code expired. Start sign in again."));
            return;
        }
        failSignIn(tr("Sign in failed.\n\n%1").arg(token.errorString));
    };

    connect(copyButton, &QPushButton::clicked, &prompt, [userCode = code.userCode]() {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("QtMesh ID sign-in: Copy Code"));
        if (QApplication::clipboard())
            QApplication::clipboard()->setText(userCode);
    });
    connect(cancelButton, &QPushButton::clicked, &prompt, [&pollTimer, &prompt]() {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("QtMesh ID sign-in: Cancel"));
        pollTimer.stop();
        prompt.reject();
    });
    connect(openButton, &QPushButton::clicked, &prompt,
            [&prompt, statusLabel, verificationUri = code.verificationUriComplete]() {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                      QStringLiteral("QtMesh ID sign-in: Open Browser"));
        if (!QDesktopServices::openUrl(QUrl(verificationUri))) {
            SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                          QStringLiteral("QtMesh ID sign-in: Open Browser failed"));
            QMessageBox::warning(&prompt, tr("QtMesh ID"),
                                 tr("Could not open qtmesh.dev in your browser."));
            return;
        }
        statusLabel->setText(tr("Complete sign-in in your browser…"));
    });
    connect(&pollTimer, &QTimer::timeout, &prompt, pollOnce);

    openButton->setDefault(true);
    prompt.resize(440, prompt.sizeHint().height());

    pollTimer.start(intervalMs);
    QTimer::singleShot(0, &prompt, pollOnce);
    prompt.exec();
    pollTimer.stop();

    setConnecting(false);
    SentryReporter::addBreadcrumb(QStringLiteral("cloud.auth"),
        signedIn ? QStringLiteral("QtMesh ID sign-in completed")
                 : QStringLiteral("QtMesh ID sign-in not completed"));
    return signedIn;
}

void QtMeshId::signOut()
{
    const QString token = CloudCredentialStore::loadSession().token;
    if (!token.isEmpty())
        QtMeshCloudClient::logout(token, /*timeoutMs=*/10000);

    CloudCredentialStore::clearSession();

    QSettings settings;
    settings.remove(AppSettingsKeys::cloudToken());
    settings.remove(AppSettingsKeys::cloudTokenExpiresAt());
    settings.remove(AppSettingsKeys::cloudUserName());
    settings.remove(AppSettingsKeys::cloudUserEmail());
    settings.remove(AppSettingsKeys::cloudUserSlug());
    settings.sync();
    SentryReporter::addBreadcrumb(QStringLiteral("cloud.auth"),
                                  QStringLiteral("QtMesh ID signed out"));
    refresh();
}
