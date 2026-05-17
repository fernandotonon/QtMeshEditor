#ifndef EMUHOOKS_H
#define EMUHOOKS_H

/**
 * GPU/GTE interception callbacks (Phase 2 — #418).
 * Stub cores leave hooks null; real cores invoke these from the worker thread.
 */
class EmuHooks
{
public:
    virtual ~EmuHooks() = default;
};

#endif // EMUHOOKS_H
