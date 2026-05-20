#ifdef ENABLE_PS1_RIP

#include <gtest/gtest.h>

#include "PS1/runtime/CaptureBuffer.h"
#include "PS1/runtime/Gp0HookDispatch.h"
#include "PS1/runtime/RipperHooks.h"

#include <QElapsedTimer>

#include <atomic>
#include <cstring>

TEST(Gp0HookDispatchTest, DisarmedCaptureMinimalOverhead)
{
    alignas(4) uint8_t ram[64 * 1024];
    std::memset(ram, 0x20, sizeof(ram));

    std::atomic<bool> armed{false};
    CaptureBuffer buffer;
    RipperHooks hooks;
    hooks.setArmedFlag(&armed);
    hooks.setBuffer(&buffer);

    QElapsedTimer timer;
    constexpr int kIterations = 30;
    timer.start();
    for (int i = 0; i < kIterations; ++i)
        hooks.ingestSystemRamForGpuCapture(ram, sizeof(ram));
    const qint64 disarmedNs = timer.nsecsElapsed();

    armed.store(true, std::memory_order_release);
    timer.restart();
    for (int i = 0; i < kIterations; ++i)
        hooks.ingestSystemRamForGpuCapture(ram, sizeof(ram));
    const qint64 armedNs = timer.nsecsElapsed();

    ASSERT_GT(armedNs, 0);
    EXPECT_LT(disarmedNs, armedNs / 50)
        << "disarmed=" << disarmedNs << "ns armed=" << armedNs << "ns";
    EXPECT_LT(static_cast<double>(disarmedNs) / static_cast<double>(armedNs), 0.01)
        << "disarmed overhead should stay below 1% of armed scan cost";
}

#endif // ENABLE_PS1_RIP
