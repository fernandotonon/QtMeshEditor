#include <gtest/gtest.h>
#include "about.h"
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QToolButton>
#include "QtMeshEcosystem.h"

class AboutTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup code if needed
    }

    void TearDown() override {
        // Cleanup code if needed
    }
};

TEST_F(AboutTest, VersionTextIsCorrect) {
    // QApplication already created by test_main.cpp - do not create another
    ASSERT_NE(QCoreApplication::instance(), nullptr);

    About aboutDialog;
    About aboutDialog2(&aboutDialog); // Also test with a parent widget.
    QString expectedVersionText = QString("Version: ") + QTMESHEDITOR_VERSION;
    EXPECT_EQ(aboutDialog.getVersionText(), expectedVersionText);
    EXPECT_EQ(aboutDialog2.getVersionText(), expectedVersionText);
}

TEST_F(AboutTest, HasAboutEcosystemAndSupportTabs) {
    About dialog;
    auto* tabs = dialog.findChild<QTabWidget*>();
    ASSERT_NE(tabs, nullptr);
    ASSERT_EQ(tabs->count(), 3);
    EXPECT_EQ(tabs->tabText(0), QStringLiteral("About"));
    EXPECT_EQ(tabs->tabText(1), QStringLiteral("QtMesh Ecosystem"));
    EXPECT_EQ(tabs->tabText(2), QStringLiteral("Support Development"));
}

TEST_F(AboutTest, DescribesCurrentCapabilitiesNotJustOgreMeshes) {
    About dialog;
    bool sawDescription = false;
    for (auto* label : dialog.findChildren<QLabel*>()) {
        EXPECT_FALSE(label->text().contains(QStringLiteral("tweaking Ogre Meshes")));
        sawDescription |= label->text().contains(QStringLiteral("open-source 3D creation"));
    }
    EXPECT_TRUE(sawDescription);
}

TEST_F(AboutTest, ShowsEveryShowcaseGameAsAKeyboardFocusableCard) {
    About dialog;
    int cards = 0;
    for (auto* button : dialog.findChildren<QToolButton*>()) {
        if (button->objectName() == QStringLiteral("ecosystemGameCard")) {
            ++cards;
            EXPECT_EQ(button->focusPolicy(), Qt::StrongFocus);
            EXPECT_FALSE(button->icon().isNull()) << button->text().toStdString();
        }
    }
    EXPECT_EQ(cards, QtMeshEcosystem::showcaseGames().size());
}

TEST_F(AboutTest, SupportTabOffersContributeSponsorAndGames) {
    About dialog;
    QStringList texts;
    for (auto* button : dialog.findChildren<QPushButton*>())
        texts.append(button->text());
    EXPECT_TRUE(texts.contains(QStringLiteral("Contribute on GitHub")));
    EXPECT_TRUE(texts.contains(QStringLiteral("Sponsor on GitHub")));
    EXPECT_TRUE(texts.contains(QStringLiteral("Explore QtMesh Games")));
}

TEST_F(AboutTest, CreditsTheMaintainerFirstAndContributorsSeparately) {
    About dialog;
    auto* author = dialog.findChild<QLabel*>(QStringLiteral("aboutAuthor"));
    ASSERT_NE(author, nullptr);
    EXPECT_TRUE(author->text().contains(QStringLiteral("Created and maintained by")));
    EXPECT_TRUE(author->text().contains(QStringLiteral("https://github.com/fernandotonon\"")));
    EXPECT_TRUE(author->text().contains(QStringLiteral("Fernando Tonon")));
    EXPECT_FALSE(author->text().contains(QStringLiteral("Akira")));

    auto* thanks = dialog.findChild<QLabel*>(QStringLiteral("aboutAcknowledgments"));
    ASSERT_NE(thanks, nullptr);
    EXPECT_TRUE(thanks->text().contains(QStringLiteral("https://github.com/akira215")));
    EXPECT_TRUE(thanks->text().contains(
        QStringLiteral("https://github.com/fernandotonon/QtMeshEditor/graphs/contributors")));

    // The old "Developed by / with contributions from" wording is gone, and
    // the licence notice is still there.
    bool sawLicence = false;
    for (auto* label : dialog.findChildren<QLabel*>()) {
        EXPECT_FALSE(label->text().contains(QStringLiteral("with contributions from")));
        sawLicence |= label->text().contains(QStringLiteral("MIT License"));
    }
    EXPECT_TRUE(sawLicence);
}
