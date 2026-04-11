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
