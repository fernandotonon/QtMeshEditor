#include "animationcontrolslider.h"

AnimationControlSlider::AnimationControlSlider(QWidget *parent)
    : QSlider(parent) {}

void AnimationControlSlider::addTick(int value, QColor color)
{
    m_ticks.push_back(std::make_pair(value, color));
}

void AnimationControlSlider::clearTicks()
{
    m_ticks.clear();
    m_selectedTick = -1;
    update();
}

void AnimationControlSlider::setSelectedTick(int value)
{
    if (m_selectedTick != value) {
        m_selectedTick = value;
        update();
    }
}

void AnimationControlSlider::paintEvent(QPaintEvent *event)
{
    QSlider::paintEvent(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    for(const auto &[tickValue, tickColor] : m_ticks)
    {
        int x = QStyle::sliderPositionFromValue(minimum(), maximum(), tickValue, width());

        if (tickValue == m_selectedTick) {
            // Selected tick: red, thicker, with downward-pointing triangle
            painter.setPen(QPen(Qt::red, 4));
            painter.drawLine(x, 15, x, height());

            // Draw downward-pointing triangle above the tick
            QPolygonF triangle;
            triangle << QPointF(x - 5, 10) << QPointF(x + 5, 10) << QPointF(x, 16);
            painter.setBrush(Qt::red);
            painter.setPen(Qt::NoPen);
            painter.drawPolygon(triangle);
        } else {
            // Normal tick: original color, pen width 2
            painter.setPen(QPen(tickColor, 2));
            painter.drawLine(x, 15, x, height());
        }
    }
}
