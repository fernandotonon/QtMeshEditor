#include "EmuViewport.h"
#include "PsxJoypadState.h"

#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>

namespace {

constexpr unsigned kPort = 0;

QRect aspectContentArea(const QSize &widgetSize, EmuViewport::AspectMode aspect)
{
    if (widgetSize.width() < 1 || widgetSize.height() < 1)
        return {};

    QRect area(0, 0, widgetSize.width(), widgetSize.height());
    if (aspect != EmuViewport::AspectMode::Display43)
        return area;

    constexpr qreal kDisplayAspect = 4.0 / 3.0;
    const qreal widgetAspect =
        static_cast<qreal>(widgetSize.width()) / static_cast<qreal>(widgetSize.height());
    if (widgetAspect > kDisplayAspect) {
        const int w = static_cast<int>(widgetSize.height() * kDisplayAspect);
        area = QRect((widgetSize.width() - w) / 2, 0, w, widgetSize.height());
    } else if (widgetAspect < kDisplayAspect) {
        const int h = static_cast<int>(widgetSize.width() / kDisplayAspect);
        area = QRect(0, (widgetSize.height() - h) / 2, widgetSize.width(), h);
    }
    return area;
}

void setButton(unsigned id, bool pressed)
{
    PsxJoypadState::setPressed(kPort, id, pressed);
}

bool mapKey(Qt::Key key, unsigned *buttonOut)
{
    switch (key) {
    case Qt::Key_Up:
        *buttonOut = PsxJoypadButton::Up;
        return true;
    case Qt::Key_Down:
        *buttonOut = PsxJoypadButton::Down;
        return true;
    case Qt::Key_Left:
        *buttonOut = PsxJoypadButton::Left;
        return true;
    case Qt::Key_Right:
        *buttonOut = PsxJoypadButton::Right;
        return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
    case Qt::Key_X:
        *buttonOut = PsxJoypadButton::B;
        return true;
    case Qt::Key_Z:
    case Qt::Key_Escape:
    case Qt::Key_Backspace:
    case Qt::Key_C:
        *buttonOut = PsxJoypadButton::A;
        return true;
    case Qt::Key_S:
        *buttonOut = PsxJoypadButton::X;
        return true;
    case Qt::Key_D:
    case Qt::Key_A:
        *buttonOut = PsxJoypadButton::Y;
        return true;
    case Qt::Key_Q:
        *buttonOut = PsxJoypadButton::L;
        return true;
    case Qt::Key_W:
        *buttonOut = PsxJoypadButton::R;
        return true;
    case Qt::Key_E:
        *buttonOut = PsxJoypadButton::L2;
        return true;
    case Qt::Key_R:
        *buttonOut = PsxJoypadButton::R2;
        return true;
    case Qt::Key_1:
        *buttonOut = PsxJoypadButton::L3;
        return true;
    case Qt::Key_2:
        *buttonOut = PsxJoypadButton::R3;
        return true;
    case Qt::Key_Tab:
    case Qt::Key_Shift:
        *buttonOut = PsxJoypadButton::Select;
        return true;
    case Qt::Key_P:
        *buttonOut = PsxJoypadButton::Start;
        return true;
    default:
        return false;
    }
}

bool mapMouse(Qt::MouseButton button, unsigned *buttonOut)
{
    switch (button) {
    case Qt::LeftButton:
        *buttonOut = PsxJoypadButton::B;
        return true;
    case Qt::RightButton:
        *buttonOut = PsxJoypadButton::A;
        return true;
    case Qt::MiddleButton:
        *buttonOut = PsxJoypadButton::Start;
        return true;
    default:
        return false;
    }
}

} // namespace

EmuViewport::FrameLayout EmuViewport::computeFrameLayout(const QSize &widgetSize,
                                                       const QSize &frameSize, bool integerScale,
                                                       bool smoothFiltering, AspectMode aspect)
{
    FrameLayout layout;
    if (widgetSize.width() < 1 || widgetSize.height() < 1 || frameSize.width() < 1
        || frameSize.height() < 1)
        return layout;

    const QRect content = aspectContentArea(widgetSize, aspect);
    if (content.isEmpty())
        return layout;

    QSize dst = frameSize;
    if (integerScale) {
        const int scaleX = qMax(1, content.width() / frameSize.width());
        const int scaleY = qMax(1, content.height() / frameSize.height());
        const int scale = qMin(scaleX, scaleY);
        dst = QSize(frameSize.width() * scale, frameSize.height() * scale);
        layout.smoothFiltering = false;
    } else {
        dst = frameSize.scaled(content.size(), Qt::KeepAspectRatio);
        layout.smoothFiltering = smoothFiltering;
    }

    const int x = content.x() + (content.width() - dst.width()) / 2;
    const int y = content.y() + (content.height() - dst.height()) / 2;
    layout.target = QRect(x, y, dst.width(), dst.height());
    return layout;
}

