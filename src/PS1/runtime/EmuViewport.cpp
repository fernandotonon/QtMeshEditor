#include "EmuViewport.h"
#include "PsxJoypadState.h"

#include <QApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>

namespace {

constexpr unsigned kPort = 0;

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
    if (QApplication::keyboardGrabber() == this)
        releaseKeyboard();
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
    if (QApplication::keyboardGrabber() == this)
        releaseKeyboard();
    QWidget::focusOutEvent(event);
    update();
}

void EmuViewport::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.fillRect(rect(), QColor(16, 16, 16));

    if (m_frame.isNull()) {
        painter.setPen(QColor(160, 160, 160));
        painter.drawText(rect(), Qt::AlignCenter,
                         tr("No frame\nClick here, then use keyboard or mouse to control the emulator"));
        return;
    }

    const QSize src = m_frame.size();
    QSize dst = src;
    if (m_integerScale && src.width() > 0 && src.height() > 0) {
        const int scaleX = qMax(1, width() / src.width());
        const int scaleY = qMax(1, height() / src.height());
        const int scale = qMin(scaleX, scaleY);
        dst = QSize(src.width() * scale, src.height() * scale);
    } else {
        dst = src.scaled(size(), Qt::KeepAspectRatio);
    }

    const int x = (width() - dst.width()) / 2;
    const int y = (height() - dst.height()) / 2;
    const QRect target(x, y, dst.width(), dst.height());

    painter.setRenderHint(QPainter::SmoothPixmapTransform, !m_integerScale);
    painter.drawImage(target, m_frame);

    painter.setPen(QColor(220, 220, 220));
    painter.drawText(8, 20, tr("FPS: %1").arg(m_fps, 0, 'f', 1));

    if (m_showInputHelp) {
        const bool focused = hasFocus();
        painter.setPen(focused ? QColor(180, 255, 180) : QColor(255, 220, 120));
        const QString help = focused
                                 ? tr("Arrows · X/Enter/LMB=✕ · Z/RMB=○ · P=Start · Space=○ · Tab=Select")
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
