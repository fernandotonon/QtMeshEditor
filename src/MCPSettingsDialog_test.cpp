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

static QLabel* findStatusLabel(MCPSettingsDialog& dialog) {
    const auto labels = dialog.findChildren<QLabel*>();
    for (auto* label : labels) {
        const QString text = label->text();
        if (text.contains("Running") || text.contains("Stopped") || text.contains("Failed"))
            return label;
    }
    return nullptr;
}

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

TEST_F(MCPSettingsDialogTest, EnableToggledStartSuccessDisablesPortAndShowsRunningStatus) {
    MCPSettingsDialog dialog(false, 5050);

    int capturedPort = -1;
    dialog.startCallback = [&capturedPort](int port) -> bool {
        capturedPort = port;
        return true;
    };

    auto enableCheckBox = dialog.findChild<QCheckBox*>();
    auto portSpinBox = dialog.findChild<QSpinBox*>();
    ASSERT_NE(enableCheckBox, nullptr);
    ASSERT_NE(portSpinBox, nullptr);

    enableCheckBox->setChecked(true);

    EXPECT_TRUE(enableCheckBox->isChecked());
    EXPECT_EQ(capturedPort, 5050);
    EXPECT_FALSE(portSpinBox->isEnabled());

    auto statusLabel = findStatusLabel(dialog);
    ASSERT_NE(statusLabel, nullptr);
    EXPECT_TRUE(statusLabel->text().contains("Running on port 5050"));
    EXPECT_TRUE(statusLabel->styleSheet().contains("green"));
}

TEST_F(MCPSettingsDialogTest, EnableToggledStartFailureRevertsCheckboxAndShowsErrorStatus) {
    MCPSettingsDialog dialog(false, 6060);

    bool callbackCalled = false;
    int capturedPort = -1;
    dialog.startCallback = [&callbackCalled, &capturedPort](int port) -> bool {
        callbackCalled = true;
        capturedPort = port;
        return false;
    };

    auto enableCheckBox = dialog.findChild<QCheckBox*>();
    auto portSpinBox = dialog.findChild<QSpinBox*>();
    ASSERT_NE(enableCheckBox, nullptr);
    ASSERT_NE(portSpinBox, nullptr);

    enableCheckBox->setChecked(true);

    EXPECT_TRUE(callbackCalled);
    EXPECT_EQ(capturedPort, 6060);
    EXPECT_FALSE(enableCheckBox->isChecked());
    EXPECT_TRUE(portSpinBox->isEnabled());

    auto statusLabel = findStatusLabel(dialog);
    ASSERT_NE(statusLabel, nullptr);
    EXPECT_TRUE(statusLabel->text().contains("Failed to start on port 6060"));
    EXPECT_TRUE(statusLabel->styleSheet().contains("red"));
}

TEST_F(MCPSettingsDialogTest, DisableCallsStopCallbackAndShowsStoppedStatus) {
    MCPSettingsDialog dialog(true, 7070);

    int stopCalls = 0;
    dialog.stopCallback = [&stopCalls]() {
        ++stopCalls;
    };

    auto enableCheckBox = dialog.findChild<QCheckBox*>();
    auto portSpinBox = dialog.findChild<QSpinBox*>();
    ASSERT_NE(enableCheckBox, nullptr);
    ASSERT_NE(portSpinBox, nullptr);
    ASSERT_TRUE(enableCheckBox->isChecked());

    enableCheckBox->setChecked(false);

    EXPECT_EQ(stopCalls, 1);
    EXPECT_FALSE(enableCheckBox->isChecked());
    EXPECT_TRUE(portSpinBox->isEnabled());

    auto statusLabel = findStatusLabel(dialog);
    ASSERT_NE(statusLabel, nullptr);
    EXPECT_EQ(statusLabel->text(), "Stopped");
    EXPECT_TRUE(statusLabel->styleSheet().contains("gray"));
}

TEST_F(MCPSettingsDialogTest, EnableWithoutStartCallbackFallsBackToFailureState) {
    MCPSettingsDialog dialog(false, 8088);

    auto enableCheckBox = dialog.findChild<QCheckBox*>();
    ASSERT_NE(enableCheckBox, nullptr);

    enableCheckBox->setChecked(true);

    EXPECT_FALSE(enableCheckBox->isChecked());
    auto statusLabel = findStatusLabel(dialog);
    ASSERT_NE(statusLabel, nullptr);
    EXPECT_TRUE(statusLabel->text().contains("Failed to start on port 8088"));
}
