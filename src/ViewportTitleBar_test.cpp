#include <gtest/gtest.h>
#include <QAction>
#include <QApplication>
#include <QCoreApplication>
#include <QDockWidget>
#include <QLabel>
#include <QMainWindow>
#include <QSignalSpy>
#include <QToolButton>

#include "ViewportTitleBar.h"

class ViewportTitleBarTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_NE(qobject_cast<QApplication*>(QCoreApplication::instance()), nullptr);
    }
};

TEST_F(ViewportTitleBarTest, NullActionsProduceNullButtons)
{
    QDockWidget dock(QStringLiteral("Test Viewport"));
    ViewportTitleBar bar(&dock, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(bar.gridButton(), nullptr);
    EXPECT_EQ(bar.normalsButton(), nullptr);
    EXPECT_EQ(bar.meshInfoButton(), nullptr);
    EXPECT_EQ(bar.viewCubeButton(), nullptr);
    // Float / close still created because dock is non-null
    EXPECT_NE(bar.floatButton(), nullptr);
    EXPECT_NE(bar.closeButton(), nullptr);
}

TEST_F(ViewportTitleBarTest, NullDockMeansNoFloatOrCloseButtons)
{
    ViewportTitleBar bar(nullptr, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(bar.floatButton(), nullptr);
    EXPECT_EQ(bar.closeButton(), nullptr);
    // Title label always exists
    EXPECT_NE(bar.titleLabel(), nullptr);
}

TEST_F(ViewportTitleBarTest, TitleLabelMirrorsDockTitle)
{
    QDockWidget dock(QStringLiteral("My Title"));
    ViewportTitleBar bar(&dock, nullptr, nullptr, nullptr, nullptr);
    ASSERT_NE(bar.titleLabel(), nullptr);
    EXPECT_EQ(bar.titleLabel()->text(), QStringLiteral("My Title"));

    // Changing the dock's title should propagate to the bar's label.
    dock.setWindowTitle(QStringLiteral("New Title"));
    QCoreApplication::processEvents();
    EXPECT_EQ(bar.titleLabel()->text(), QStringLiteral("New Title"));
}

TEST_F(ViewportTitleBarTest, ButtonReflectsActionInitialState)
{
    QDockWidget dock;
    QAction grid;
    grid.setCheckable(true);
    grid.setChecked(true);
    grid.setText(QStringLiteral("Grid"));
    ViewportTitleBar bar(&dock, &grid, nullptr, nullptr, nullptr);
    ASSERT_NE(bar.gridButton(), nullptr);
    EXPECT_TRUE(bar.gridButton()->isCheckable());
    EXPECT_TRUE(bar.gridButton()->isChecked());
    EXPECT_EQ(bar.gridButton()->text(), QStringLiteral("G"));
}

TEST_F(ViewportTitleBarTest, ActionToggleSyncsToButton)
{
    QDockWidget dock;
    QAction grid;
    grid.setCheckable(true);
    grid.setChecked(false);
    ViewportTitleBar bar(&dock, &grid, nullptr, nullptr, nullptr);
    ASSERT_NE(bar.gridButton(), nullptr);
    EXPECT_FALSE(bar.gridButton()->isChecked());

    grid.setChecked(true);
    EXPECT_TRUE(bar.gridButton()->isChecked());

    grid.setEnabled(false);
    EXPECT_FALSE(bar.gridButton()->isEnabled());
}

TEST_F(ViewportTitleBarTest, ButtonClickTriggersAction)
{
    QDockWidget dock;
    QAction normals;
    normals.setCheckable(true);
    QSignalSpy triggered(&normals, &QAction::triggered);
    ViewportTitleBar bar(&dock, nullptr, &normals, nullptr, nullptr);
    ASSERT_NE(bar.normalsButton(), nullptr);
    bar.normalsButton()->click();
    EXPECT_EQ(triggered.count(), 1);
}

TEST_F(ViewportTitleBarTest, CloseButtonClosesDock)
{
    QDockWidget dock(QStringLiteral("Vis"));
    dock.show();
    ViewportTitleBar bar(&dock, nullptr, nullptr, nullptr, nullptr);
    ASSERT_NE(bar.closeButton(), nullptr);
    bar.closeButton()->click();
    QCoreApplication::processEvents();
    EXPECT_FALSE(dock.isVisible());
}

TEST_F(ViewportTitleBarTest, FloatButtonTogglesFloating)
{
    // Dock needs a real QMainWindow parent: a parentless dock is permanently
    // floating, and toggling it has no observable effect (nowhere to dock to).
    QMainWindow window;
    auto* dock = new QDockWidget(QStringLiteral("Vis"), &window);
    window.addDockWidget(Qt::RightDockWidgetArea, dock);
    ViewportTitleBar bar(dock, nullptr, nullptr, nullptr, nullptr);
    ASSERT_NE(bar.floatButton(), nullptr);

    const bool before = dock->isFloating();   // false — docked into window
    bar.floatButton()->click();
    QCoreApplication::processEvents();
    EXPECT_NE(dock->isFloating(), before);    // toggled to true

    bar.floatButton()->click();
    QCoreApplication::processEvents();
    EXPECT_EQ(dock->isFloating(), before);    // toggled back
}

TEST_F(ViewportTitleBarTest, ActionToolTipPropagatesToButton)
{
    QDockWidget dock;
    QAction info;
    info.setCheckable(true);
    info.setText(QStringLiteral("Mesh Info"));
    info.setToolTip(QStringLiteral("Toggle the floating mesh-info overlay"));
    ViewportTitleBar bar(&dock, nullptr, nullptr, &info, nullptr);
    ASSERT_NE(bar.meshInfoButton(), nullptr);
    EXPECT_EQ(bar.meshInfoButton()->toolTip(),
              QStringLiteral("Toggle the floating mesh-info overlay"));
}

TEST_F(ViewportTitleBarTest, EmptyTooltipFallsBackToActionText)
{
    QDockWidget dock;
    QAction cube;
    cube.setCheckable(true);
    cube.setText(QStringLiteral("View Cube"));
    // No setToolTip — should fall back to action text.
    ViewportTitleBar bar(&dock, nullptr, nullptr, nullptr, &cube);
    ASSERT_NE(bar.viewCubeButton(), nullptr);
    EXPECT_EQ(bar.viewCubeButton()->toolTip(), QStringLiteral("View Cube"));
}

TEST_F(ViewportTitleBarTest, LightsToggleButtonBindsToAction)
{
    // The lights-overlay toggle lives in the viewport title strip like
    // Grid/Normals/Info — toggling the button drives the action and the
    // action's state reflects back onto the button.
    QDockWidget dock;
    QAction lights(QStringLiteral("Show Light Icons"), nullptr);
    lights.setCheckable(true);
    lights.setChecked(true);
    ViewportTitleBar bar(&dock, nullptr, nullptr, nullptr, nullptr, &lights);
    ASSERT_NE(bar.lightsButton(), nullptr);
    EXPECT_TRUE(bar.lightsButton()->isChecked());
    bar.lightsButton()->click();
    EXPECT_FALSE(lights.isChecked());
    lights.setChecked(true);
    EXPECT_TRUE(bar.lightsButton()->isChecked());

    // Omitting the action (old ctor shape) creates no button.
    ViewportTitleBar bare(&dock, nullptr, nullptr, nullptr, nullptr);
    EXPECT_EQ(bare.lightsButton(), nullptr);
}
