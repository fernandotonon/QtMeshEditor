#include "CloudAccountMenuButton.h"

#include "AppSettingsKeys.h"
#include "CloudCredentialStore.h"

#include <QApplication>
#include <QCoreApplication>
#include <QLabel>
#include <QSettings>
#include <gtest/gtest.h>

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
}

TEST_F(CloudAccountMenuButtonTest, SignedOutButtonRepaintsWithoutCrash)
{
    CloudAccountMenuButton button;
    button.show();
    button.refresh();
    button.repaint();
    QApplication::processEvents();

    CloudSession session;
    session.token = QStringLiteral("test-token");
    session.email = QStringLiteral("dev@example.com");
    ASSERT_TRUE(CloudCredentialStore::saveSession(session));
    QSettings().setValue(AppSettingsKeys::cloudUserName(), QStringLiteral("Dev User"));
    button.refresh();
    button.repaint();
    QApplication::processEvents();

    auto* subtitle = button.findChild<QLabel*>(QStringLiteral("cloudAccountMenuHeaderSubtitle"));
    ASSERT_NE(subtitle, nullptr);
    EXPECT_EQ(subtitle->text(), QStringLiteral("Signed in to QtMesh Cloud"));

    CloudCredentialStore::clearSession();
    button.refresh();
    button.repaint();
    QApplication::processEvents();
}
