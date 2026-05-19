#include "PsxJoypadState.h"
#include "PS1RipManager.h"

void PsxJoypadState::setPressed(unsigned port, unsigned buttonId, bool pressed)
{
    if (port >= kPortCount || buttonId >= kButtonCount)
        return;
    if (PS1RipManager *manager = PS1RipManager::getSingletonPtr())
        manager->setJoypadPressed(port, buttonId, pressed);
}

void PsxJoypadState::reset(unsigned port)
{
    if (port >= kPortCount)
        return;
    if (PS1RipManager *manager = PS1RipManager::getSingletonPtr())
        manager->resetJoypad(port);
}

void PsxJoypadState::resetAll()
{
    for (unsigned port = 0; port < kPortCount; ++port)
        reset(port);
}
