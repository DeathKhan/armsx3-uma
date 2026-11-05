#include "Utilities/Cell.h"
#include <cassert>
#include <cstdlib>
int main(int argc, char**)
{
#if defined(ANDROID)
    assert(!cell::enabled());
    setenv("ARMSX3_CELL", "1", 1);
    cell::initialize();
    assert(cell::enabled());
#else
    assert(cell::enabled() == (argc > 1));
#endif
}
