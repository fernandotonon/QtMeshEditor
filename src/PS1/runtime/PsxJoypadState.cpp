#include "PsxJoypadState.h"
#include "PS1RipManager.h"

namespace {

uint16_t s_keyboardMask[PsxJoypadState::kPortCount] = {};
uint16_t s_gamepadMask[PsxJoypadState::kPortCount] = {};

uint16_t *maskFor(PsxJoypadState::Source source, unsigned port)
{
    if (port >= PsxJoypadState::kPortCount)
        return nullptr;
    return source == PsxJoypadState::Source::Gamepad ? &s_gamepadMask[port] : &s_keyboardMask[port];
}

void syncButton(unsigned port, unsigned buttonId)
{
    if (port >= PsxJoypadState::kPortCount || buttonId >= PsxJoypadState::kButtonCount)
        return;
    const bool pressed =
        ((s_keyboardMask[port] | s_gamepadMask[port]) & (uint16_t(1) << buttonId)) != 0;
    if (PS1RipManager *manager = PS1RipManager::getSingletonPtr())
        manager->setJoypadPressed(port, buttonId, pressed);
}

void syncPort(unsigned port)
{
    for (unsigned button = 0; button < PsxJoypadState::kButtonCount; ++button)
        syncButton(port, button);
}

} // namespace

void PsxJoypadState::setPressed(unsigned port, unsigned buttonId, bool pressed, Source source)
{
    if (port >= kPortCount || buttonId >= kButtonCount)
        return;

    uint16_t *mask = maskFor(source, port);
    if (!mask)
        return;

    const uint16_t bit = uint16_t(1) << buttonId;
    if (pressed)
        *mask |= bit;
    else
        *mask &= ~bit;

    syncButton(port, buttonId);
}

void PsxJoypadState::reset(unsigned port)
{
    if (port >= kPortCount)
        return;
    s_keyboardMask[port] = 0;
    s_gamepadMask[port] = 0;
    if (PS1RipManager *manager = PS1RipManager::getSingletonPtr())
        manager->resetJoypad(port);
}

void PsxJoypadState::resetSource(unsigned port, Source source)
{
    if (port >= kPortCount)
        return;
    if (uint16_t *mask = maskFor(source, port)) {
        *mask = 0;
        syncPort(port);
    }
}

void PsxJoypadState::resetAll()
{
    for (unsigned port = 0; port < kPortCount; ++port)
        reset(port);
}
