#include <gtest/gtest.h>
#include "WelcomeDialog.h"

#include <QApplication>
#include <QCoreApplication>
#include <QSettings>

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
