#include <gtest/gtest.h>
#include "ThemeManager.h"
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>

class ThemeManagerTests : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    void SetUp() override {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
    }
};

TEST_F(ThemeManagerTests, Singleton) {
    auto* tm = ThemeManager::instance();
    ASSERT_NE(tm, nullptr);
    EXPECT_EQ(tm, ThemeManager::instance());
}

TEST_F(ThemeManagerTests, ColorsAreValid) {
    auto* tm = ThemeManager::instance();
    ASSERT_NE(tm, nullptr);
    EXPECT_TRUE(tm->windowColor().isValid());
    EXPECT_TRUE(tm->panelColor().isValid());
    EXPECT_TRUE(tm->headerColor().isValid());
    EXPECT_TRUE(tm->inputColor().isValid());
    EXPECT_TRUE(tm->textColor().isValid());
    EXPECT_TRUE(tm->disabledTextColor().isValid());
    EXPECT_TRUE(tm->highlightColor().isValid());
    EXPECT_TRUE(tm->buttonColor().isValid());
    EXPECT_TRUE(tm->borderColor().isValid());
    EXPECT_TRUE(tm->accentColor().isValid());
}

TEST_F(ThemeManagerTests, ThemeNameNotEmpty) {
    auto* tm = ThemeManager::instance();
    ASSERT_NE(tm, nullptr);
    QString name = tm->themeName();
    EXPECT_FALSE(name.isEmpty());
    EXPECT_TRUE(name == "light" || name == "dark");
}

TEST_F(ThemeManagerTests, RefreshThemeEmitsSignal) {
    auto* tm = ThemeManager::instance();
    ASSERT_NE(tm, nullptr);
    QSignalSpy spy(tm, &ThemeManager::themeChanged);
    tm->refreshTheme();
    EXPECT_EQ(spy.count(), 1);
}
