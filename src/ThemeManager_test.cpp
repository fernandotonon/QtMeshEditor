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

TEST_F(ThemeManagerTests, PlaceholderTextColorIsValid) {
    auto* tm = ThemeManager::instance();
    ASSERT_NE(tm, nullptr);
    EXPECT_TRUE(tm->placeholderTextColor().isValid());
}

TEST_F(ThemeManagerTests, HighlightedTextColorIsValid) {
    auto* tm = ThemeManager::instance();
    ASSERT_NE(tm, nullptr);
    EXPECT_TRUE(tm->highlightedTextColor().isValid());
}

TEST_F(ThemeManagerTests, ButtonTextColorIsValid) {
    auto* tm = ThemeManager::instance();
    ASSERT_NE(tm, nullptr);
    EXPECT_TRUE(tm->buttonTextColor().isValid());
}

TEST_F(ThemeManagerTests, QmlInstanceReturnsSameAsInstance) {
    auto* tm1 = ThemeManager::instance();
    auto* tm2 = ThemeManager::qmlInstance(nullptr, nullptr);
    EXPECT_EQ(tm1, tm2);
}

TEST_F(ThemeManagerTests, KillAndRecreate) {
    auto* tm1 = ThemeManager::instance();
    ASSERT_NE(tm1, nullptr);

    ThemeManager::kill();

    auto* tm2 = ThemeManager::instance();
    ASSERT_NE(tm2, nullptr);
    // Fresh instance should still have valid colors
    EXPECT_TRUE(tm2->windowColor().isValid());
    EXPECT_TRUE(tm2->textColor().isValid());
}

TEST_F(ThemeManagerTests, AllColorsConsistent) {
    auto* tm = ThemeManager::instance();
    ASSERT_NE(tm, nullptr);

    // All color accessors should return valid QColor objects
    QList<QColor> colors = {
        tm->windowColor(),
        tm->panelColor(),
        tm->headerColor(),
        tm->inputColor(),
        tm->textColor(),
        tm->disabledTextColor(),
        tm->placeholderTextColor(),
        tm->highlightColor(),
        tm->highlightedTextColor(),
        tm->buttonColor(),
        tm->buttonTextColor(),
        tm->borderColor(),
        tm->accentColor()
    };

    for (const QColor& c : colors) {
        EXPECT_TRUE(c.isValid());
        // Alpha should be between 0 and 255
        EXPECT_GE(c.alpha(), 0);
        EXPECT_LE(c.alpha(), 255);
    }
}

TEST_F(ThemeManagerTests, RefreshThemeMultipleTimes) {
    auto* tm = ThemeManager::instance();
    ASSERT_NE(tm, nullptr);
    QSignalSpy spy(tm, &ThemeManager::themeChanged);

    tm->refreshTheme();
    tm->refreshTheme();
    tm->refreshTheme();

    EXPECT_EQ(spy.count(), 3);
}

TEST_F(ThemeManagerTests, ThemeNameIsLightOrDark) {
    auto* tm = ThemeManager::instance();
    ASSERT_NE(tm, nullptr);
    QString name = tm->themeName();
    EXPECT_TRUE(name == "light" || name == "dark");
}
