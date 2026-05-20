#ifdef ENABLE_PS1_RIP

#include "EmuViewport.h"

#include <gtest/gtest.h>
#include <QApplication>

class EmuViewportTest : public ::testing::Test
{
protected:
    void SetUp() override { ASSERT_NE(QCoreApplication::instance(), nullptr); }
};

TEST_F(EmuViewportTest, IntegerScaleUsesWholePixelSteps)
{
    const QSize frame(320, 240);
    const EmuViewport::FrameLayout layout = EmuViewport::computeFrameLayout(
        QSize(960, 720), frame, true, false, EmuViewport::AspectMode::Native);

    ASSERT_FALSE(layout.target.isEmpty());
    EXPECT_FALSE(layout.smoothFiltering);
    EXPECT_EQ(layout.target.width() % frame.width(), 0);
    EXPECT_EQ(layout.target.height() % frame.height(), 0);
    EXPECT_GE(layout.target.width(), frame.width());
    EXPECT_GE(layout.target.height(), frame.height());
}

TEST_F(EmuViewportTest, SmoothFilteringWhenNotIntegerScaled)
{
    const EmuViewport::FrameLayout layout = EmuViewport::computeFrameLayout(
        QSize(400, 300), QSize(320, 240), false, true, EmuViewport::AspectMode::Native);

    ASSERT_FALSE(layout.target.isEmpty());
    EXPECT_TRUE(layout.smoothFiltering);
}

TEST_F(EmuViewportTest, Display43LetterboxesWideWidget)
{
    const EmuViewport::FrameLayout layout = EmuViewport::computeFrameLayout(
        QSize(1000, 600), QSize(320, 240), true, false, EmuViewport::AspectMode::Display43);

    ASSERT_FALSE(layout.target.isEmpty());
    const qreal layoutAspect =
        static_cast<qreal>(layout.target.width()) / static_cast<qreal>(layout.target.height());
    EXPECT_NEAR(layoutAspect, 4.0 / 3.0, 0.02);
    EXPECT_LT(layout.target.width(), 1000);
}

#endif // ENABLE_PS1_RIP
