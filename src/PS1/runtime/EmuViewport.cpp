#include "EmuViewport.h"

#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>

EmuViewport::EmuViewport(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(320, 240);
    setAttribute(Qt::WA_OpaquePaintEvent);
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

void EmuViewport::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.fillRect(rect(), QColor(16, 16, 16));

    if (m_frame.isNull()) {
        painter.setPen(QColor(160, 160, 160));
        painter.drawText(rect(), Qt::AlignCenter, tr("No frame"));
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
}

void EmuViewport::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    update();
}
