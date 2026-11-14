#include "QtMeshId.h"
#include "QtMeshIdPanel.h"
#include "AppSettingsKeys.h"
#include "CloudCredentialStore.h"
#include "WelcomeDialog.h"
#include "WelcomeScreenController.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>

namespace {

// Each test runs against its own settings store: never the developer's real
// QtMesh ID session.
class QtMeshIdTest : public ::testing::Test {
protected:
    QString previousOrganizationName;
    QString previousApplicationName;

    void SetUp() override
    {
        previousOrganizationName = QCoreApplication::organizationName();
        previousApplicationName = QCoreApplication::applicationName();
        QCoreApplication::setOrganizationName(QStringLiteral("QtMeshEditorTests"));
        QCoreApplication::setApplicationName(QStringLiteral("QtMeshIdTest"));
        QSettings().clear();
        CloudCredentialStore::clearSession();
        QtMeshId::instance()->refresh();
    }

    void TearDown() override
    {
        CloudCredentialStore::clearSession();
        QSettings().clear();
        QtMeshId::instance()->refresh();
        QCoreApplication::setOrganizationName(previousOrganizationName);
        QCoreApplication::setApplicationName(previousApplicationName);
    }

    static void storeSession(qint64 expiresAt, const QString& name)
    {
        CloudSession session;
        session.token = QStringLiteral("secret-token-never-shown");
        session.expiresAt = expiresAt;
        session.email = QStringLiteral("player@example.com");
        ASSERT_TRUE(CloudCredentialStore::saveSession(session));
        QSettings().setValue(AppSettingsKeys::cloudUserName(), name);
        QtMeshId::instance()->refresh();
    }

    static QString panelText(const QWidget& root)
    {
        QStringList parts;
        for (auto* label : root.findChildren<QLabel*>())
            parts << label->text();
        for (auto* button : root.findChildren<QPushButton*>())
            parts << button->text();
        return parts.join(QLatin1Char('\n'));
    }
};

qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

} // namespace

TEST(QtMeshIdPure, ExpiryIsMillisecondsAndZeroMeansUnknown)
{
    EXPECT_FALSE(QtMeshId::isExpired(0, 1000));        // server sent no expiry
    EXPECT_FALSE(QtMeshId::isExpired(2000, 1000));
    EXPECT_TRUE(QtMeshId::isExpired(1000, 1000));
    EXPECT_TRUE(QtMeshId::isExpired(999, 1000));
}

TEST(QtMeshIdPure, StateIdsAreStableForQml)
{
    EXPECT_EQ(QtMeshId::stateId(QtMeshId::State::SignedOut), "signedOut");
    EXPECT_EQ(QtMeshId::stateId(QtMeshId::State::Connecting), "connecting");
    EXPECT_EQ(QtMeshId::stateId(QtMeshId::State::SignedIn), "signedIn");
    EXPECT_EQ(QtMeshId::stateId(QtMeshId::State::Expired), "expired");
    EXPECT_EQ(QtMeshId::manageAccountUrl().scheme(), "https");
    EXPECT_EQ(QtMeshId::manageAccountUrl().host(), "qtmesh.dev");
}

TEST_F(QtMeshIdTest, StateFollowsTheStoredSession)
{
    auto* id = QtMeshId::instance();
    EXPECT_EQ(id->state(), QtMeshId::State::SignedOut);
    EXPECT_TRUE(id->displayName().isEmpty());

    storeSession(nowMs() + 3600 * 1000, QStringLiteral("Ada Lovelace"));
    EXPECT_EQ(id->state(), QtMeshId::State::SignedIn);
    EXPECT_EQ(id->displayName(), "Ada Lovelace");
    EXPECT_EQ(id->initials(), "AL");

    storeSession(nowMs() - 1000, QStringLiteral("Ada Lovelace"));
    EXPECT_EQ(id->state(), QtMeshId::State::Expired);
}

