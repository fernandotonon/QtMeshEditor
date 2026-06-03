#include "CloudAccountMenuButton.h"

#include "AppSettingsKeys.h"
#include "CloudCredentialStore.h"

#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <QMenu>
#include <QSettings>
#include <QSignalSpy>
#include <QToolButton>
#include <QWidgetAction>
#include <gtest/gtest.h>

namespace {

QAction* findMenuAction(CloudAccountMenuButton& button, const QString& objectName)
{
    for (QAction* action : button.menu()->actions()) {
        if (action->objectName() == objectName)
            return action;
    }
    return nullptr;
}

bool menuListsHeader(CloudAccountMenuButton& button)
{
    for (QAction* action : button.menu()->actions()) {
        auto* widgetAction = qobject_cast<QWidgetAction*>(action);
        if (!widgetAction)
            continue;
        QWidget* widget = widgetAction->defaultWidget();
        if (widget && widget->objectName() == QStringLiteral("cloudAccountMenuHeader"))
            return true;
    }
    return false;
}

} // namespace

class CloudAccountMenuButtonTest : public ::testing::Test {
protected:
    QString previousOrganizationName;
    QString previousApplicationName;

    void SetUp() override
    {
        previousOrganizationName = QCoreApplication::organizationName();
        previousApplicationName = QCoreApplication::applicationName();
        QCoreApplication::setOrganizationName(QStringLiteral("QtMeshEditorTests"));
        QCoreApplication::setApplicationName(QStringLiteral("CloudAccountMenuButtonTest"));
        QSettings().clear();
        CloudCredentialStore::clearSession();
    }

    void TearDown() override
    {
        CloudCredentialStore::clearSession();
        QSettings().clear();
        QCoreApplication::setOrganizationName(previousOrganizationName);
        QCoreApplication::setApplicationName(previousApplicationName);
    }

    static void saveTestSession(const QString& email = QStringLiteral("dev@example.com"))
    {
        CloudSession session;
        session.token = QStringLiteral("test-token");
        session.email = email;
        ASSERT_TRUE(CloudCredentialStore::saveSession(session));
    }
};

TEST_F(CloudAccountMenuButtonTest, InitialsFromDisplayName)
{
    EXPECT_EQ(CloudAccountMenuButton::initialsFromDisplayName(QStringLiteral("Fernando Tonon")),
              QStringLiteral("FT"));
    EXPECT_EQ(CloudAccountMenuButton::initialsFromDisplayName(QStringLiteral("Dev User")),
              QStringLiteral("DU"));
    EXPECT_EQ(CloudAccountMenuButton::initialsFromDisplayName(QStringLiteral("Ada")),
              QStringLiteral("A"));
    EXPECT_EQ(CloudAccountMenuButton::initialsFromDisplayName(QStringLiteral("  ")), QString());
    EXPECT_EQ(CloudAccountMenuButton::initialsFromDisplayName(QStringLiteral("123")),
              QString());
    EXPECT_EQ(CloudAccountMenuButton::initialsFromDisplayName(QStringLiteral("Jean-Pierre Martin")),
              QStringLiteral("JM"));
    EXPECT_EQ(CloudAccountMenuButton::initialsFromDisplayName(QStringLiteral("x y")),
              QStringLiteral("XY"));
}

TEST_F(CloudAccountMenuButtonTest, RefreshSignedOutState)
{
    CloudAccountMenuButton button;
    button.refresh();

    auto* signIn = findMenuAction(button, QStringLiteral("actionQtMeshCloudSignIn"));
    auto* signOut = findMenuAction(button, QStringLiteral("actionQtMeshCloudSignOut"));
    auto* openProjects = findMenuAction(button, QStringLiteral("actionQtMeshCloudOpenProjects"));
    auto* upload = findMenuAction(button, QStringLiteral("actionQtMeshCloudUploadFiles"));
    ASSERT_NE(signIn, nullptr);
    ASSERT_NE(signOut, nullptr);
    ASSERT_NE(openProjects, nullptr);
    ASSERT_NE(upload, nullptr);

    EXPECT_TRUE(signIn->isVisible());
    EXPECT_TRUE(signIn->isEnabled());
    EXPECT_FALSE(signOut->isVisible());
    EXPECT_FALSE(openProjects->isEnabled());
    EXPECT_TRUE(upload->isEnabled());
    EXPECT_FALSE(menuListsHeader(button));
    EXPECT_EQ(button.toolButton()->toolTip(), QStringLiteral("Sign in to QtMesh Cloud"));
}

