#ifndef ANIMATIONCONTROLSLIDER_H
#define ANIMATIONCONTROLSLIDER_H

#include <QSlider>
#include <QPainter>
#include <QStyle>

class AnimationControlSlider : public QSlider
{
    Q_OBJECT

public:
    explicit AnimationControlSlider(QWidget *parent = nullptr);
    void addTick(int value, QColor color);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    std::vector<std::pair<int, QColor>> m_ticks;
};

#endif // ANIMATIONCONTROLSLIDER_H
