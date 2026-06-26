#include <gtest/gtest.h>
#include "ThemeManager.h"
#include <QApplication>
#include <QCoreApplication>
#include <QPalette>
#include <QSignalSpy>
#include <QSettings>

class ThemeManagerTests : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    QPalette originalPalette;

    void SetUp() override {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        originalPalette = app->palette();
    }

    void TearDown() override {
        if (app) {
            app->setPalette(originalPalette);
            app->processEvents();
        }
        QSettings settings;
        settings.clear();
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

TEST_F(ThemeManagerTests, ColorsMatchCurrentPalette) {
    auto* tm = ThemeManager::instance();
    ASSERT_NE(tm, nullptr);

    QPalette palette = app->palette();
    const QColor window(245, 246, 247);
    const QColor base(32, 42, 52);
    const QColor text(12, 22, 32);
    const QColor disabled(62, 72, 82);
    const QColor placeholder(92, 102, 112);
    const QColor highlight(122, 132, 142);
    const QColor highlightedText(202, 212, 222);
    const QColor button(152, 162, 172);
    const QColor buttonText(182, 192, 202);
    const QColor mid(111, 121, 131);

    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    palette.setColor(QPalette::PlaceholderText, placeholder);
    palette.setColor(QPalette::Highlight, highlight);
    palette.setColor(QPalette::HighlightedText, highlightedText);
    palette.setColor(QPalette::Button, button);
    palette.setColor(QPalette::ButtonText, buttonText);
    palette.setColor(QPalette::Mid, mid);
    app->setPalette(palette);

    EXPECT_EQ(tm->windowColor(), window);
    EXPECT_EQ(tm->panelColor(), window);
    EXPECT_EQ(tm->headerColor(), window.darker(110));
    EXPECT_EQ(tm->inputColor(), base);
    EXPECT_EQ(tm->textColor(), text);
    EXPECT_EQ(tm->disabledTextColor(), disabled);
    EXPECT_EQ(tm->placeholderTextColor(), placeholder);
    EXPECT_EQ(tm->highlightColor(), highlight);
    EXPECT_EQ(tm->highlightedTextColor(), highlightedText);
    EXPECT_EQ(tm->buttonColor(), button);
    EXPECT_EQ(tm->buttonTextColor(), buttonText);
    EXPECT_EQ(tm->borderColor(), mid);
    EXPECT_EQ(tm->accentColor(), highlight);
}

TEST_F(ThemeManagerTests, RefreshThemeEmitsSignal) {
    auto* tm = ThemeManager::instance();
    ASSERT_NE(tm, nullptr);
    QSignalSpy spy(tm, &ThemeManager::themeChanged);
    tm->refreshTheme();
    EXPECT_EQ(spy.count(), 1);
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

TEST_F(ThemeManagerTests, ThemeNameTracksPaletteLightness) {
    auto* tm = ThemeManager::instance();
    ASSERT_NE(tm, nullptr);

    QPalette palette = app->palette();
    palette.setColor(QPalette::Window, QColor(250, 250, 250));
    app->setPalette(palette);
    EXPECT_EQ(tm->themeName(), QString("light"));

    palette.setColor(QPalette::Window, QColor(20, 20, 20));
    app->setPalette(palette);
    EXPECT_EQ(tm->themeName(), QString("dark"));
}

TEST_F(ThemeManagerTests, ApplySavedThemeFromSettingsUsesAppearanceTheme)
{
    QSettings settings;
    settings.setValue(QStringLiteral("palette"), QStringLiteral("light"));
    settings.setValue(QStringLiteral("Appearance/theme"), QStringLiteral("Dark"));

    ThemeManager::applySavedThemeFromSettings();

    EXPECT_EQ(app->palette().color(QPalette::Window), QColor(53, 53, 53));
    EXPECT_EQ(app->palette().color(QPalette::Text), QColor(Qt::white));
}

TEST_F(ThemeManagerTests, ApplySavedThemeFromSettingsUsesCustomLegacyPalette)
{
    QSettings settings;
    const QColor custom(12, 34, 56);
    settings.setValue(QStringLiteral("palette"), QStringLiteral("custom"));
    settings.setValue(QStringLiteral("customPalette"), custom);

    ThemeManager::applySavedThemeFromSettings();

    EXPECT_EQ(app->palette().color(QPalette::Window), custom);
}

TEST_F(ThemeManagerTests, ApplySavedThemeFromSettingsPrefersCustomPaletteOverStaleAppearanceTheme)
{
    QSettings settings;
    const QColor custom(98, 76, 54);
    settings.setValue(QStringLiteral("palette"), QStringLiteral("custom"));
    settings.setValue(QStringLiteral("customPalette"), custom);
    settings.setValue(QStringLiteral("Appearance/theme"), QStringLiteral("Light"));

    ThemeManager::applySavedThemeFromSettings();

    EXPECT_EQ(app->palette().color(QPalette::Window), custom);
}
