#include <gtest/gtest.h>
#include "ThemeManager.h"
#include <QApplication>
#include <QCoreApplication>
#include <QPalette>
#include <QSettings>
#include <QColor>
#include <QString>

// Coverage-focused suite for ThemeManager::applyThemePreference(const QString&).
// Distinct fixture + suite name from ThemeManagerTests in ThemeManager_test.cpp
// to avoid ODR / duplicate-registration clashes. Targets branches not exercised
// by the existing suite: the "light" branch, the "custom"-with-invalid no-op,
// case/whitespace insensitivity, and the unknown-value fallthrough.
class ThemeManagerPrefCoverageTests : public ::testing::Test {
protected:
    QApplication* app = nullptr;
    QPalette originalPalette;

    void SetUp() override {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
        originalPalette = app->palette();
        // Start each test from a clean settings slate so a stale customPalette
        // from a prior test can't leak into the "custom"-invalid no-op case.
        QSettings settings;
        settings.clear();
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

// --- "dark" branch -----------------------------------------------------------

TEST_F(ThemeManagerPrefCoverageTests, DarkBranchSetsWindowAndHighlight) {
    ThemeManager::applyThemePreference(QStringLiteral("dark"));
    const QPalette& p = app->palette();
    EXPECT_EQ(p.color(QPalette::Window), QColor(53, 53, 53));
    EXPECT_EQ(p.color(QPalette::Highlight), QColor(42, 130, 218));
}

TEST_F(ThemeManagerPrefCoverageTests, DarkBranchSetsFullPalette) {
    ThemeManager::applyThemePreference(QStringLiteral("dark"));
    const QPalette& p = app->palette();
    EXPECT_EQ(p.color(QPalette::WindowText), QColor(Qt::white));
    EXPECT_EQ(p.color(QPalette::Base), QColor(35, 35, 35));
    EXPECT_EQ(p.color(QPalette::AlternateBase), QColor(53, 53, 53));
    EXPECT_EQ(p.color(QPalette::ToolTipBase), QColor(25, 25, 25));
    EXPECT_EQ(p.color(QPalette::Text), QColor(Qt::white));
    EXPECT_EQ(p.color(QPalette::Button), QColor(53, 53, 53));
    EXPECT_EQ(p.color(QPalette::ButtonText), QColor(Qt::white));
    EXPECT_EQ(p.color(QPalette::Link), QColor(42, 130, 218));
    EXPECT_EQ(p.color(QPalette::HighlightedText), QColor(Qt::black));
}

// --- "light" branch ----------------------------------------------------------

TEST_F(ThemeManagerPrefCoverageTests, LightBranchSetsGhostwhitePalette) {
    // First drive the palette to a known non-light state so we observe a change.
    ThemeManager::applyThemePreference(QStringLiteral("dark"));
    ASSERT_EQ(app->palette().color(QPalette::Window), QColor(53, 53, 53));

    ThemeManager::applyThemePreference(QStringLiteral("light"));

    // QApplication::setPalette(const QColor&) builds a palette whose Window
    // color is the supplied color (ghostwhite).
    const QColor ghostwhite(QStringLiteral("ghostwhite"));
    ASSERT_TRUE(ghostwhite.isValid());
    EXPECT_EQ(app->palette().color(QPalette::Window), ghostwhite);
    // ghostwhite is a light color, so themeName() should report "light".
    EXPECT_EQ(ThemeManager::instance()->themeName(), QString("light"));
}

// --- "custom" branch: valid palette ------------------------------------------

TEST_F(ThemeManagerPrefCoverageTests, CustomBranchWithValidPaletteApplied) {
    const QColor custom(17, 99, 200);
    QSettings settings;
    settings.setValue(QStringLiteral("customPalette"), custom);

    ThemeManager::applyThemePreference(QStringLiteral("custom"));

    EXPECT_EQ(app->palette().color(QPalette::Window), custom);
}

// --- "custom" branch: invalid / absent palette is a no-op --------------------

TEST_F(ThemeManagerPrefCoverageTests, CustomBranchWithAbsentPaletteIsNoOp) {
    // Pin the palette to a known marker; no customPalette set in QSettings.
    QPalette marker;
    marker.setColor(QPalette::Window, QColor(7, 8, 9));
    app->setPalette(marker);
    ASSERT_EQ(app->palette().color(QPalette::Window), QColor(7, 8, 9));

    QSettings settings;
    ASSERT_FALSE(settings.value(QStringLiteral("customPalette")).isValid());

    ThemeManager::applyThemePreference(QStringLiteral("custom"));

    // No valid custom color -> setPalette never called -> unchanged marker.
    EXPECT_EQ(app->palette().color(QPalette::Window), QColor(7, 8, 9));
}

TEST_F(ThemeManagerPrefCoverageTests, CustomBranchWithInvalidStoredValueIsNoOp) {
    // Store a value that does not convert to a valid QColor.
    QSettings settings;
    settings.setValue(QStringLiteral("customPalette"), QStringLiteral("not-a-color"));

    QPalette marker;
    marker.setColor(QPalette::Window, QColor(11, 22, 33));
    app->setPalette(marker);
    ASSERT_EQ(app->palette().color(QPalette::Window), QColor(11, 22, 33));

    ThemeManager::applyThemePreference(QStringLiteral("custom"));

    EXPECT_EQ(app->palette().color(QPalette::Window), QColor(11, 22, 33));
}

// --- case / whitespace insensitivity -----------------------------------------

TEST_F(ThemeManagerPrefCoverageTests, DarkIsTrimmedAndLowercased) {
    ThemeManager::applyThemePreference(QStringLiteral("  Dark  "));
    EXPECT_EQ(app->palette().color(QPalette::Window), QColor(53, 53, 53));
    EXPECT_EQ(app->palette().color(QPalette::Highlight), QColor(42, 130, 218));
}

TEST_F(ThemeManagerPrefCoverageTests, LightMixedCaseIsNormalized) {
    ThemeManager::applyThemePreference(QStringLiteral("dark"));
    ASSERT_EQ(app->palette().color(QPalette::Window), QColor(53, 53, 53));

    ThemeManager::applyThemePreference(QStringLiteral("\tLiGhT\n"));

    EXPECT_EQ(app->palette().color(QPalette::Window),
              QColor(QStringLiteral("ghostwhite")));
}

TEST_F(ThemeManagerPrefCoverageTests, CustomMixedCaseAndWhitespaceIsNormalized) {
    const QColor custom(200, 50, 75);
    QSettings settings;
    settings.setValue(QStringLiteral("customPalette"), custom);

    ThemeManager::applyThemePreference(QStringLiteral("  CUSTOM  "));

    EXPECT_EQ(app->palette().color(QPalette::Window), custom);
}

// --- unknown value fallthrough (no-op) ---------------------------------------

TEST_F(ThemeManagerPrefCoverageTests, UnknownValueIsNoOp) {
    QPalette marker;
    marker.setColor(QPalette::Window, QColor(1, 2, 3));
    app->setPalette(marker);
    ASSERT_EQ(app->palette().color(QPalette::Window), QColor(1, 2, 3));

    ThemeManager::applyThemePreference(QStringLiteral("blue"));

    EXPECT_EQ(app->palette().color(QPalette::Window), QColor(1, 2, 3));
}

TEST_F(ThemeManagerPrefCoverageTests, EmptyStringIsNoOp) {
    QPalette marker;
    marker.setColor(QPalette::Window, QColor(4, 5, 6));
    app->setPalette(marker);
    ASSERT_EQ(app->palette().color(QPalette::Window), QColor(4, 5, 6));

    ThemeManager::applyThemePreference(QString());

    EXPECT_EQ(app->palette().color(QPalette::Window), QColor(4, 5, 6));
}

TEST_F(ThemeManagerPrefCoverageTests, WhitespaceOnlyStringIsNoOp) {
    QPalette marker;
    marker.setColor(QPalette::Window, QColor(9, 8, 7));
    app->setPalette(marker);
    ASSERT_EQ(app->palette().color(QPalette::Window), QColor(9, 8, 7));

    ThemeManager::applyThemePreference(QStringLiteral("    "));

    EXPECT_EQ(app->palette().color(QPalette::Window), QColor(9, 8, 7));
}
