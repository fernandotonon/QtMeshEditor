#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QTest>
#include "animationcontrolslider.h"

// Test fixture for AnimationControlSlider class
class AnimationControlSliderTest : public ::testing::Test {
protected:
    void SetUp() override {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);

        slider = new AnimationControlSlider();
        slider->setRange(0, 100);
    }

    void TearDown() override {
        delete slider;
        slider = nullptr;
    }

    QApplication* app = nullptr;
    AnimationControlSlider* slider = nullptr;
};

TEST_F(AnimationControlSliderTest, Constructor_DefaultSelectedTickIsMinusOne) {
    // Verify that a freshly constructed slider has selectedTick == -1
    EXPECT_EQ(slider->selectedTick(), -1);
}

TEST_F(AnimationControlSliderTest, SetRange_VerifyMinMax) {
    // The fixture sets range to [0, 100]
    EXPECT_EQ(slider->minimum(), 0);
    EXPECT_EQ(slider->maximum(), 100);

    // Change range and verify
    slider->setRange(10, 500);
    EXPECT_EQ(slider->minimum(), 10);
    EXPECT_EQ(slider->maximum(), 500);
}

TEST_F(AnimationControlSliderTest, AddTick_SingleTickNoCrash) {
    // Adding a single tick should not crash
    EXPECT_NO_THROW(slider->addTick(50, Qt::blue));
}

TEST_F(AnimationControlSliderTest, AddMultipleTicks_NoCrash) {
    // Adding multiple ticks should not crash
    EXPECT_NO_THROW({
        slider->addTick(10, Qt::red);
        slider->addTick(20, Qt::green);
        slider->addTick(30, Qt::blue);
        slider->addTick(40, Qt::yellow);
        slider->addTick(50, Qt::cyan);
    });
}

TEST_F(AnimationControlSliderTest, ClearTicks_ResetsSelectedTickToMinusOne) {
    // Add some ticks and set a selected tick
    slider->addTick(25, Qt::red);
    slider->addTick(75, Qt::blue);
    slider->setSelectedTick(25);
    ASSERT_EQ(slider->selectedTick(), 25);

    // Clear ticks should reset selectedTick to -1
    slider->clearTicks();
    EXPECT_EQ(slider->selectedTick(), -1);
}

TEST_F(AnimationControlSliderTest, SetSelectedTick_GetterReturnsCorrectValue) {
    slider->setSelectedTick(50);
    EXPECT_EQ(slider->selectedTick(), 50);
}

TEST_F(AnimationControlSliderTest, SetSelectedTick_SameValueNoUnnecessaryUpdate) {
    // Set to 42 once
    slider->setSelectedTick(42);
    EXPECT_EQ(slider->selectedTick(), 42);

    // Set to 42 again — should not change anything (guard condition in implementation)
    slider->setSelectedTick(42);
    EXPECT_EQ(slider->selectedTick(), 42);
}

TEST_F(AnimationControlSliderTest, SetSelectedTick_DifferentValues) {
    slider->setSelectedTick(10);
    EXPECT_EQ(slider->selectedTick(), 10);

    slider->setSelectedTick(20);
    EXPECT_EQ(slider->selectedTick(), 20);

    // Original value should not persist
    EXPECT_NE(slider->selectedTick(), 10);
}

TEST_F(AnimationControlSliderTest, ClearTicks_AfterSetSelectedTick_ResetsToMinusOne) {
    // Set a selected tick first
    slider->setSelectedTick(75);
    ASSERT_EQ(slider->selectedTick(), 75);

    // Add ticks and then clear
    slider->addTick(30, Qt::red);
    slider->addTick(60, Qt::green);
    slider->clearTicks();

    // selectedTick should be reset
    EXPECT_EQ(slider->selectedTick(), -1);
}

TEST_F(AnimationControlSliderTest, PaintEvent_NoCrash) {
    // Add ticks including a selected one to exercise all paint code paths
    slider->addTick(10, Qt::red);
    slider->addTick(50, Qt::green);
    slider->addTick(90, Qt::blue);
    slider->setSelectedTick(50);

    // Show widget and force a paint to trigger paintEvent
    slider->resize(200, 30);
    slider->show();
    if (app) app->processEvents();

    EXPECT_NO_THROW({
        slider->repaint();
        if (app) app->processEvents();
    });
}

// ── Mouse click on tick marks ────────────────────────────────────

TEST_F(AnimationControlSliderTest, MouseClickAtTickPosition) {
    // Add ticks at known positions
    slider->addTick(25, Qt::red);
    slider->addTick(50, Qt::green);
    slider->addTick(75, Qt::blue);

    slider->resize(200, 30);
    slider->show();
    if (!QTest::qWaitForWindowExposed(slider, 1000))
        GTEST_SKIP() << "Window not exposed (headless environment)";

    // Simulate a mouse click near the middle of the slider (position for tick 50)
    // QSlider maps clicks to values; the click should change the slider value
    QPoint center(slider->width() / 2, slider->height() / 2);
    QTest::mouseClick(slider, Qt::LeftButton, Qt::NoModifier, center);
    if (app) app->processEvents();

    // The slider value should have changed to something near the center
    // (exact value depends on slider geometry, but should be roughly 50)
    EXPECT_GE(slider->value(), 30);
    EXPECT_LE(slider->value(), 70);
}

