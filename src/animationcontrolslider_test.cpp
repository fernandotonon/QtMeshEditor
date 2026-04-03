#include <gtest/gtest.h>
#include <QApplication>
#include <QCoreApplication>
#include <QPixmap>

#define private public
#include "animationcontrolslider.h"
#undef private

class AnimationControlSliderTest : public ::testing::Test {
protected:
    QApplication* app = nullptr;

    void SetUp() override {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
    }
};

TEST_F(AnimationControlSliderTest, AddTickStoresTickData) {
    AnimationControlSlider slider;
    slider.addTick(10, Qt::green);
    slider.addTick(50, Qt::blue);

    ASSERT_EQ(slider.m_ticks.size(), 2u);
    EXPECT_EQ(slider.m_ticks[0].first, 10);
    EXPECT_EQ(slider.m_ticks[0].second, QColor(Qt::green));
    EXPECT_EQ(slider.m_ticks[1].first, 50);
    EXPECT_EQ(slider.m_ticks[1].second, QColor(Qt::blue));
}

TEST_F(AnimationControlSliderTest, ClearTicksRemovesTicksAndResetsSelection) {
    AnimationControlSlider slider;
    slider.addTick(20, Qt::yellow);
    slider.setSelectedTick(20);
    ASSERT_EQ(slider.selectedTick(), 20);
    ASSERT_FALSE(slider.m_ticks.empty());

    slider.clearTicks();

    EXPECT_TRUE(slider.m_ticks.empty());
    EXPECT_EQ(slider.selectedTick(), -1);
}

TEST_F(AnimationControlSliderTest, SetSelectedTickUpdatesValue) {
    AnimationControlSlider slider;
    EXPECT_EQ(slider.selectedTick(), -1);

    slider.setSelectedTick(15);
    EXPECT_EQ(slider.selectedTick(), 15);

    slider.setSelectedTick(15); // no-op branch
    EXPECT_EQ(slider.selectedTick(), 15);

    slider.setSelectedTick(42);
    EXPECT_EQ(slider.selectedTick(), 42);
}

TEST_F(AnimationControlSliderTest, PaintEventCoversSelectedAndUnselectedTicks) {
    AnimationControlSlider slider;
    slider.setRange(0, 100);
    slider.setFixedSize(240, 40);
    slider.addTick(25, Qt::green); // unselected path
    slider.addTick(75, Qt::blue);  // selected path
    slider.setSelectedTick(75);

    QPixmap pixmap(slider.size());
    pixmap.fill(Qt::transparent);
    slider.render(&pixmap); // triggers paintEvent

    slider.setSelectedTick(-1); // now both ticks use non-selected branch
    slider.render(&pixmap);

    SUCCEED();
}
