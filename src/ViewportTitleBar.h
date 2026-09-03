/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------------
*/

#ifndef VIEWPORT_TITLE_BAR_H
#define VIEWPORT_TITLE_BAR_H

#include <QWidget>

class QAction;
class QDockWidget;
class QLabel;
class QToolButton;

/// Custom title-bar widget used by `EditorViewport` (a `QDockWidget`).
///
/// Replaces Qt's default dock title bar so the per-viewport display
/// controls (Show Grid / Show Normals / Show Mesh Info / Show View Cube /
/// Show Lights) live
/// directly in the title strip instead of as a separate floating widget. This avoids the
/// long-standing compositing issue with `OgreWidget` (which uses
/// `WA_PaintOnScreen` and produced black rectangles for any overlapping Qt
/// child widget) and frees the top-right corner of the viewport for the
/// View Cube without needing manual collision avoidance.
///
/// Because `setTitleBarWidget` removes Qt's built-in float/close buttons, we
/// re-create them here and wire them to `QDockWidget::setFloating()` /
/// `QDockWidget::close()` using `QStyle` icons so the look adapts to the
/// active palette.
class ViewportTitleBar : public QWidget
{
    Q_OBJECT

public:
    /// Builds a title bar bound to `dock`. Viewport actions may be null (in
    /// which case no button is created for that slot — useful for tests that
    /// don't construct a full MainWindow).
    ViewportTitleBar(QDockWidget* dock,
                     QAction* gridAction,
                     QAction* normalsAction,
                     QAction* meshInfoAction,
                     QAction* viewCubeAction,
                     QAction* lightsAction = nullptr,
                     QWidget* parent = nullptr);

    /// Returns the toolbutton bound to the corresponding QAction (or nullptr
    /// if no action was provided at construction). Exposed for unit tests.
    QToolButton* gridButton() const { return m_gridButton; }
    QToolButton* normalsButton() const { return m_normalsButton; }
    QToolButton* meshInfoButton() const { return m_meshInfoButton; }
    QToolButton* viewCubeButton() const { return m_viewCubeButton; }
    QToolButton* lightsButton() const { return m_lightsButton; }
    QToolButton* floatButton() const { return m_floatButton; }
    QToolButton* closeButton() const { return m_closeButton; }
    QLabel* titleLabel() const { return m_titleLabel; }

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    /// Creates a small toolbar-style button bound to `action`. Updates the
    /// button's label/checked/enabled state from the action and triggers the
    /// action when clicked. Returns nullptr if `action` is null.
    QToolButton* makeActionButton(QAction* action, const QString& letterLabel);

    QDockWidget* m_dock;
    QLabel*      m_titleLabel = nullptr;
    QToolButton* m_gridButton = nullptr;
    QToolButton* m_normalsButton = nullptr;
    QToolButton* m_meshInfoButton = nullptr;
    QToolButton* m_viewCubeButton = nullptr;
    QToolButton* m_lightsButton = nullptr;
    QToolButton* m_floatButton = nullptr;
    QToolButton* m_closeButton = nullptr;

    /// Drag-to-detach state. While the dock is docked, pressing on the
    /// title strip (not on a button) and dragging past the system drag
    /// distance promotes the dock to floating and lets the user move it.
    /// Re-docking by drop is not implemented — use the float button to
    /// re-dock.
    bool   m_pressedOnDragArea = false;
    QPoint m_dragStartGlobal;
    QPoint m_dragOffset;
};

#endif // VIEWPORT_TITLE_BAR_H
