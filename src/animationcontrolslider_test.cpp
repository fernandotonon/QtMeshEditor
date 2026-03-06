#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
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
