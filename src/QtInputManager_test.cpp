#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPointF>
#include "QtInputManager.h"

// ---------------------------------------------------------------------------
// Mock listeners
// ---------------------------------------------------------------------------

class MockKeyListener : public QtKeyListener
{
public:
    MOCK_METHOD(void, keyPressEvent, (QKeyEvent *event), (override));
    MOCK_METHOD(void, keyReleaseEvent, (QKeyEvent *event), (override));
};

class MockMouseListener : public QtMouseListener
{
public:
    MOCK_METHOD(void, mousePressEvent, (QMouseEvent *event), (override));
    MOCK_METHOD(void, mouseReleaseEvent, (QMouseEvent *event), (override));
    MOCK_METHOD(void, mouseMoveEvent, (QMouseEvent *event), (override));
    MOCK_METHOD(void, wheelEvent, (QWheelEvent *event), (override));
};

// ---------------------------------------------------------------------------
// A key listener that removes itself from the input manager when it
// receives a keyPressEvent.  Used to exercise the deferred-removal path.
// ---------------------------------------------------------------------------

class SelfRemovingKeyListener : public QtKeyListener
{
public:
    int pressCount = 0;

    void keyPressEvent(QKeyEvent * /*event*/) override
    {
        ++pressCount;
        QtInputManager::getInstance().RemoveKeyListener(this);
    }
    void keyReleaseEvent(QKeyEvent * /*event*/) override {}
};

// ---------------------------------------------------------------------------
// A mouse listener that removes itself on mousePressEvent.
// ---------------------------------------------------------------------------

class SelfRemovingMouseListener : public QtMouseListener
{
public:
    int pressCount = 0;

    void mousePressEvent(QMouseEvent * /*event*/) override
    {
        ++pressCount;
        QtInputManager::getInstance().RemoveMouseListener(this);
    }
    void mouseReleaseEvent(QMouseEvent * /*event*/) override {}
    void mouseMoveEvent(QMouseEvent * /*event*/) override {}
    void wheelEvent(QWheelEvent * /*event*/) override {}
};

// ---------------------------------------------------------------------------
// Fixture -- because QtInputManager is a singleton we must clean up
// between tests.  There is no public "clear" API, so we add/remove
// the listeners we create in each test.
// ---------------------------------------------------------------------------

class QtInputManagerTest : public ::testing::Test
{
protected:
    // Helper: create a stack-allocated QKeyEvent for testing purposes.
    static QKeyEvent makeKeyEvent(QEvent::Type type, int key,
                                  Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        return QKeyEvent(type, key, mods);
    }

    // Helper: create a stack-allocated QMouseEvent.
    static QMouseEvent makeMouseEvent(QEvent::Type type,
                                      const QPointF &pos = QPointF(10, 20),
                                      Qt::MouseButton button = Qt::LeftButton,
                                      Qt::MouseButtons buttons = Qt::LeftButton,
                                      Qt::KeyboardModifiers mods = Qt::NoModifier)
    {
        return QMouseEvent(type, pos, button, buttons, mods);
    }

    // Helper: create a stack-allocated QWheelEvent.
    static QWheelEvent makeWheelEvent(const QPointF &pos = QPointF(10, 20),
                                      const QPoint &angleDelta = QPoint(0, 120))
    {
        return QWheelEvent(pos, pos, QPoint(), angleDelta,
                           Qt::NoButton, Qt::NoModifier,
                           Qt::NoScrollPhase, false);
    }

    QtInputManager &mgr = QtInputManager::getInstance();
};

// ===========================================================================
// Key listener management
// ===========================================================================

TEST_F(QtInputManagerTest, AddKeyListener_ReceivesKeyPressAndRelease)
{
    MockKeyListener listener;
    mgr.AddKeyListener(&listener);

    EXPECT_CALL(listener, keyPressEvent(::testing::_)).Times(1);
    EXPECT_CALL(listener, keyReleaseEvent(::testing::_)).Times(1);

    auto press = makeKeyEvent(QEvent::KeyPress, Qt::Key_A);
    mgr.keyPressEvent(&press);

    auto release = makeKeyEvent(QEvent::KeyRelease, Qt::Key_A);
    mgr.keyReleaseEvent(&release);

    // Cleanup
    mgr.RemoveKeyListener(&listener);
    // Force the pending removal to be processed.
    mgr.keyPressEvent(&press);
}

