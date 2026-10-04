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
    // NB everything asserted here must be WINDOW-ACTIVATION INDEPENDENT.
    // A headless CI display (Xvfb, no window manager) never activates the
    // window, and Qt gates BOTH of these on activation:
    //   * QFocusEvent delivery — so focusIn/focusOut counts stay 0;
    //   * QWidget::hasFocus() — it is `window()->focusWidget() == this &&
    //     window()->isActiveWindow()`, so it is false even when this widget
    //     IS the focus widget.
    // `window.focusWidget()` is the one that tracks focus regardless, which is
    // why the assertion above it passes on CI. Two earlier attempts at this
    // test asserted a FocusIn count and then hasFocus(), and each passed on a
    // developer desktop and failed the Linux lane. Assert focusWidget() only,
    // and gate anything activation-dependent behind isActiveWindow().
    const bool windowActive = window.isActiveWindow();
    ASSERT_EQ(window.focusWidget(), target) << "precondition: already the focus widget";
    const int focusOutsBefore = target->focusOuts;
    QMouseEvent again(QEvent::MouseButtonPress, QPointF(2, 2), QPointF(2, 2), QPointF(2, 2),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &again);
    QCoreApplication::processEvents();
    EXPECT_EQ(window.focusWidget(), target) << "still the focus widget afterwards";
    EXPECT_EQ(target->presses, 2);
    if (windowActive) {
        EXPECT_TRUE(target->hasFocus());
        EXPECT_GT(target->focusOuts, focusOutsBefore)
            << "an active window must see the FocusOut/FocusIn pair the QML scene needs";
    }

    // other event types are ignored
    other->setFocus(Qt::OtherFocusReason);
    QMouseEvent move(QEvent::MouseMove, QPointF(3, 3), QPointF(3, 3), QPointF(3, 3),
                     Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &move);
    EXPECT_EQ(window.focusWidget(), other);
}

// Regression guard for the two CI-only failures this test already caused.
// The window is deliberately NEVER shown, which reproduces what a headless CI
// display (Xvfb, no window manager) does: the window is not ACTIVE. Qt gates
// QWidget::hasFocus() and QFocusEvent delivery on activation, so a test that
// asserts either passes on a developer desktop and fails the Linux lane. The
// filter's real contract — the clicked widget becomes the window's focus
// widget — holds either way, and that is what this pins.
TEST(ClickFocusFilter, FocusWidgetIsSetEvenWhenTheWindowIsNotActive)
{
    QWidget window;                       // NOT shown → never active
    auto* other  = new QLineEdit(&window);
    auto* target = new RecordingWidget(&window);
    target->setFocusPolicy(Qt::StrongFocus);
    ClickFocusFilter filter;
    target->installEventFilter(&filter);
    other->setFocus(Qt::OtherFocusReason);
    QCoreApplication::processEvents();
    ASSERT_FALSE(window.isActiveWindow()) << "precondition: this is the CI case";

    QMouseEvent press(QEvent::MouseButtonPress, QPointF(2, 2), QPointF(2, 2), QPointF(2, 2),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &press);
    QCoreApplication::processEvents();

    EXPECT_EQ(window.focusWidget(), target) << "the filter's contract does not depend on activation";
    EXPECT_EQ(target->presses, 1) << "and the press is never consumed";
    // hasFocus() is deliberately NOT asserted here: it is
    // `focusWidget() == this && isActiveWindow()`, so it is false by
    // construction in this case. Asserting it is the bug this test guards.
    EXPECT_FALSE(target->hasFocus()) << "documents WHY hasFocus must not be asserted unconditionally";
}
