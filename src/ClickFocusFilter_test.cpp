// The AI chat dock could not regain keyboard focus by clicking its input
// once another dock had taken widget focus (only a detour through the
// viewport worked). The filter turns every press into an explicit setFocus.
#include <gtest/gtest.h>

#include "ClickFocusFilter.h"

#include <QCoreApplication>
#include <QLineEdit>
#include <QMouseEvent>
#include <QWidget>

namespace {
// Records whether the press reached the widget — the filter must never consume it.
class RecordingWidget : public QWidget
{
public:
    using QWidget::QWidget;
    int presses = 0;
protected:
    void mousePressEvent(QMouseEvent* e) override { ++presses; QWidget::mousePressEvent(e); }
};
} // namespace

TEST(ClickFocusFilter, MousePressMovesWidgetFocusToTheWatchedWidget)
{
    QWidget window;
    auto* other  = new QLineEdit(&window);
    auto* target = new RecordingWidget(&window);
    target->setFocusPolicy(Qt::StrongFocus);
    ClickFocusFilter filter;
    target->installEventFilter(&filter);
    window.show();
    other->setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
    ASSERT_EQ(window.focusWidget(), other);

    QMouseEvent press(QEvent::MouseButtonPress, QPointF(2, 2), QPointF(2, 2), QPointF(2, 2),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &press);
    QCoreApplication::processEvents();
    EXPECT_EQ(window.focusWidget(), target) << "the press must move widget focus to the clicked widget";
    EXPECT_EQ(target->presses, 1) << "the filter never consumes the event — the widget (and the QML scene behind it) still gets the press";

    // a press on an already-focused widget is a no-op (no focus churn)
    QMouseEvent again(QEvent::MouseButtonPress, QPointF(2, 2), QPointF(2, 2), QPointF(2, 2),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &again);
    EXPECT_EQ(window.focusWidget(), target);
    EXPECT_EQ(target->presses, 2);

    // other event types are ignored
    other->setFocus(Qt::OtherFocusReason);
    QMouseEvent move(QEvent::MouseMove, QPointF(3, 3), QPointF(3, 3), QPointF(3, 3),
                     Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &move);
    EXPECT_EQ(window.focusWidget(), other);
}
