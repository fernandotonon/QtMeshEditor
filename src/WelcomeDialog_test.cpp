#include <gtest/gtest.h>
#include "WelcomeDialog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QSettings>
#include <QCheckBox>
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryFile>
#include <QSignalSpy>
#include <QLabel>
#include <QToolButton>
#include <QVariantMap>

namespace {
QPushButton* findButtonByText(WelcomeDialog& dialog, const QString& text)
{
    const auto buttons = dialog.findChildren<QPushButton*>();
    for (auto* button : buttons) {
        if (button && button->text() == text) {
            return button;
        }
    }
    return nullptr;
}
}

class WelcomeDialogTests : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_NE(qobject_cast<QApplication*>(QCoreApplication::instance()), nullptr);
        // Clear the setting before each test
        QSettings settings;
        settings.remove("WelcomeScreen/dontShowAgain");
    }

    void TearDown() override {
        // Clean up the setting after each test
        QSettings settings;
        settings.remove("WelcomeScreen/dontShowAgain");
    }
};

TEST_F(WelcomeDialogTests, ShouldShowReturnsTrueByDefault) {
    EXPECT_TRUE(WelcomeDialog::shouldShow());
}

TEST_F(WelcomeDialogTests, ShouldShowReturnsFalseAfterDontShowAgain) {
    QSettings settings;
    settings.setValue("WelcomeScreen/dontShowAgain", true);
    EXPECT_FALSE(WelcomeDialog::shouldShow());
}

TEST_F(WelcomeDialogTests, ShouldShowReturnsTrueAfterSettingCleared) {
    QSettings settings;
    settings.setValue("WelcomeScreen/dontShowAgain", true);
    EXPECT_FALSE(WelcomeDialog::shouldShow());

    settings.remove("WelcomeScreen/dontShowAgain");
    EXPECT_TRUE(WelcomeDialog::shouldShow());
}

TEST_F(WelcomeDialogTests, DefaultActionIsDismissed) {
    WelcomeDialog dialog;
    EXPECT_EQ(dialog.userAction(), WelcomeDialog::Dismissed);
}

TEST_F(WelcomeDialogTests, DefaultSelectedFileIsEmpty) {
    WelcomeDialog dialog;
    EXPECT_TRUE(dialog.selectedFile().isEmpty());
}

TEST_F(WelcomeDialogTests, OpenFileDialogOptionsHonorsPlatformBehavior)
{
    const QFileDialog::Options options = WelcomeDialog::openFileDialogOptions();

    EXPECT_TRUE(options.testFlag(QFileDialog::HideNameFilterDetails));
    EXPECT_TRUE(options.testFlag(QFileDialog::DontUseNativeDialog));
}

TEST_F(WelcomeDialogTests, ShouldShowRespectsDontShowAgainSetting) {
    // Explicitly set the setting and verify shouldShow reads it
    QSettings settings;
    settings.setValue("WelcomeScreen/dontShowAgain", true);
    settings.sync();
    EXPECT_FALSE(WelcomeDialog::shouldShow());

    settings.setValue("WelcomeScreen/dontShowAgain", false);
    settings.sync();
    EXPECT_TRUE(WelcomeDialog::shouldShow());
}

TEST_F(WelcomeDialogTests, ShouldShowReturnsTrueWhenSettingIsNonBooleanFalsy) {
    // When the setting is set to a value that converts to false, shouldShow returns true
    QSettings settings;
    settings.setValue("WelcomeScreen/dontShowAgain", 0);
    // QVariant(0).toBool() == false, so shouldShow should return true
    EXPECT_TRUE(WelcomeDialog::shouldShow());
}

TEST_F(WelcomeDialogTests, MultipleDialogInstancesShareSameSettingsState) {
    QSettings settings;
    settings.setValue("WelcomeScreen/dontShowAgain", true);

    // Both calls should return the same result since they read from QSettings
    EXPECT_FALSE(WelcomeDialog::shouldShow());
    EXPECT_FALSE(WelcomeDialog::shouldShow());

    settings.remove("WelcomeScreen/dontShowAgain");
    EXPECT_TRUE(WelcomeDialog::shouldShow());
}

TEST_F(WelcomeDialogTests, NewSceneButtonSetsActionAndAcceptsDialog)
{
    WelcomeDialog dialog;
    QSignalSpy acceptedSpy(&dialog, &QDialog::accepted);
    ASSERT_TRUE(acceptedSpy.isValid());

    auto* newSceneButton = findButtonByText(dialog, "New Scene");
    ASSERT_NE(newSceneButton, nullptr);

    newSceneButton->click();

    EXPECT_EQ(dialog.userAction(), WelcomeDialog::NewScene);
    EXPECT_EQ(acceptedSpy.count(), 1);
    EXPECT_EQ(dialog.result(), QDialog::Accepted);
}