TEST_F(QtMeshIdTest, RefreshSignalsOnlyRealChanges)
{
    QSignalSpy spy(QtMeshId::instance(), &QtMeshId::sessionChanged);
    QtMeshId::instance()->refresh();
    EXPECT_EQ(spy.count(), 0);
    storeSession(nowMs() + 3600 * 1000, QStringLiteral("Ada"));
    EXPECT_EQ(spy.count(), 1);
    QtMeshId::instance()->refresh();
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(QtMeshIdTest, SignedOutPanelInvitesWithoutRequiringAnAccount)
{
    WelcomeDialog dialog;
    auto* panel = dialog.findChild<QtMeshIdPanel*>(QStringLiteral("qtmeshIdPanel"));
    ASSERT_NE(panel, nullptr);
    EXPECT_NE(panel->findChild<QPushButton*>(QStringLiteral("qtmeshIdSignIn")), nullptr);
    EXPECT_NE(panel->findChild<QPushButton*>(QStringLiteral("qtmeshIdCreate")), nullptr);
    EXPECT_EQ(panel->findChild<QPushButton*>(QStringLiteral("qtmeshIdManage")), nullptr);
    const QString text = panelText(*panel);
    EXPECT_TRUE(text.contains(QStringLiteral("Optional")));
    EXPECT_TRUE(text.contains(QStringLiteral("Sign in with QtMesh ID")));
    EXPECT_TRUE(text.contains(QStringLiteral("Create a free account")));
    // Neither account button may become the dialog's Enter-key default.
    for (auto* button : panel->findChildren<QPushButton*>()) {
        EXPECT_FALSE(button->isDefault()) << button->text().toStdString();
        EXPECT_FALSE(button->autoDefault()) << button->text().toStdString();
    }
}

TEST_F(QtMeshIdTest, SignedInPanelShowsTheAccountAndNoSecrets)
{
    storeSession(nowMs() + 3600 * 1000, QStringLiteral("Ada Lovelace"));
    WelcomeDialog dialog;
    auto* panel = dialog.findChild<QtMeshIdPanel*>(QStringLiteral("qtmeshIdPanel"));
    ASSERT_NE(panel, nullptr);
    const QString text = panelText(*panel);
    EXPECT_TRUE(text.contains(QStringLiteral("Connected as <b>Ada Lovelace</b>")));
    EXPECT_FALSE(text.contains(QStringLiteral("secret-token")));
    EXPECT_NE(panel->findChild<QPushButton*>(QStringLiteral("qtmeshIdManage")), nullptr);
    EXPECT_NE(panel->findChild<QPushButton*>(QStringLiteral("qtmeshIdSignOut")), nullptr);
    EXPECT_EQ(panel->findChild<QPushButton*>(QStringLiteral("qtmeshIdSignIn")), nullptr);
    EXPECT_NE(panel->findChild<QLabel*>(QStringLiteral("qtmeshIdAvatar")), nullptr);
}

TEST_F(QtMeshIdTest, ExpiredPanelAsksToSignInAgain)
{
    storeSession(nowMs() - 1000, QStringLiteral("Ada"));
    QtMeshIdPanel panel(QStringLiteral("test"));
    EXPECT_TRUE(panelText(panel).contains(QStringLiteral("expired")));
    EXPECT_NE(panel.findChild<QPushButton*>(QStringLiteral("qtmeshIdSignIn")), nullptr);
    EXPECT_EQ(panel.findChild<QPushButton*>(QStringLiteral("qtmeshIdCreate")), nullptr);
}

TEST_F(QtMeshIdTest, PanelFollowsASignInOrOutMadeElsewhere)
{
    QtMeshIdPanel panel(QStringLiteral("test"));
    ASSERT_NE(panel.findChild<QPushButton*>(QStringLiteral("qtmeshIdSignIn")), nullptr);

    storeSession(nowMs() + 3600 * 1000, QStringLiteral("Ada"));   // e.g. the account button
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_NE(panel.findChild<QPushButton*>(QStringLiteral("qtmeshIdManage")), nullptr);
    EXPECT_EQ(panel.findChild<QPushButton*>(QStringLiteral("qtmeshIdSignIn")), nullptr);

    CloudCredentialStore::clearSession();
    QtMeshId::instance()->refresh();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_NE(panel.findChild<QPushButton*>(QStringLiteral("qtmeshIdSignIn")), nullptr);
    EXPECT_EQ(panel.findChild<QPushButton*>(QStringLiteral("qtmeshIdManage")), nullptr);
}

TEST_F(QtMeshIdTest, OverlayControllerExposesStateAndNameOnly)
{
    auto* controller = WelcomeScreenController::instance();
    QSignalSpy spy(controller, &WelcomeScreenController::accountChanged);
    EXPECT_EQ(controller->accountState(), "signedOut");
    storeSession(nowMs() + 3600 * 1000, QStringLiteral("Ada Lovelace"));
    EXPECT_GE(spy.count(), 1);
    EXPECT_EQ(controller->accountState(), "signedIn");
    EXPECT_EQ(controller->accountName(), "Ada Lovelace");
    EXPECT_EQ(controller->accountInitials(), "AL");
}
