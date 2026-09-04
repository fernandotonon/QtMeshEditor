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

#include "ViewportTitleBar.h"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QStyle>
#include <QToolButton>

namespace {
constexpr int kBarHeight        = 15;
constexpr int kButtonWidth      = 22;
constexpr int kButtonHeight     = 13;
constexpr int kIconButtonWidth  = 16;
constexpr int kHorizontalMargin = 4;
constexpr int kButtonSpacing    = 2;
constexpr int kLabelFontDelta   = -1; ///< px shrink off default font for compactness
constexpr int kViewportButtonFontPixelSize = 8;

const char* kTitleButtonStyle = R"(
    QToolButton {
        border: 1px solid #666666;
        border-radius: 2px;
        background: #3b3b3b;
        color: #d8d8d8;
        padding: 0;
    }
    QToolButton:hover {
        border-color: #8a8a8a;
        background: #4a4a4a;
    }
    QToolButton:checked {
        border-color: #8a8a8a;
        background: #4a4a4a;
        color: #f2f2f2;
    }
    QToolButton:pressed {
        background: #2f2f2f;
    }
    QToolButton:disabled {
        color: #7a7a7a;
    }
)";

const char* kTitleBarStyle = R"(
    QWidget#viewportTitleBar {
        background: #2d2d2d;
    }
    QLabel#viewportTitleLabel {
        color: #d8d8d8;
        background: transparent;
    }
)";
}

ViewportTitleBar::ViewportTitleBar(QDockWidget* dock,
                                   QAction* gridAction,
                                   QAction* normalsAction,
                                   QAction* meshInfoAction,
                                   QAction* viewCubeAction,
                                   QAction* lightsAction,
                                   QWidget* parent)
    : QWidget(parent ? parent : dock)
    , m_dock(dock)
{
    setObjectName(QStringLiteral("viewportTitleBar"));
    setAutoFillBackground(true);
    setFixedHeight(kBarHeight);
    setStyleSheet(QString::fromLatin1(kTitleBarStyle));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(kHorizontalMargin, 0, 2, 0);
    layout->setSpacing(kButtonSpacing);

    m_titleLabel = new QLabel(this);
    m_titleLabel->setObjectName(QStringLiteral("viewportTitleLabel"));
    if (m_dock)
        m_titleLabel->setText(m_dock->windowTitle());
    QFont labelFont = font();
    if (labelFont.pixelSize() > 0)
        labelFont.setPixelSize(qMax(8, labelFont.pixelSize() + kLabelFontDelta));
    else
        labelFont.setPointSize(qMax(7, labelFont.pointSize() - 1));
    m_titleLabel->setFont(labelFont);
    m_titleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(m_titleLabel, /*stretch*/ 1);

    if (m_dock) {
        connect(m_dock, &QDockWidget::windowTitleChanged,
                m_titleLabel, &QLabel::setText);
    }

    m_gridButton     = makeActionButton(gridAction,     QStringLiteral("G"));
    m_normalsButton  = makeActionButton(normalsAction,  QStringLiteral("N"));
    m_meshInfoButton = makeActionButton(meshInfoAction, QStringLiteral("I"));
    m_viewCubeButton = makeActionButton(viewCubeAction, QStringLiteral("C"));
    m_lightsButton   = makeActionButton(lightsAction,   QStringLiteral("L"));
    if (m_gridButton)     layout->addWidget(m_gridButton);
    if (m_normalsButton)  layout->addWidget(m_normalsButton);
    if (m_meshInfoButton) layout->addWidget(m_meshInfoButton);
    if (m_viewCubeButton) layout->addWidget(m_viewCubeButton);
    if (m_lightsButton)   layout->addWidget(m_lightsButton);

    if (m_dock) {
        // Re-implement the float/close buttons that `setTitleBarWidget`
        // removed. Use QStyle pixmaps so the icons follow the platform/theme.
        m_floatButton = new QToolButton(this);
        m_floatButton->setObjectName(QStringLiteral("viewportFloatButton"));
        m_floatButton->setAutoRaise(true);
        m_floatButton->setFixedSize(kIconButtonWidth, kButtonHeight);
        m_floatButton->setFocusPolicy(Qt::NoFocus);
        m_floatButton->setStyleSheet(QString::fromLatin1(kTitleButtonStyle));
        m_floatButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarNormalButton));
        m_floatButton->setToolTip(tr("Float / dock viewport"));
        connect(m_floatButton, &QToolButton::clicked, m_dock, [this]() {
            if (m_dock)
                m_dock->setFloating(!m_dock->isFloating());
        });
        layout->addWidget(m_floatButton);

        m_closeButton = new QToolButton(this);
        m_closeButton->setObjectName(QStringLiteral("viewportCloseButton"));
        m_closeButton->setAutoRaise(true);
        m_closeButton->setFixedSize(kIconButtonWidth, kButtonHeight);
        m_closeButton->setFocusPolicy(Qt::NoFocus);
        m_closeButton->setStyleSheet(QString::fromLatin1(kTitleButtonStyle));
        m_closeButton->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
        m_closeButton->setToolTip(tr("Close viewport"));
        connect(m_closeButton, &QToolButton::clicked, m_dock, &QDockWidget::close);
        layout->addWidget(m_closeButton);
    }
}