TEST_F(WelcomeDialogTests, GetStartedPersistsDontShowAgainWhenChecked)
{
    WelcomeDialog dialog;

    auto* dontShowCheck = dialog.findChild<QCheckBox*>();
    ASSERT_NE(dontShowCheck, nullptr);
    dontShowCheck->setChecked(true);

    auto* getStartedButton = findButtonByText(dialog, "Get Started");
    ASSERT_NE(getStartedButton, nullptr);
    getStartedButton->click();

    EXPECT_EQ(dialog.userAction(), WelcomeDialog::Dismissed);
    EXPECT_EQ(dialog.result(), QDialog::Accepted);

    QSettings settings;
    EXPECT_TRUE(settings.value("WelcomeScreen/dontShowAgain", false).toBool());
}

TEST_F(WelcomeDialogTests, ActivatingRecentFileSetsOpenRecentActionAndSelectedPath)
{
    QTemporaryFile existingFile;
    ASSERT_TRUE(existingFile.open());
    const QString existingPath = existingFile.fileName();

    QSettings settings;
    settings.setValue("RecentFiles/files", QStringList() << existingPath << "/path/that/does/not/exist.mesh");

    WelcomeDialog dialog;
    auto* recentList = dialog.findChild<QListWidget*>();
    ASSERT_NE(recentList, nullptr);
    ASSERT_EQ(recentList->count(), 1);

    QListWidgetItem* firstItem = recentList->item(0);
    ASSERT_NE(firstItem, nullptr);

    emit recentList->itemActivated(firstItem);

    EXPECT_EQ(dialog.userAction(), WelcomeDialog::OpenRecent);
    EXPECT_EQ(dialog.selectedFile(), existingPath);
    EXPECT_EQ(dialog.result(), QDialog::Accepted);
}

TEST_F(WelcomeDialogTests, ShowcaseHasFourGameCardsAndAnExploreLink)
{
    WelcomeDialog dialog;
    int cards = 0;
    for (auto* button : dialog.findChildren<QToolButton*>()) {
        if (button->objectName() == QStringLiteral("ecosystemGameCard")) {
            ++cards;
            EXPECT_EQ(button->focusPolicy(), Qt::StrongFocus);
        }
    }
    EXPECT_EQ(cards, 4);
    EXPECT_NE(findButtonByText(dialog, QStringLiteral("Explore QtMesh Games →")), nullptr);
}

TEST_F(WelcomeDialogTests, NoRecentFilesShowsAPlaceholderInsteadOfAnEmptyList)
{
    QSettings settings;
    const QVariant saved = settings.value("RecentFiles/files");
    settings.setValue("RecentFiles/files", QStringList() << "/path/that/does/not/exist.mesh");

    WelcomeDialog dialog;
    EXPECT_EQ(dialog.findChild<QListWidget*>(), nullptr);
    bool sawPlaceholder = false;
    for (auto* label : dialog.findChildren<QLabel*>())
        sawPlaceholder |= label->text().startsWith(QStringLiteral("No recent files yet"));
    EXPECT_TRUE(sawPlaceholder);

    if (saved.isValid()) settings.setValue("RecentFiles/files", saved);
    else settings.remove("RecentFiles/files");
}

TEST_F(WelcomeDialogTests, DiscoverySuggestionShowsAndDismissReportsTheKey)
{
    WelcomeDialog dialog;
    QString dismissedKey;
    dialog.setDiscoverySuggestion(
        QVariantMap{{"featureKey", "uv_unwrap"}, {"title", "Auto UV unwrap"},
                    {"blurb", "One click."}, {"personalized", false}},
        [&dismissedKey](const QString& key) { dismissedKey = key; });

    QToolButton* hide = nullptr;
    for (auto* button : dialog.findChildren<QToolButton*>())
        if (button->accessibleName() == QStringLiteral("Dismiss suggestion")) hide = button;
    ASSERT_NE(hide, nullptr);
    hide->click();
    EXPECT_EQ(dismissedKey, QStringLiteral("uv_unwrap"));
}

TEST_F(WelcomeDialogTests, EmptySuggestionAddsNoCard)
{
    WelcomeDialog dialog;
    dialog.setDiscoverySuggestion(QVariantMap());
    for (auto* button : dialog.findChildren<QToolButton*>())
        EXPECT_NE(button->accessibleName(), QStringLiteral("Dismiss suggestion"));
}
