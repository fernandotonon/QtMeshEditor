#include <gtest/gtest.h>
#include <QApplication>
#include <QPushButton>
#include "AnimationWidget.h"

class AnimationWidgetTest : public ::testing::Test {
protected:
    QApplication* app;
    AnimationWidget* widget;

    static int argc;
    static char* argv[];

    void SetUp() override {
        app = new QApplication(argc, argv);
        widget = new AnimationWidget();
    }

    void TearDown() override {
        delete widget;
        delete app;
    }
};

int AnimationWidgetTest::argc = 0;
char* AnimationWidgetTest::argv[] = {};

TEST_F(AnimationWidgetTest, PlayPauseButtonTogglesState) {
    bool initialState = widget->isPlaying(); 
    widget->on_PlayPauseButton_toggled(!initialState);
    bool newState = widget->isPlaying(); 

    EXPECT_NE(initialState, newState);
}