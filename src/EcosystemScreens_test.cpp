// Renders the Welcome and About dialogs in the light and dark palettes.
// Always checks they render; with QTMESH_UI_SNAPSHOT_DIR set it also writes
// the PNGs there (the review screenshots for UI changes).
#include <gtest/gtest.h>

#include "WelcomeDialog.h"
#include "about.h"

#include <QApplication>
#include <QDir>
#include <QLayout>
#include <QPixmap>
#include <QTabWidget>

namespace {
// Mirrors MainWindow::darkPalette(): Mid == Window and a grey Link role, the
// two things the screens must survive.
QPalette editorDarkPalette()
{
    QPalette p;
    p.setColor(QPalette::Window, QColor(37, 37, 37));
    p.setColor(QPalette::WindowText, QColor(212, 212, 212));
    p.setColor(QPalette::Base, QColor(60, 60, 60));
    p.setColor(QPalette::AlternateBase, QColor(45, 45, 45));
    p.setColor(QPalette::PlaceholderText, QColor(127, 127, 127));
    p.setColor(QPalette::Text, QColor(212, 212, 212));
    p.setColor(QPalette::Link, QColor(100, 100, 100));
    p.setColor(QPalette::Button, QColor(45, 45, 45));
    p.setColor(QPalette::ButtonText, QColor(212, 212, 212));
    p.setColor(QPalette::Highlight, QColor(38, 79, 120));
    p.setColor(QPalette::HighlightedText, QColor(240, 240, 240));
    p.setColor(QPalette::Light, QColor(60, 60, 60));
    p.setColor(QPalette::Mid, QColor(37, 37, 37));
    p.setColor(QPalette::Dark, QColor(30, 30, 30));
    return p;
}

void snapshot(QWidget& w, const QString& name)
{
    // The size the user first sees: the dialog's default, grown to whatever
    // its layout needs (never squeezed below it).
    w.ensurePolished();
    if (w.layout())
        w.layout()->activate();
    w.resize(w.size().expandedTo(w.minimumSizeHint()));
    const QPixmap shot = w.grab();
    EXPECT_FALSE(shot.isNull()) << name.toStdString();
    const QString dir = qEnvironmentVariable("QTMESH_UI_SNAPSHOT_DIR");
    if (!dir.isEmpty()) {
        QDir().mkpath(dir);
        EXPECT_TRUE(shot.save(QDir(dir).filePath(name + QStringLiteral(".png"))));
    }
}

void renderAll(const QString& theme)
{
    {
        WelcomeDialog welcome;
        welcome.setDiscoverySuggestion(
            QVariantMap{{"featureKey", "uv_unwrap"}, {"title", "Auto UV Unwrap"},
                        {"blurb", "Unwrap any mesh into clean, non-overlapping UVs in one click."},
                        {"personalized", false}});
        snapshot(welcome, QStringLiteral("welcome_") + theme);
    }
    About about;
    auto* tabs = about.findChild<QTabWidget*>();
    ASSERT_NE(tabs, nullptr);
    for (int i = 0; i < tabs->count(); ++i) {
        tabs->setCurrentIndex(i);
        snapshot(about, QStringLiteral("about_%1_%2").arg(i).arg(theme));
    }
}
} // namespace

TEST(EcosystemScreens, RenderInLightAndDarkPalettes)
{
    const QPalette original = QApplication::palette();
    renderAll(QStringLiteral("light"));
    QApplication::setPalette(editorDarkPalette());
    renderAll(QStringLiteral("dark"));
    QApplication::setPalette(original);
}
