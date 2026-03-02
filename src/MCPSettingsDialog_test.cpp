#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QCheckBox>
#include <QSpinBox>
#include <QLabel>
#include <QThread>
#include "MCPSettingsDialog.h"

class MCPSettingsDialogTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;

    void SetUp() override {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
    }

    void TearDown() override {
        QThread::msleep(10);
    }
};

TEST_F(MCPSettingsDialogTest, ConstructionDoesNotCrash) {
    MCPSettingsDialog dialog(false, 8080);
    EXPECT_TRUE(true);
}

TEST_F(MCPSettingsDialogTest, UIElementsExist) {
    MCPSettingsDialog dialog(false, 8080);

    // Check if checkbox exists
    auto enableCheckBox = dialog.findChild<QCheckBox*>();
    EXPECT_NE(enableCheckBox, nullptr);

    // Check if port spinbox exists
    auto portSpinBox = dialog.findChild<QSpinBox*>();
    EXPECT_NE(portSpinBox, nullptr);

    // Check if status label exists
    auto statusLabel = dialog.findChild<QLabel*>();
    EXPECT_NE(statusLabel, nullptr);
}

TEST_F(MCPSettingsDialogTest, DefaultValuesServerNotRunning) {
    MCPSettingsDialog dialog(false, 8080);

    auto enableCheckBox = dialog.findChild<QCheckBox*>();
    ASSERT_NE(enableCheckBox, nullptr);
    EXPECT_FALSE(enableCheckBox->isChecked());

    auto portSpinBox = dialog.findChild<QSpinBox*>();
    ASSERT_NE(portSpinBox, nullptr);
    EXPECT_EQ(portSpinBox->value(), 8080);
}

TEST_F(MCPSettingsDialogTest, DefaultValuesServerRunning) {
    MCPSettingsDialog dialog(true, 9090);

    auto enableCheckBox = dialog.findChild<QCheckBox*>();
    ASSERT_NE(enableCheckBox, nullptr);
    EXPECT_TRUE(enableCheckBox->isChecked());

    auto portSpinBox = dialog.findChild<QSpinBox*>();
    ASSERT_NE(portSpinBox, nullptr);
    EXPECT_EQ(portSpinBox->value(), 9090);
}

TEST_F(MCPSettingsDialogTest, PortSpinBoxIsDisabledWhenServerRunning) {
    MCPSettingsDialog dialog(true, 8080);

    auto portSpinBox = dialog.findChild<QSpinBox*>();
    ASSERT_NE(portSpinBox, nullptr);
    // When server is running, port should be read-only/disabled
    EXPECT_FALSE(portSpinBox->isEnabled());
}

TEST_F(MCPSettingsDialogTest, PortSpinBoxIsEnabledWhenServerNotRunning) {
    MCPSettingsDialog dialog(false, 8080);

    auto portSpinBox = dialog.findChild<QSpinBox*>();
    ASSERT_NE(portSpinBox, nullptr);
    EXPECT_TRUE(portSpinBox->isEnabled());
}

TEST_F(MCPSettingsDialogTest, StatusLabelExists) {
    MCPSettingsDialog dialog(false, 8080);

    auto statusLabel = dialog.findChild<QLabel*>();
    ASSERT_NE(statusLabel, nullptr);
    EXPECT_FALSE(statusLabel->text().isEmpty());
}

TEST_F(MCPSettingsDialogTest, CallbacksCanBeSet) {
    MCPSettingsDialog dialog(false, 8080);

    bool startCallbackCalled = false;
    bool stopCallbackCalled = false;

    dialog.startCallback = [&startCallbackCalled](int port) -> bool {
        startCallbackCalled = true;
        return true;
    };

    dialog.stopCallback = [&stopCallbackCalled]() {
        stopCallbackCalled = true;
    };

    // Verify callbacks are set
    EXPECT_TRUE(dialog.startCallback != nullptr);
    EXPECT_TRUE(dialog.stopCallback != nullptr);

    // Test that callbacks can be called
    bool result = dialog.startCallback(8080);
    EXPECT_TRUE(result);
    EXPECT_TRUE(startCallbackCalled);

    dialog.stopCallback();
    EXPECT_TRUE(stopCallbackCalled);
}
