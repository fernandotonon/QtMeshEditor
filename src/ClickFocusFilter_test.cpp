// The AI chat dock could not regain keyboard focus by clicking its input
// once another dock had taken widget focus (only a detour through the
// viewport worked). The filter turns every press into an explicit setFocus.
#include <gtest/gtest.h>

#include "ClickFocusFilter.h"

#include <QCoreApplication>
#include <QLineEdit>
#include <QFocusEvent>
#include <QMouseEvent>
#include <QWidget>

namespace {
// Records whether the press reached the widget — the filter must never consume
// it — and the focus traffic the filter generates.
class RecordingWidget : public QWidget
{
public:
    using QWidget::QWidget;
    int presses = 0;
    int focusIns = 0;
    int focusOuts = 0;
protected:
    void mousePressEvent(QMouseEvent* e) override { ++presses; QWidget::mousePressEvent(e); }
    void focusInEvent(QFocusEvent* e) override { ++focusIns; QWidget::focusInEvent(e); }
    void focusOutEvent(QFocusEvent* e) override { ++focusOuts; QWidget::focusOutEvent(e); }
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

    // A press on an ALREADY-focused widget must still RE-ASSERT focus: that is
    // the "clicked outside the app, came back, caret dead" case — the widget
    // kept its focus flag but the QML scene never saw a fresh FocusIn.
    //
    // NB assert the clearFocus()+setFocus() PAIR, not a FocusIn count: Qt
    // delivers QFocusEvent only inside an ACTIVE window, and a headless CI
    // display (Xvfb, no window manager) never activates one — the focus widget
    // still changes, but no focus event is dispatched, so a FocusIn assertion
    // passes on a developer desktop and fails on CI. Counting the focus-OUT
    // that only clearFocus() can produce proves the re-assert happened on every
    // platform; that pair is what makes QQuickWidget forward FocusIn to its
    // scene when the window IS active.
    ASSERT_TRUE(target->hasFocus()) << "precondition: already focused";
    const int focusOutsBefore = target->focusOuts;
    const bool windowActive = window.isActiveWindow();
    QMouseEvent again(QEvent::MouseButtonPress, QPointF(2, 2), QPointF(2, 2), QPointF(2, 2),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &again);
    QCoreApplication::processEvents();
    EXPECT_EQ(window.focusWidget(), target) << "still the focus widget afterwards";
    EXPECT_EQ(target->presses, 2);
    EXPECT_TRUE(target->hasFocus());
    if (windowActive)
        EXPECT_GT(target->focusOuts, focusOutsBefore)
            << "an active window must see the FocusOut/FocusIn pair the QML scene needs";

    // other event types are ignored
    other->setFocus(Qt::OtherFocusReason);
    QMouseEvent move(QEvent::MouseMove, QPointF(3, 3), QPointF(3, 3), QPointF(3, 3),
                     Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &move);
    EXPECT_EQ(window.focusWidget(), other);
}