TEST_F(QtInputManagerTest, RemoveKeyListener_StopsReceivingEvents)
{
    MockKeyListener listener;
    mgr.AddKeyListener(&listener);
    mgr.RemoveKeyListener(&listener);

    // The removal is deferred -- it happens at the start of the NEXT dispatch.
    // So after one dispatch the listener should no longer be called.
    EXPECT_CALL(listener, keyPressEvent(::testing::_)).Times(0);

    auto press = makeKeyEvent(QEvent::KeyPress, Qt::Key_B);
    mgr.keyPressEvent(&press);
}

TEST_F(QtInputManagerTest, MultipleKeyListeners_AllReceiveEvents)
{
    MockKeyListener listenerA;
    MockKeyListener listenerB;
    mgr.AddKeyListener(&listenerA);
    mgr.AddKeyListener(&listenerB);

    EXPECT_CALL(listenerA, keyPressEvent(::testing::_)).Times(1);
    EXPECT_CALL(listenerB, keyPressEvent(::testing::_)).Times(1);

    auto press = makeKeyEvent(QEvent::KeyPress, Qt::Key_C);
    mgr.keyPressEvent(&press);

    // Cleanup
    mgr.RemoveKeyListener(&listenerA);
    mgr.RemoveKeyListener(&listenerB);
    mgr.keyPressEvent(&press);  // flush deferred removals
}

// ===========================================================================
// Mouse listener management
// ===========================================================================

TEST_F(QtInputManagerTest, AddMouseListener_ReceivesAllMouseEvents)
{
    MockMouseListener listener;
    mgr.AddMouseListener(&listener);

    EXPECT_CALL(listener, mousePressEvent(::testing::_)).Times(1);
    EXPECT_CALL(listener, mouseReleaseEvent(::testing::_)).Times(1);
    EXPECT_CALL(listener, mouseMoveEvent(::testing::_)).Times(1);
    EXPECT_CALL(listener, wheelEvent(::testing::_)).Times(1);

    auto press = makeMouseEvent(QEvent::MouseButtonPress);
    mgr.mousePressEvent(&press);

    auto release = makeMouseEvent(QEvent::MouseButtonRelease);
    mgr.mouseReleaseEvent(&release);

    auto move = makeMouseEvent(QEvent::MouseMove, QPointF(30, 40),
                               Qt::NoButton, Qt::LeftButton);
    mgr.mouseMoveEvent(&move);

    auto wheel = makeWheelEvent();
    mgr.wheelEvent(&wheel);

    // Cleanup
    mgr.RemoveMouseListener(&listener);
    auto flushPress = makeMouseEvent(QEvent::MouseButtonPress);
    mgr.mousePressEvent(&flushPress);
}

TEST_F(QtInputManagerTest, RemoveMouseListener_StopsReceivingEvents)
{
    MockMouseListener listener;
    mgr.AddMouseListener(&listener);
    mgr.RemoveMouseListener(&listener);

    EXPECT_CALL(listener, mousePressEvent(::testing::_)).Times(0);
    EXPECT_CALL(listener, mouseReleaseEvent(::testing::_)).Times(0);
    EXPECT_CALL(listener, mouseMoveEvent(::testing::_)).Times(0);
    EXPECT_CALL(listener, wheelEvent(::testing::_)).Times(0);

    auto press = makeMouseEvent(QEvent::MouseButtonPress);
    mgr.mousePressEvent(&press);

    auto release = makeMouseEvent(QEvent::MouseButtonRelease);
    mgr.mouseReleaseEvent(&release);

    auto move = makeMouseEvent(QEvent::MouseMove);
    mgr.mouseMoveEvent(&move);

    auto wheel = makeWheelEvent();
    mgr.wheelEvent(&wheel);
}

TEST_F(QtInputManagerTest, MultipleMouseListeners_AllReceiveEvents)
{
    MockMouseListener listenerA;
    MockMouseListener listenerB;
    mgr.AddMouseListener(&listenerA);
    mgr.AddMouseListener(&listenerB);

    EXPECT_CALL(listenerA, mousePressEvent(::testing::_)).Times(1);
    EXPECT_CALL(listenerB, mousePressEvent(::testing::_)).Times(1);

    auto press = makeMouseEvent(QEvent::MouseButtonPress);
    mgr.mousePressEvent(&press);

    // Cleanup
    mgr.RemoveMouseListener(&listenerA);
    mgr.RemoveMouseListener(&listenerB);
    auto flushPress = makeMouseEvent(QEvent::MouseButtonPress);
    mgr.mousePressEvent(&flushPress);
}

