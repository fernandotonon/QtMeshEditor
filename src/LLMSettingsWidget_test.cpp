#ifdef ENABLE_LOCAL_LLM

#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QTabWidget>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include "LLMSettingsWidget.h"

class LLMSettingsWidgetTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
    }

    QApplication* app = nullptr;
};

TEST_F(LLMSettingsWidgetTest, Constructor)
{
    LLMSettingsWidget widget;
    EXPECT_EQ(widget.windowTitle(), "AI Model Settings");
    EXPECT_GE(widget.minimumWidth(), 500);
    EXPECT_GE(widget.minimumHeight(), 450);
}

TEST_F(LLMSettingsWidgetTest, HasTabWidget)
{
    LLMSettingsWidget widget;
    QTabWidget* tabWidget = widget.findChild<QTabWidget*>();
    ASSERT_NE(tabWidget, nullptr);
    EXPECT_EQ(tabWidget->count(), 3);
}

TEST_F(LLMSettingsWidgetTest, TabNames)
{
    LLMSettingsWidget widget;
    QTabWidget* tabWidget = widget.findChild<QTabWidget*>();
    ASSERT_NE(tabWidget, nullptr);
    EXPECT_EQ(tabWidget->tabText(0), "Models");
    EXPECT_EQ(tabWidget->tabText(1), "Settings");
    EXPECT_EQ(tabWidget->tabText(2), "Download");
}

TEST_F(LLMSettingsWidgetTest, HasModelCombo)
{
    LLMSettingsWidget widget;
    QComboBox* combo = widget.findChild<QComboBox*>();
    EXPECT_NE(combo, nullptr);
}

TEST_F(LLMSettingsWidgetTest, HasLoadButton)
{
    LLMSettingsWidget widget;
    // Find buttons by text
    QList<QPushButton*> buttons = widget.findChildren<QPushButton*>();
    bool hasLoad = false;
    for (QPushButton* btn : buttons) {
        if (btn->text().contains("Load", Qt::CaseInsensitive)) {
            hasLoad = true;
            break;
        }
    }
    EXPECT_TRUE(hasLoad);
}

TEST_F(LLMSettingsWidgetTest, HasStatusLabel)
{
    LLMSettingsWidget widget;
    QList<QLabel*> labels = widget.findChildren<QLabel*>();
    EXPECT_GT(labels.size(), 0);
}

// Test formatFileSize via indirect observation
// formatFileSize is private, but we can test behavior through the download tab UI

#endif // ENABLE_LOCAL_LLM
