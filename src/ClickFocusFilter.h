#ifndef CLICKFOCUSFILTER_H
#define CLICKFOCUSFILTER_H

// Gives a widget keyboard focus on any mouse press inside it.
//
// Why this exists: a QQuickWidget hosting a QML text field (the AI chat
// dock) receives the QML side of a click — the TextArea shows its caret —
// but once Qt-widget focus has moved to ANOTHER dock (the Inspector is a
// QQuickWidget too), the click does not always move QWidget focus back, so
// keystrokes keep going to the previous widget. Clicking the 3D viewport
// (a plain QWidget) and then the field again "fixed" it because the viewport
// took focus and released it normally. Installing this filter on the hosting
// widget makes every press an explicit setFocus(), independent of the QML
// scene's own focus bookkeeping.
//
// Pure Qt, no Ogre — unit-tested with an offscreen QWidget.

#include <QEvent>
#include <QObject>
#include <QWidget>

class ClickFocusFilter : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::MouseButtonPress) {
            if (auto* w = qobject_cast<QWidget*>(watched); w && !w->hasFocus())
                w->setFocus(Qt::MouseFocusReason);
        }
        return false;   // never consume — the QML scene still needs the press
    }
};

#endif // CLICKFOCUSFILTER_H