// ===========================================================================
// Deferred removal during event dispatch
// ===========================================================================

TEST_F(QtInputManagerTest, DeferredRemoval_KeyListener_ProcessedOnNextDispatch)
{
    // Verify that Remove queues the listener for removal and it is actually
    // removed only when the next event dispatch begins.
    MockKeyListener listener;
    mgr.AddKeyListener(&listener);

    // First dispatch -- listener is still active.
    EXPECT_CALL(listener, keyPressEvent(::testing::_)).Times(1);
    auto press1 = makeKeyEvent(QEvent::KeyPress, Qt::Key_X);
    mgr.keyPressEvent(&press1);

    // Request removal.
    mgr.RemoveKeyListener(&listener);

    // Second dispatch -- removal is processed first, then events dispatched.
    EXPECT_CALL(listener, keyPressEvent(::testing::_)).Times(0);
    auto press2 = makeKeyEvent(QEvent::KeyPress, Qt::Key_Y);
    mgr.keyPressEvent(&press2);
}

TEST_F(QtInputManagerTest, DeferredRemoval_MouseListener_ProcessedOnNextDispatch)
{
    MockMouseListener listener;
    mgr.AddMouseListener(&listener);

    // First dispatch -- listener is still active.
    EXPECT_CALL(listener, mousePressEvent(::testing::_)).Times(1);
    auto press1 = makeMouseEvent(QEvent::MouseButtonPress);
    mgr.mousePressEvent(&press1);

    // Request removal.
    mgr.RemoveMouseListener(&listener);

    // Second dispatch -- removal is processed first.
    EXPECT_CALL(listener, mousePressEvent(::testing::_)).Times(0);
    auto press2 = makeMouseEvent(QEvent::MouseButtonPress);
    mgr.mousePressEvent(&press2);
}

TEST_F(QtInputManagerTest, SelfRemovingKeyListener_SafeDuringDispatch)
{
    // A listener that calls RemoveKeyListener(this) from inside keyPressEvent.
    // Because removal is deferred, the iteration should complete safely.
    // The listener should still receive the current event but not subsequent ones.
    SelfRemovingKeyListener selfRemover;
    MockKeyListener other;
    mgr.AddKeyListener(&selfRemover);
    mgr.AddKeyListener(&other);

    // Both listeners receive the first event.
    EXPECT_CALL(other, keyPressEvent(::testing::_)).Times(1);
    auto press1 = makeKeyEvent(QEvent::KeyPress, Qt::Key_Z);
    mgr.keyPressEvent(&press1);
    EXPECT_EQ(selfRemover.pressCount, 1);

    // On the next dispatch the self-remover should have been purged.
    EXPECT_CALL(other, keyPressEvent(::testing::_)).Times(1);
    auto press2 = makeKeyEvent(QEvent::KeyPress, Qt::Key_Z);
    mgr.keyPressEvent(&press2);
    EXPECT_EQ(selfRemover.pressCount, 1);  // no additional call

    // Cleanup
    mgr.RemoveKeyListener(&other);
    mgr.keyPressEvent(&press2);
}

TEST_F(QtInputManagerTest, SelfRemovingMouseListener_SafeDuringDispatch)
{
    SelfRemovingMouseListener selfRemover;
    MockMouseListener other;
    mgr.AddMouseListener(&selfRemover);
    mgr.AddMouseListener(&other);

    // Both listeners receive the first event.
    EXPECT_CALL(other, mousePressEvent(::testing::_)).Times(1);
    auto press1 = makeMouseEvent(QEvent::MouseButtonPress);
    mgr.mousePressEvent(&press1);
    EXPECT_EQ(selfRemover.pressCount, 1);

    // On the next dispatch the self-remover should have been purged.
    EXPECT_CALL(other, mousePressEvent(::testing::_)).Times(1);
    auto press2 = makeMouseEvent(QEvent::MouseButtonPress);
    mgr.mousePressEvent(&press2);
    EXPECT_EQ(selfRemover.pressCount, 1);  // no additional call

    // Cleanup
    mgr.RemoveMouseListener(&other);
    auto flushPress = makeMouseEvent(QEvent::MouseButtonPress);
    mgr.mousePressEvent(&flushPress);
}

// ===========================================================================
// Event dispatch to correct handler methods
// ===========================================================================

