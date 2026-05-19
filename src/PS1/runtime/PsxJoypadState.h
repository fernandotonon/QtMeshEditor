#ifndef PSXJOYPADSTATE_H
#define PSXJOYPADSTATE_H

#include <cstdint>

/** Libretro RETRO_DEVICE_ID_JOYPAD_* (PlayStation: B=Cross, A=Circle). */
namespace PsxJoypadButton {
constexpr unsigned B = 0;
constexpr unsigned Y = 1;
constexpr unsigned Select = 2;
constexpr unsigned Start = 3;
constexpr unsigned Up = 4;
constexpr unsigned Down = 5;
constexpr unsigned Left = 6;
constexpr unsigned Right = 7;
constexpr unsigned A = 8;
constexpr unsigned X = 9;
constexpr unsigned L = 10;
constexpr unsigned R = 11;
constexpr unsigned L2 = 12;
constexpr unsigned R2 = 13;
constexpr unsigned L3 = 14;
constexpr unsigned R3 = 15;
} // namespace PsxJoypadButton

/** Forwards UI input to the active EmuCore on the worker thread (main app only). */
class PsxJoypadState
{
public:
    static constexpr unsigned kPortCount = 2;
    static constexpr unsigned kButtonCount = 16;

    static void setPressed(unsigned port, unsigned buttonId, bool pressed);
    static void reset(unsigned port = 0);
    static void resetAll();
};

#endif // PSXJOYPADSTATE_H