QToolButton* ViewportTitleBar::makeActionButton(QAction* action,
                                                const QString& letterLabel)
{
    if (!action)
        return nullptr;

    auto* button = new QToolButton(this);
    button->setObjectName(QStringLiteral("viewport") + letterLabel + QStringLiteral("Button"));
    button->setAutoRaise(true);
    button->setCheckable(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setFixedSize(kButtonWidth, kButtonHeight);
    button->setText(letterLabel);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setStyleSheet(QString::fromLatin1(kTitleButtonStyle));
    button->setToolTip(action->toolTip().isEmpty() ? action->text() : action->toolTip());
    QFont buttonFont = button->font();
    buttonFont.setPixelSize(kViewportButtonFontPixelSize);
    button->setFont(buttonFont);

    // Mirror the QAction state — we deliberately do NOT use `setDefaultAction`
    // because that would also adopt the action's full text/icon and override
    // our compact letter glyph. Instead, sync state both directions manually.
    button->setChecked(action->isChecked());
    button->setEnabled(action->isEnabled());

    connect(action, &QAction::changed, button, [button, action]() {
        const QSignalBlocker blocker(button);
        button->setChecked(action->isChecked());
        button->setEnabled(action->isEnabled());
    });
    connect(action, &QAction::toggled, button, [button](bool on) {
        const QSignalBlocker blocker(button);
        button->setChecked(on);
    });
    connect(button, &QToolButton::clicked, action, [action]() {
        action->trigger();
    });

    return button;
}

void ViewportTitleBar::mousePressEvent(QMouseEvent* event)
{
    // Treat presses on the title label (or the empty strip) as the drag
    // handle — anywhere a child toolbutton answers `childAt` is excluded so
    // clicks on G/N/I/C/float/close fall through to the buttons.
    QWidget* hit = childAt(event->pos());
    const bool onDragArea = (hit == nullptr || hit == m_titleLabel);
    m_pressedOnDragArea = (event->button() == Qt::LeftButton) && onDragArea && m_dock;

    if (m_pressedOnDragArea) {
        m_dragStartGlobal = event->globalPosition().toPoint();
        // Track offset between cursor and dock top-left so detaching feels
        // continuous rather than snapping the floating window to the cursor.
        m_dragOffset = event->globalPosition().toPoint() - m_dock->mapToGlobal(QPoint(0, 0));
    }

    QWidget::mousePressEvent(event);
}

void ViewportTitleBar::mouseMoveEvent(QMouseEvent* event)
{
    if (m_pressedOnDragArea && m_dock && (event->buttons() & Qt::LeftButton)) {
        const QPoint delta = event->globalPosition().toPoint() - m_dragStartGlobal;
        const int distance = qMax(qAbs(delta.x()), qAbs(delta.y()));

        if (!m_dock->isFloating() && distance > QApplication::startDragDistance()) {
            // Detach into a floating window. We resize-then-float (instead
            // of relying on `setFloating(true)`'s default placement) so the
            // window appears under the cursor where the drag started.
            m_dock->setFloating(true);
        }

        if (m_dock->isFloating()) {
            m_dock->move(event->globalPosition().toPoint() - m_dragOffset);
        }
    }

    QWidget::mouseMoveEvent(event);
}

void ViewportTitleBar::mouseReleaseEvent(QMouseEvent* event)
{
    m_pressedOnDragArea = false;
    QWidget::mouseReleaseEvent(event);
}

void ViewportTitleBar::mouseDoubleClickEvent(QMouseEvent* event)
{
    QWidget* hit = childAt(event->pos());
    if (event->button() == Qt::LeftButton && m_dock
        && (hit == nullptr || hit == m_titleLabel))
    {
        // Double-click on title toggles floating, matching Qt's default
        // QDockWidget title-bar behaviour.
        m_dock->setFloating(!m_dock->isFloating());
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}
