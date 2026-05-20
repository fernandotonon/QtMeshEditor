#ifdef ENABLE_PS1_RIP
#include <gtest/gtest.h>

#include "PS1/runtime/PsxJoypadBindings.h"
#include "PS1/runtime/PsxJoypadState.h"

TEST(PsxJoypadBindingsTest, DefaultMappingMatchesIssue417)
{
    PsxJoypadBindings::resetToDefaults();

    unsigned button = 999;
    EXPECT_TRUE(PsxJoypadBindings::mapKey(Qt::Key_Up, &button));
    EXPECT_EQ(button, PsxJoypadButton::Up);

    EXPECT_TRUE(PsxJoypadBindings::mapKey(Qt::Key_Z, &button));
    EXPECT_EQ(button, PsxJoypadButton::A);
    EXPECT_TRUE(PsxJoypadBindings::mapKey(Qt::Key_X, &button));
    EXPECT_EQ(button, PsxJoypadButton::B);
    EXPECT_TRUE(PsxJoypadBindings::mapKey(Qt::Key_S, &button));
    EXPECT_EQ(button, PsxJoypadButton::X);
    EXPECT_TRUE(PsxJoypadBindings::mapKey(Qt::Key_D, &button));
    EXPECT_EQ(button, PsxJoypadButton::Y);
    EXPECT_TRUE(PsxJoypadBindings::mapKey(Qt::Key_Return, &button));
    EXPECT_EQ(button, PsxJoypadButton::Start);
    EXPECT_TRUE(PsxJoypadBindings::mapKey(Qt::Key_Shift, &button));
    EXPECT_EQ(button, PsxJoypadButton::Select);
}

TEST(PsxJoypadBindingsTest, CustomBindingOverridesDefault)
{
    PsxJoypadBindings::resetToDefaults();
    PsxJoypadBindings::setKeyForButton(PsxJoypadButton::Start, Qt::Key_P);

    unsigned button = 0;
    EXPECT_TRUE(PsxJoypadBindings::mapKey(Qt::Key_P, &button));
    EXPECT_EQ(button, PsxJoypadButton::Start);
    EXPECT_FALSE(PsxJoypadBindings::mapKey(Qt::Key_Return, &button));
}
#endif