TEST_F(QtInputManagerTest, KeyReleaseEvent_DispatchedCorrectly)
{
    MockKeyListener listener;
    mgr.AddKeyListener(&listener);

    // Only keyReleaseEvent should be called, not keyPressEvent.
    EXPECT_CALL(listener, keyPressEvent(::testing::_)).Times(0);
    EXPECT_CALL(listener, keyReleaseEvent(::testing::_)).Times(1);

    auto release = makeKeyEvent(QEvent::KeyRelease, Qt::Key_Escape);
    mgr.keyReleaseEvent(&release);

    // Cleanup
    mgr.RemoveKeyListener(&listener);
    auto press = makeKeyEvent(QEvent::KeyPress, Qt::Key_A);
    mgr.keyPressEvent(&press);
}

TEST_F(QtInputManagerTest, MouseMoveEvent_DispatchedCorrectly)
{
    MockMouseListener listener;
    mgr.AddMouseListener(&listener);

    EXPECT_CALL(listener, mousePressEvent(::testing::_)).Times(0);
    EXPECT_CALL(listener, mouseReleaseEvent(::testing::_)).Times(0);
    EXPECT_CALL(listener, mouseMoveEvent(::testing::_)).Times(1);
    EXPECT_CALL(listener, wheelEvent(::testing::_)).Times(0);

    auto move = makeMouseEvent(QEvent::MouseMove, QPointF(100, 200),
                               Qt::NoButton, Qt::LeftButton);
    mgr.mouseMoveEvent(&move);

    // Cleanup
    mgr.RemoveMouseListener(&listener);
    auto flushPress = makeMouseEvent(QEvent::MouseButtonPress);
    mgr.mousePressEvent(&flushPress);
}

TEST_F(QtInputManagerTest, WheelEvent_DispatchedCorrectly)
{
    MockMouseListener listener;
    mgr.AddMouseListener(&listener);

    EXPECT_CALL(listener, mousePressEvent(::testing::_)).Times(0);
    EXPECT_CALL(listener, mouseReleaseEvent(::testing::_)).Times(0);
    EXPECT_CALL(listener, mouseMoveEvent(::testing::_)).Times(0);
    EXPECT_CALL(listener, wheelEvent(::testing::_)).Times(1);

    auto wheel = makeWheelEvent(QPointF(50, 60), QPoint(0, -120));
    mgr.wheelEvent(&wheel);

    // Cleanup
    mgr.RemoveMouseListener(&listener);
    auto flushPress = makeMouseEvent(QEvent::MouseButtonPress);
    mgr.mousePressEvent(&flushPress);
}

// ===========================================================================
// Edge cases
// ===========================================================================

TEST_F(QtInputManagerTest, NoListeners_DispatchDoesNotCrash)
{
    // Dispatching events with no listeners should be a no-op.
    auto keyPress = makeKeyEvent(QEvent::KeyPress, Qt::Key_Space);
    mgr.keyPressEvent(&keyPress);

    auto keyRelease = makeKeyEvent(QEvent::KeyRelease, Qt::Key_Space);
    mgr.keyReleaseEvent(&keyRelease);

    auto mousePress = makeMouseEvent(QEvent::MouseButtonPress);
    mgr.mousePressEvent(&mousePress);

    auto mouseRelease = makeMouseEvent(QEvent::MouseButtonRelease);
    mgr.mouseReleaseEvent(&mouseRelease);

    auto mouseMove = makeMouseEvent(QEvent::MouseMove);
    mgr.mouseMoveEvent(&mouseMove);

    auto wheel = makeWheelEvent();
    mgr.wheelEvent(&wheel);
}

TEST_F(QtInputManagerTest, RemoveSameListenerTwice_DoesNotCrash)
{
    MockKeyListener listener;
    mgr.AddKeyListener(&listener);
    mgr.RemoveKeyListener(&listener);
    mgr.RemoveKeyListener(&listener);  // duplicate removal request

    EXPECT_CALL(listener, keyPressEvent(::testing::_)).Times(0);
    auto press = makeKeyEvent(QEvent::KeyPress, Qt::Key_Q);
    mgr.keyPressEvent(&press);
}

TEST_F(QtInputManagerTest, RemoveNeverAddedListener_DoesNotCrash)
{
    MockKeyListener neverAdded;
    mgr.RemoveKeyListener(&neverAdded);

    // Dispatch should succeed without issues.
    auto press = makeKeyEvent(QEvent::KeyPress, Qt::Key_W);
    mgr.keyPressEvent(&press);
}

