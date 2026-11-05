#include "Utilities/CellWait.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cerrno>

// Exercise the actual phase object with Linux FUTEX_WAIT, deliberately racing
// phase transition, publication and notification. This checks the handoff;
// EVTSTRM/SEV behavior still requires the AArch64 device test.
static void stress_handoff(bool spin)
{
    constexpr unsigned count = 100'000;
    cell::wait_phase phase;
    std::atomic<unsigned> cond{0}, request{0}, ack{0};
    std::atomic<unsigned> timeouts{0};
    std::thread producer([&]
    {
        for (unsigned i = 1; i <= count; ++i)
        {
            while (request.load() != i) std::this_thread::yield();
            if ((i & 7) == 0) std::this_thread::yield();
            cond.store(1);
            if (!phase.event_mode()) syscall(SYS_futex, &cond, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
            while (ack.load() != i) std::this_thread::yield();
        }
    });
    for (unsigned i = 1; i <= count; ++i)
    {
        cond.store(0);
        phase.begin(spin);
        request.store(i);
        unsigned budget = spin ? (i & 31) : 0;
        while (cond.load() == 0 && budget--) std::this_thread::yield();
        phase.handoff_to_native();
        while (cond.load() == 0)
        {
            timespec timeout{0, 100'000'000};
            if (syscall(SYS_futex, &cond, FUTEX_WAIT_PRIVATE, 0, &timeout, nullptr, 0) == -1 && errno == ETIMEDOUT)
            {
                ++timeouts;
                break;
            }
        }
        assert(cond.load() == 1);
        ack.store(i);
    }
    producer.join();
    assert(timeouts.load() == 0);
}

int main()
{
    stress_handoff(false);
    stress_handoff(true);
    std::puts("Cell wait handoff: 200000 races, zero timeouts");
}
