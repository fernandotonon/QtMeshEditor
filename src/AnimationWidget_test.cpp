#include <gtest/gtest.h>
#include <QApplication>
#include <QSignalSpy>
#include <QPushButton>
#include "AnimationWidget.h"

TEST(AnimationWidgetTest, AnimationStateChange)
{
    // Create the QApplication
    int argc = 0;
    char** argv = nullptr;
    QApplication app(argc, argv);

    // Create an instance of AnimationWidget
    AnimationWidget widget;

    auto playButton = widget.findChild<QPushButton*>("PlayPauseButton");

    // Create a signal spy to monitor the changeAnimationState signal
    QSignalSpy spy(&widget, SIGNAL(changeAnimationState(bool)));

    // click the play button to change animation state
    playButton->click();

    // Check if the changeAnimationState signal was emitted with the correct argument
    ASSERT_EQ(spy.count(), 1);
    QList<QVariant> arguments = spy.takeFirst();
    ASSERT_EQ(arguments.at(0).toBool(), true);

    // click the play button to change animation state
    playButton->click();

    // Check if the changeAnimationState signal was emitted with the correct argument
    ASSERT_EQ(spy.count(), 1);
    arguments = spy.takeFirst();
    ASSERT_EQ(arguments.at(0).toBool(), false);
}
