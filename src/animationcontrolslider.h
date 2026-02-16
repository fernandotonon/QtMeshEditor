#ifndef ANIMATIONCONTROLSLIDER_H
#define ANIMATIONCONTROLSLIDER_H

#include <QSlider>
#include <QPainter>
#include <QPolygonF>
#include <QStyle>

class AnimationControlSlider : public QSlider
{
    Q_OBJECT

public:
    explicit AnimationControlSlider(QWidget *parent = nullptr);
    void addTick(int value, QColor color);
    void clearTicks();
    void setSelectedTick(int value);
    int selectedTick() const { return m_selectedTick; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    std::vector<std::pair<int, QColor>> m_ticks;
    int m_selectedTick = -1;
};

#endif // ANIMATIONCONTROLSLIDER_H