TEST_F(CloudAccountMenuButtonTest, RefreshSignedInShowsHeaderAndAccountActions)
{
    saveTestSession();
    QSettings().setValue(AppSettingsKeys::cloudUserName(), QStringLiteral("Dev User"));

    CloudAccountMenuButton button;
    button.refresh();

    auto* signIn = findMenuAction(button, QStringLiteral("actionQtMeshCloudSignIn"));
    auto* signOut = findMenuAction(button, QStringLiteral("actionQtMeshCloudSignOut"));
    auto* openProjects = findMenuAction(button, QStringLiteral("actionQtMeshCloudOpenProjects"));
    ASSERT_NE(signIn, nullptr);
    ASSERT_NE(signOut, nullptr);
    ASSERT_NE(openProjects, nullptr);

    EXPECT_FALSE(signIn->isVisible());
    EXPECT_TRUE(signOut->isVisible());
    EXPECT_TRUE(signOut->isEnabled());
    EXPECT_TRUE(openProjects->isEnabled());
    EXPECT_TRUE(menuListsHeader(button));

    auto* headerName = button.findChild<QLabel*>(QStringLiteral("cloudAccountMenuHeaderName"),
                                                   Qt::FindChildrenRecursively);
    ASSERT_NE(headerName, nullptr);
    EXPECT_EQ(headerName->text(), QStringLiteral("Dev User"));

    auto* subtitle = button.findChild<QLabel*>(QStringLiteral("cloudAccountMenuHeaderSubtitle"),
                                                Qt::FindChildrenRecursively);
    ASSERT_NE(subtitle, nullptr);
    EXPECT_EQ(subtitle->text(), QStringLiteral("Signed in to QtMesh Cloud"));

    EXPECT_TRUE(button.toolButton()->toolTip().contains(QStringLiteral("Dev User")));
}

TEST_F(CloudAccountMenuButtonTest, RefreshUsesSlugWhenUserNameMissing)
{
    saveTestSession();
    QSettings().setValue(AppSettingsKeys::cloudUserSlug(), QStringLiteral("dev-slug"));

    CloudAccountMenuButton button;
    button.refresh();

    auto* headerName = button.findChild<QLabel*>(QStringLiteral("cloudAccountMenuHeaderName"),
                                                   Qt::FindChildrenRecursively);
    ASSERT_NE(headerName, nullptr);
    EXPECT_EQ(headerName->text(), QStringLiteral("dev-slug"));
}

TEST_F(CloudAccountMenuButtonTest, RefreshUsesEmailWhenNameAndSlugMissing)
{
    saveTestSession(QStringLiteral("person@example.com"));

    CloudAccountMenuButton button;
    button.refresh();

    auto* headerName = button.findChild<QLabel*>(QStringLiteral("cloudAccountMenuHeaderName"),
                                                   Qt::FindChildrenRecursively);
    ASSERT_NE(headerName, nullptr);
    EXPECT_EQ(headerName->text(), QStringLiteral("person@example.com"));
}

TEST_F(CloudAccountMenuButtonTest, SignedInWithoutDisplayNameHidesHeader)
{
    CloudSession session;
    session.token = QStringLiteral("test-token");
    ASSERT_TRUE(CloudCredentialStore::saveSession(session));

    CloudAccountMenuButton button;
    button.refresh();

    EXPECT_FALSE(menuListsHeader(button));
    EXPECT_EQ(button.toolButton()->toolTip(), QStringLiteral("Sign in to QtMesh Cloud"));
}

