#include "Utilities/CellFiber.h"

#include <array>
#include <cassert>
#include <cfenv>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

using cell::fiber;

struct task
{
    int id;
    int rounding;
    int steps = 0;
    int destroyed = 0;
};

static void count_entry(fiber& self, void* opaque)
{
    auto& task = *static_cast<struct task*>(opaque);
    struct scoped
    {
        int& count;
        ~scoped() { ++count; }
    } cleanup{task.destroyed};
    assert(std::fesetround(task.rounding) == 0);
    volatile std::uint64_t values[16];
    for (int i = 0; i < 16; ++i)
        values[i] = std::uint64_t(task.id + 1) * (i + 13);
    const auto local = reinterpret_cast<std::uintptr_t>(&values);
    const auto start = reinterpret_cast<std::uintptr_t>(self.stack_begin());
    assert(local >= start && local + sizeof(values) <= start + self.stack_size());
    for (int n = 0; n < 10000; ++n)
    {
        for (int i = 0; i < 16; ++i)
            assert(values[i] == std::uint64_t(task.id + 1) * (i + 13));
        assert(std::fegetround() == task.rounding);
        ++task.steps;
        self.yield();
    }
}

static void expect_guard_fault(void* address)
{
    const pid_t child = ::fork();
    assert(child >= 0);
    if (child == 0)
    {
        struct rlimit limit{};
        ::setrlimit(RLIMIT_CORE, &limit);
        *static_cast<volatile char*>(address) = 1;
        _exit(0);
    }
    int status = 0;
    assert(::waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
}

int main()
{
    std::fesetround(FE_TONEAREST);
    task a{1, FE_DOWNWARD}, b{2, FE_UPWARD};
    fiber first(count_entry, &a, 128 * 1024 + 1);
    fiber second(count_entry, &b, 256 * 1024);
    for (int n = 0; n < 10000; ++n)
    {
        assert(first.resume());
        assert(second.resume());
        assert(std::fegetround() == FE_TONEAREST);
        assert(a.steps == n + 1 && b.steps == n + 1);
        assert(a.destroyed == 0 && b.destroyed == 0);
    }
    assert(!first.resume() && first.finished());
    assert(!second.resume() && second.finished());
    assert(a.destroyed == 1 && b.destroyed == 1);
    assert(!first.resume());

    bool migration_rejected = false;
    std::thread wrong_host([&] {
        try { first.resume(); }
        catch (const std::logic_error&) { migration_rejected = true; }
    });
    wrong_host.join();
    assert(migration_rejected);

    bool caught = false;
    fiber exception([](fiber&, void*) { throw std::runtime_error("entry exception"); });
    try { exception.resume(); }
    catch (const std::runtime_error&) { caught = true; }
    assert(caught && exception.finished());

    bool recursive_rejected = false;
    fiber recursion([](fiber& self, void* out) {
        try { self.resume(); }
        catch (const std::logic_error&) { *static_cast<bool*>(out) = true; }
    }, &recursive_rejected);
    assert(!recursion.resume() && recursive_rejected);

    bool yield_rejected = false;
    try { first.yield(); }
    catch (const std::logic_error&) { yield_rejected = true; }
    assert(yield_rejected);

    bool invalid_rejected = false;
    try { fiber invalid(nullptr); }
    catch (const std::invalid_argument&) { invalid_rejected = true; }
    assert(invalid_rejected);

    const auto begin = reinterpret_cast<std::uintptr_t>(first.stack_begin());
    expect_guard_fault(reinterpret_cast<void*>(begin - 1));
    expect_guard_fault(reinterpret_cast<void*>(begin + first.stack_size()));
    std::puts("PASS: 20,000 yields/resumes, isolated FP state and stacks, normal unwind, exception forwarding, guards, misuse rejection");
}
