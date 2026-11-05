#include "Utilities/Cell.h"
#include <cassert>
#include <cstdio>

int main()
{
    assert(cell::enabled());
    assert(!cell::event_wait_enabled(UINT64_MAX, true));
    cell::waiter_enabled() = true;
    assert(!cell::event_wait_enabled(UINT64_MAX, false));
    assert(!cell::event_wait_enabled(199'999, true));
    assert(cell::event_wait_enabled(200'000, true));
    assert(cell::event_wait_enabled(UINT64_MAX, true));
    {
        const cell::scoped_native_wait timer;
        assert(!cell::event_wait_enabled(UINT64_MAX, true));
        {
            const cell::scoped_native_wait nested;
            assert(!cell::event_wait_enabled(UINT64_MAX, true));
        }
        assert(!cell::event_wait_enabled(UINT64_MAX, true));
    }
    assert(cell::event_wait_enabled(UINT64_MAX, true));
    cell::waiter_enabled() = false;
    {
        const cell::scoped_native_wait helper;
        assert(!cell::event_wait_enabled(UINT64_MAX, true));
    }
    assert(!cell::waiter_enabled());
    std::puts("Wait policy: CPU role, EVTSTRM, short timeout, timer suppression/nesting passed");
}