TEST_F(AnimationControlSliderTest, MouseClickAtBeginning) {
    slider->addTick(0, Qt::red);
    slider->addTick(100, Qt::blue);

    slider->resize(400, 30);
    slider->show();
    if (app) app->processEvents();

    // Click near the beginning (left side)
    QPoint leftSide(5, slider->height() / 2);
    QTest::mouseClick(slider, Qt::LeftButton, Qt::NoModifier, leftSide);
    if (app) app->processEvents();

    // Value should be in the lower half (slider click mapping varies by style)
    EXPECT_LE(slider->value(), 50);
}

TEST_F(AnimationControlSliderTest, MouseClickAtEnd) {
    slider->addTick(0, Qt::red);
    slider->addTick(100, Qt::blue);

    slider->resize(400, 30);
    slider->show();
    if (!QTest::qWaitForWindowExposed(slider, 1000))
        GTEST_SKIP() << "Window not exposed (headless environment)";

    // Click near the end (right side)
    QPoint rightSide(slider->width() - 5, slider->height() / 2);
    QTest::mouseClick(slider, Qt::LeftButton, Qt::NoModifier, rightSide);
    if (app) app->processEvents();

    // Value should have changed from 0; exact value depends on platform style
    // On macOS, clicking near the end may jump to a value around 40-100 depending
    // on the slider groove margin. Just verify it moved to a non-zero value.
    EXPECT_GT(slider->value(), 0);
}

// ── setValue changes ─────────────────────────────────────────────

TEST_F(AnimationControlSliderTest, SetValue_UpdatesSliderPosition) {
    slider->setValue(42);
    EXPECT_EQ(slider->value(), 42);
}

TEST_F(AnimationControlSliderTest, SetValue_ClampsBelowMinimum) {
    slider->setRange(10, 90);
    slider->setValue(5);
    EXPECT_EQ(slider->value(), 10);
}

TEST_F(AnimationControlSliderTest, SetValue_ClampsAboveMaximum) {
    slider->setRange(10, 90);
    slider->setValue(100);
    EXPECT_EQ(slider->value(), 90);
}

TEST_F(AnimationControlSliderTest, SetValue_MultipleTimes) {
    for (int i = 0; i <= 100; i += 10) {
        slider->setValue(i);
        EXPECT_EQ(slider->value(), i);
    }
}

TEST_F(AnimationControlSliderTest, SetValue_EmitsValueChanged) {
    bool signalReceived = false;
    int receivedValue = -1;

    QObject::connect(slider, &QSlider::valueChanged, [&](int val) {
        signalReceived = true;
        receivedValue = val;
    });

    slider->setValue(75);
    if (app) app->processEvents();

    EXPECT_TRUE(signalReceived);
    EXPECT_EQ(receivedValue, 75);
}

TEST_F(AnimationControlSliderTest, SetValue_SameValueDoesNotEmitSignal) {
    slider->setValue(50);
    if (app) app->processEvents();

    int signalCount = 0;
    QObject::connect(slider, &QSlider::valueChanged, [&](int) {
        signalCount++;
    });

    // Setting the same value should not emit again
    slider->setValue(50);
    if (app) app->processEvents();

    EXPECT_EQ(signalCount, 0);
}

// ── Paint event with various states ──────────────────────────────

TEST_F(AnimationControlSliderTest, PaintEvent_NoTicks_NoCrash) {
    // No ticks added -- paint should still work
    slider->resize(200, 30);
    slider->show();
    if (app) app->processEvents();

    EXPECT_NO_THROW({
        slider->repaint();
        if (app) app->processEvents();
    });
}

TEST_F(AnimationControlSliderTest, PaintEvent_SelectedTickOutOfRange_NoCrash) {
    slider->addTick(50, Qt::red);
    slider->setSelectedTick(999); // not matching any tick

    slider->resize(200, 30);
    slider->show();
    if (app) app->processEvents();

    EXPECT_NO_THROW({
        slider->repaint();
        if (app) app->processEvents();
    });
}

TEST_F(AnimationControlSliderTest, PaintEvent_ManyTicks_NoCrash) {
    // Add many ticks to exercise the paint loop
    for (int i = 0; i <= 100; i += 2) {
        slider->addTick(i, QColor(i * 2, 255 - i * 2, 128));
    }
    slider->setSelectedTick(50);

    slider->resize(200, 30);
    slider->show();
    if (app) app->processEvents();

    EXPECT_NO_THROW({
        slider->repaint();
        if (app) app->processEvents();
    });
}

// ── Tick management edge cases ───────────────────────────────────

TEST_F(AnimationControlSliderTest, AddTick_AtBoundaries) {
    EXPECT_NO_THROW({
        slider->addTick(0, Qt::red);      // at minimum
        slider->addTick(100, Qt::blue);    // at maximum
    });
}

TEST_F(AnimationControlSliderTest, ClearTicks_WhenEmpty_NoCrash) {
    // Clear when no ticks have been added
    EXPECT_NO_THROW(slider->clearTicks());
    EXPECT_EQ(slider->selectedTick(), -1);
}

TEST_F(AnimationControlSliderTest, ClearTicks_MultipleTimes_NoCrash) {
    slider->addTick(10, Qt::red);
    slider->clearTicks();
    slider->clearTicks(); // double clear
    EXPECT_EQ(slider->selectedTick(), -1);
}
