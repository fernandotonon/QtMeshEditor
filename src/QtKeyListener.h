#ifndef QTKEYLISTENER_H
#define QTKEYLISTENER_H

#include <QKeyEvent>

class QtKeyListener
{
public:
    virtual void keyPressEvent(QKeyEvent *event) = 0;
    virtual void keyReleaseEvent(QKeyEvent *event) = 0;
};

#endif // QTKEYLISTENER_H
