#ifndef PSXMIPSGTERUNNER_H
#define PSXMIPSGTERUNNER_H

#include "PsxGteEngine.h"

#include <cstddef>
#include <cstdint>

class EmuHooks;

/** Execute a short MIPS block that configures the GTE and runs RTPS/RTPT. */
class PsxMipsGteRunner
{
public:
    struct Result {
        int stepsExecuted = 0;
        int rtpsEvents = 0;
    };

    static Result runBlock(const uint8_t *ram, size_t ramBytes, uint32_t startPc, int maxSteps,
                           PsxGteEngine &gte, EmuHooks *hooks);
};

#endif // PSXMIPSGTERUNNER_H
