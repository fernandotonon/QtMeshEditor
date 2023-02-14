#include "animationcontrolslider.h"

AnimationControlSlider::AnimationControlSlider(QWidget *parent)
    : QSlider(parent)
{

}

void AnimationControlSlider::addTick(int value, QColor color)
{
    m_ticks.push_back(std::make_pair(value, color));
}

void AnimationControlSlider::paintEvent(QPaintEvent *event)
{
    QSlider::paintEvent(event);

    for(auto tick : m_ticks)
    {
        int x = style()->sliderPositionFromValue(minimum(), maximum(), tick.first, width());
        QPainter painter(this);
        painter.setPen(QPen(tick.second, 2));
        painter.drawLine(x, 15, x, height());
    }
    
}
