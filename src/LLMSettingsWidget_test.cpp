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
    EXPECT_GE(widget.minimumWidth(), 550);
    EXPECT_GE(widget.minimumHeight(), 500);
}

TEST_F(LLMSettingsWidgetTest, HasTabWidget)
{
    LLMSettingsWidget widget;
    QTabWidget* tabWidget = widget.findChild<QTabWidget*>();
    ASSERT_NE(tabWidget, nullptr);
#ifdef ENABLE_STABLE_DIFFUSION
    EXPECT_EQ(tabWidget->count(), 6);
#else
    EXPECT_EQ(tabWidget->count(), 4);
#endif
}

TEST_F(LLMSettingsWidgetTest, TabNames)
{
    LLMSettingsWidget widget;
    QTabWidget* tabWidget = widget.findChild<QTabWidget*>();
    ASSERT_NE(tabWidget, nullptr);
    EXPECT_EQ(tabWidget->tabText(0), "LLM Models");
    EXPECT_EQ(tabWidget->tabText(1), "Settings");
    EXPECT_EQ(tabWidget->tabText(2), "LLM Download");
    EXPECT_EQ(tabWidget->tabText(3), "QtMeshEditor Models");
#ifdef ENABLE_STABLE_DIFFUSION
    EXPECT_EQ(tabWidget->tabText(4), "SD Models");
    EXPECT_EQ(tabWidget->tabText(5), "SD Settings");
#endif
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

// #1052: the LLM Download tab has the same delete affordances as the
// QtMeshEditor Models tab; Delete Selected is disabled until a downloaded
// model is selected.
TEST_F(LLMSettingsWidgetTest, DownloadTabHasDeleteRemoveAllAndOpenFolder)
{
    LLMSettingsWidget widget;
    auto* del    = widget.findChild<QPushButton*>("llmDeleteModelButton");
    auto* delAll = widget.findChild<QPushButton*>("llmDeleteAllModelsButton");
    auto* folder = widget.findChild<QPushButton*>("llmOpenFolderButton");
    ASSERT_NE(del, nullptr);
    ASSERT_NE(delAll, nullptr);
    ASSERT_NE(folder, nullptr);
    EXPECT_EQ(del->text(), "Delete Selected");
    EXPECT_FALSE(del->isEnabled()) << "nothing selected → nothing to delete";
    EXPECT_TRUE(folder->isEnabled());
}