TEST_F(QtInputManagerTest, AddRemoveAdd_ListenerReceivesEventsAgain)
{
    MockKeyListener listener;
    mgr.AddKeyListener(&listener);
    mgr.RemoveKeyListener(&listener);

    // Flush the deferred removal.
    EXPECT_CALL(listener, keyPressEvent(::testing::_)).Times(0);
    auto press1 = makeKeyEvent(QEvent::KeyPress, Qt::Key_R);
    mgr.keyPressEvent(&press1);

    // Re-add the listener.
    mgr.AddKeyListener(&listener);

    EXPECT_CALL(listener, keyPressEvent(::testing::_)).Times(1);
    auto press2 = makeKeyEvent(QEvent::KeyPress, Qt::Key_S);
    mgr.keyPressEvent(&press2);

    // Cleanup
    mgr.RemoveKeyListener(&listener);
    mgr.keyPressEvent(&press2);
}

TEST_F(QtInputManagerTest, DeferredRemoval_ProcessedByKeyReleaseEvent)
{
    // Verify deferred removal is also processed by keyReleaseEvent,
    // not only by keyPressEvent.
    MockKeyListener listener;
    mgr.AddKeyListener(&listener);
    mgr.RemoveKeyListener(&listener);

    EXPECT_CALL(listener, keyReleaseEvent(::testing::_)).Times(0);
    auto release = makeKeyEvent(QEvent::KeyRelease, Qt::Key_T);
    mgr.keyReleaseEvent(&release);
}

TEST_F(QtInputManagerTest, DeferredMouseRemoval_ProcessedByAllMouseEventTypes)
{
    // Verify deferred mouse removal is processed by mouseReleaseEvent.
    {
        MockMouseListener listener;
        mgr.AddMouseListener(&listener);
        mgr.RemoveMouseListener(&listener);

        EXPECT_CALL(listener, mouseReleaseEvent(::testing::_)).Times(0);
        auto release = makeMouseEvent(QEvent::MouseButtonRelease);
        mgr.mouseReleaseEvent(&release);
    }

    // Verify deferred mouse removal is processed by mouseMoveEvent.
    {
        MockMouseListener listener;
        mgr.AddMouseListener(&listener);
        mgr.RemoveMouseListener(&listener);

        EXPECT_CALL(listener, mouseMoveEvent(::testing::_)).Times(0);
        auto move = makeMouseEvent(QEvent::MouseMove);
        mgr.mouseMoveEvent(&move);
    }

    // Verify deferred mouse removal is processed by wheelEvent.
    {
        MockMouseListener listener;
        mgr.AddMouseListener(&listener);
        mgr.RemoveMouseListener(&listener);

        EXPECT_CALL(listener, wheelEvent(::testing::_)).Times(0);
        auto wheel = makeWheelEvent();
        mgr.wheelEvent(&wheel);
    }
}

TEST_F(QtInputManagerTest, EventPointerPassedThrough)
{
    // Verify that the exact event pointer is forwarded to the listener.
    MockKeyListener keyListener;
    mgr.AddKeyListener(&keyListener);

    auto press = makeKeyEvent(QEvent::KeyPress, Qt::Key_F1);
    QKeyEvent *capturedEvent = nullptr;
    EXPECT_CALL(keyListener, keyPressEvent(::testing::_))
        .WillOnce(::testing::SaveArg<0>(&capturedEvent));

    mgr.keyPressEvent(&press);
    EXPECT_EQ(capturedEvent, &press);

    // Cleanup
    mgr.RemoveKeyListener(&keyListener);
    mgr.keyPressEvent(&press);
}

TEST_F(QtInputManagerTest, MouseEventPointerPassedThrough)
{
    MockMouseListener mouseListener;
    mgr.AddMouseListener(&mouseListener);

    auto press = makeMouseEvent(QEvent::MouseButtonPress, QPointF(77, 88));
    QMouseEvent *capturedEvent = nullptr;
    EXPECT_CALL(mouseListener, mousePressEvent(::testing::_))
        .WillOnce(::testing::SaveArg<0>(&capturedEvent));

    mgr.mousePressEvent(&press);
    EXPECT_EQ(capturedEvent, &press);

    // Cleanup
    mgr.RemoveMouseListener(&mouseListener);
    auto flushPress = makeMouseEvent(QEvent::MouseButtonPress);
    mgr.mousePressEvent(&flushPress);
}
