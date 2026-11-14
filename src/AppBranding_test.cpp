#include "AppBranding.h"
#include "WelcomeDialog.h"
#include "about.h"

#include <gtest/gtest.h>

#include <QLabel>

TEST(AppBranding, DisplayNameAndTechnicalNameStaySeparate)
{
    EXPECT_EQ(AppBranding::displayName(), QStringLiteral("QtMesh Editor"));
    // Persisted identifier (QSettings org/app name, AppData, credentials):
    // changing it would orphan existing installs.
    EXPECT_EQ(AppBranding::technicalName(), QStringLiteral("QtMeshEditor"));
}

TEST(AppBranding, WelcomeAndAboutShowTheDisplayName)
{
    WelcomeDialog welcome;
    EXPECT_EQ(welcome.windowTitle(), QStringLiteral("Welcome to QtMesh Editor"));

    About about;
    EXPECT_EQ(about.windowTitle(), QStringLiteral("About QtMesh Editor"));

    for (QWidget* w : {static_cast<QWidget*>(&welcome), static_cast<QWidget*>(&about)}) {
        bool sawName = false;
        for (auto* label : w->findChildren<QLabel*>()) {
            sawName |= label->text() == AppBranding::displayName();
            // No user-facing label should still use the old run-together name
            // (links to the github.com/.../QtMeshEditor repository are fine).
            const QString plain = QString(label->text()).remove(
                QStringLiteral("github.com/fernandotonon/QtMeshEditor"));
            EXPECT_FALSE(plain.contains(QStringLiteral("QtMeshEditor")))
                << label->text().toStdString();
        }
        EXPECT_TRUE(sawName) << w->windowTitle().toStdString();
    }
}