TEST_F(CloudAccountMenuButtonTest, MenuEmitsSignInRequested)
{
    CloudAccountMenuButton button;
    QSignalSpy spy(&button, &CloudAccountMenuButton::signInRequested);

    auto* signIn = findMenuAction(button, QStringLiteral("actionQtMeshCloudSignIn"));
    ASSERT_NE(signIn, nullptr);
    signIn->trigger();
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(CloudAccountMenuButtonTest, MenuEmitsSignOutRequested)
{
    saveTestSession();
    QSettings().setValue(AppSettingsKeys::cloudUserName(), QStringLiteral("Dev User"));

    CloudAccountMenuButton button;
    button.refresh();
    QSignalSpy spy(&button, &CloudAccountMenuButton::signOutRequested);

    auto* signOut = findMenuAction(button, QStringLiteral("actionQtMeshCloudSignOut"));
    ASSERT_NE(signOut, nullptr);
    signOut->trigger();
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(CloudAccountMenuButtonTest, MenuEmitsUploadFilesRequested)
{
    CloudAccountMenuButton button;
    QSignalSpy spy(&button, &CloudAccountMenuButton::uploadFilesRequested);

    auto* upload = findMenuAction(button, QStringLiteral("actionQtMeshCloudUploadFiles"));
    ASSERT_NE(upload, nullptr);
    upload->trigger();
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(CloudAccountMenuButtonTest, MenuEmitsOpenProjectsRequested)
{
    saveTestSession();
    QSettings().setValue(AppSettingsKeys::cloudUserName(), QStringLiteral("Dev User"));

    CloudAccountMenuButton button;
    button.refresh();

    auto* openProjects = findMenuAction(button, QStringLiteral("actionQtMeshCloudOpenProjects"));
    ASSERT_NE(openProjects, nullptr);
    ASSERT_TRUE(openProjects->isEnabled());

    int openProjectsSignalCount = 0;
    QMetaObject::Connection conn = connect(
        &button, &CloudAccountMenuButton::openProjectsRequested, this,
        [&openProjectsSignalCount]() { ++openProjectsSignalCount; });

    QSignalSpy actionSpy(openProjects, &QAction::triggered);
    openProjects->trigger();
    EXPECT_EQ(actionSpy.count(), 1);
    EXPECT_EQ(openProjectsSignalCount, 1);
    disconnect(conn);
}

TEST_F(CloudAccountMenuButtonTest, AboutToShowRefreshesSignedInState)
{
    CloudAccountMenuButton button;
    button.show();
    button.refresh();
    QApplication::processEvents();

    saveTestSession();
    QSettings().setValue(AppSettingsKeys::cloudUserName(), QStringLiteral("Late User"));

    emit button.menu()->aboutToShow();
    QApplication::processEvents();

    auto* signOut = findMenuAction(button, QStringLiteral("actionQtMeshCloudSignOut"));
    ASSERT_NE(signOut, nullptr);
    EXPECT_TRUE(signOut->isVisible());
    EXPECT_TRUE(menuListsHeader(button));
}

TEST_F(CloudAccountMenuButtonTest, SignedOutButtonRepaintsWithoutCrash)
{
    CloudAccountMenuButton button;
    button.show();
    button.refresh();
    button.repaint();
    QApplication::processEvents();

    saveTestSession();
    QSettings().setValue(AppSettingsKeys::cloudUserName(), QStringLiteral("Dev User"));
    button.refresh();
    button.toolButton()->repaint();
    QApplication::processEvents();

    CloudCredentialStore::clearSession();
    QSettings().remove(AppSettingsKeys::cloudUserName());
    button.refresh();
    button.repaint();
    QApplication::processEvents();

    EXPECT_FALSE(menuListsHeader(button));
}

TEST_F(CloudAccountMenuButtonTest, ToolButtonAndMenuObjectNames)
{
    CloudAccountMenuButton button;
    EXPECT_NE(button.toolButton(), nullptr);
    EXPECT_EQ(button.toolButton()->objectName(), QStringLiteral("cloudAccountButton"));
    EXPECT_NE(button.menu(), nullptr);
    EXPECT_EQ(button.menu()->objectName(), QStringLiteral("menuCloud"));
}
