#ifndef QTMOUSELISTENER_H
#define QTMOUSELISTENER_H

#include <QMouseEvent>
#include <QWheelEvent>

class QtMouseListener
{
public:
    virtual void mousePressEvent(QMouseEvent *event) = 0;
    virtual void mouseReleaseEvent(QMouseEvent *event) = 0;
    virtual void mouseMoveEvent(QMouseEvent *event) = 0;
    virtual void wheelEvent(QWheelEvent *event) = 0;
    virtual ~QtMouseListener() = default;
};

#endif // QTMOUSELISTENER_H