EmuViewport::EmuViewport(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(320, 240);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(false);
}

void EmuViewport::setFrame(const QImage &frame)
{
    if (frame.isNull())
        return;
    m_frame = frame;
    update();
}

void EmuViewport::setIntegerScale(bool enabled)
{
    m_integerScale = enabled;
    update();
}

void EmuViewport::setSmoothFiltering(bool enabled)
{
    m_smoothFiltering = enabled;
    update();
}

void EmuViewport::setAspectMode(AspectMode mode)
{
    m_aspectMode = mode;
    update();
}

void EmuViewport::setFps(double fps)
{
    m_fps = fps;
    update();
}

void EmuViewport::setShowInputHelp(bool show)
{
    m_showInputHelp = show;
    update();
}

void EmuViewport::applyKey(Qt::Key key, bool pressed)
{
    unsigned button = 0;
    if (mapKey(key, &button))
        setButton(button, pressed);
}

void EmuViewport::applyMouseButton(Qt::MouseButton button, bool pressed)
{
    unsigned mapped = 0;
    if (mapMouse(button, &mapped))
        setButton(mapped, pressed);
}

void EmuViewport::keyPressEvent(QKeyEvent *event)
{
    if (!event->isAutoRepeat()) {
        applyKey(static_cast<Qt::Key>(event->key()), true);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void EmuViewport::keyReleaseEvent(QKeyEvent *event)
{
    if (!event->isAutoRepeat()) {
        applyKey(static_cast<Qt::Key>(event->key()), false);
        event->accept();
        return;
    }
    QWidget::keyReleaseEvent(event);
}

void EmuViewport::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);
    grabKeyboard();
    applyMouseButton(event->button(), true);
    event->accept();
}

void EmuViewport::mouseReleaseEvent(QMouseEvent *event)
{
    applyMouseButton(event->button(), false);
    event->accept();
}

void EmuViewport::focusInEvent(QFocusEvent *event)
{
    QWidget::focusInEvent(event);
    update();
}

void EmuViewport::focusOutEvent(QFocusEvent *event)
{
    PsxJoypadState::resetAll();
    releaseKeyboard();
    QWidget::focusOutEvent(event);
    update();
}

void EmuViewport::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.fillRect(rect(), QColor(16, 16, 16));

    if (m_aspectMode == AspectMode::Display43) {
        const QRect content = aspectContentArea(size(), m_aspectMode);
        if (!content.isEmpty())
            painter.fillRect(content, QColor(8, 8, 8));
    }

    if (m_frame.isNull()) {
        painter.setPen(QColor(160, 160, 160));
        painter.drawText(rect(), Qt::AlignCenter,
                         tr("No frame\nClick here, then use keyboard or mouse to control the emulator"));
        return;
    }

    const FrameLayout layout =
        computeFrameLayout(size(), m_frame.size(), m_integerScale, m_smoothFiltering, m_aspectMode);
    if (layout.target.isEmpty())
        return;

    painter.setRenderHint(QPainter::SmoothPixmapTransform, layout.smoothFiltering);
    painter.drawImage(layout.target, m_frame);

    painter.setPen(QColor(220, 220, 220));
    painter.drawText(8, 20, tr("FPS: %1").arg(m_fps, 0, 'f', 1));

    if (m_showInputHelp) {
        const bool focused = hasFocus();
        painter.setPen(focused ? QColor(180, 255, 180) : QColor(255, 220, 120));
        const QString help = focused
                                 ? tr("Arrows · X/Enter/Space/LMB=✕ · Z/RMB=○ · P=Start · Tab=Select")
                                 : tr("Click viewport for input");
        const QRect helpRect(8, height() - 28, width() - 16, 20);
        painter.drawText(helpRect, Qt::AlignLeft | Qt::AlignVCenter, help);
    }
}

void EmuViewport::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    update();
}
