#include "Utilities/Cell.h"
#include <cassert>
#include <cstdio>

static void test_topology()
{
    std::array<std::uint32_t, 64> capacity{};
    for (unsigned i = 0; i < 6; ++i) capacity[i] = 792;
    capacity[6] = capacity[7] = 1024;
    auto t = cell::topology::detect(capacity, 0xff);
    assert(t && t.lower == 0x3f && t.upper == 0xc0);
    assert(!cell::topology::detect(capacity, 0x3f));
    assert(!cell::topology::detect(capacity, 0xc7));
    assert(!cell::topology::detect(capacity, 0x7f));
    capacity[0] = 100;
    assert(!cell::topology::detect(capacity, 0xff));
    capacity[0] = 0;
    assert(!cell::topology::detect(capacity, 0xff));
    capacity.fill(1024);
    assert(!cell::topology::detect(capacity, 0xff));
    capacity.fill(0);
    assert(!cell::topology::detect(capacity, 0));
    // A machine with non-contiguous allowed CPUs must never acquire disallowed ones.
    for (unsigned i : {1,3,5,7}) capacity[i] = 792;
    capacity[9] = capacity[11] = 1024;
    t = cell::topology::detect(capacity, 0xaaa);
    assert(t && t.lower == 0xaa && t.upper == 0xa00);
}

int main() { test_topology(); std::puts("Cell topology: 9 cases passed"); }
