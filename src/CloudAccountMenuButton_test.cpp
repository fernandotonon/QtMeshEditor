#include "CloudAccountMenuButton.h"

#include "CloudCredentialStore.h"

#include <QApplication>
#include <gtest/gtest.h>

TEST(CloudAccountMenuButtonTest, InitialsFromDisplayName)
{
    EXPECT_EQ(CloudAccountMenuButton::initialsFromDisplayName(QStringLiteral("Fernando Tonon")),
              QStringLiteral("FT"));
    EXPECT_EQ(CloudAccountMenuButton::initialsFromDisplayName(QStringLiteral("Dev User")),
              QStringLiteral("DU"));
    EXPECT_EQ(CloudAccountMenuButton::initialsFromDisplayName(QStringLiteral("Ada")),
              QStringLiteral("A"));
    EXPECT_EQ(CloudAccountMenuButton::initialsFromDisplayName(QStringLiteral("  ")), QString());
}

TEST(CloudAccountMenuButtonTest, SignedOutButtonRepaintsWithoutCrash)
{
    CloudCredentialStore::clearSession();

    CloudAccountMenuButton button;
    button.show();
    button.refresh();
    button.repaint();
    QApplication::processEvents();

    CloudSession session;
    session.token = QStringLiteral("test-token");
    session.email = QStringLiteral("dev@example.com");
    ASSERT_TRUE(CloudCredentialStore::saveSession(session));
    button.refresh();
    button.repaint();
    QApplication::processEvents();

    CloudCredentialStore::clearSession();
    button.refresh();
    button.repaint();
    QApplication::processEvents();
}
