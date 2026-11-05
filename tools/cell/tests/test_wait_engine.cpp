// Compile the real registry/wait/notify implementation in this TU so forced
// notification and leak checks can inspect its private registry. No wait code
// is copied or replaced. Only unrelated emulator diagnostics/sysinfo are stubbed.
#include "Utilities/StrFmt.h"
#include "rpcs3/util/sysinfo.hpp"
#include <cstdio>
#include <cstdlib>
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__aarch64__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif

namespace fmt
{
[[noreturn]] void raw_verify_error(std::source_location loc, const char8_t*, usz)
{
    std::fprintf(stderr, "verify failed: %s:%u\n", loc.file_name(), loc.line());
    std::abort();
}
[[noreturn]] void raw_throw_exception(std::source_location loc, const char* text, const fmt_type_info*, const u64*)
{
    std::fprintf(stderr, "atomic exception: %s:%u: %s\n", loc.file_name(), loc.line(), text);
    std::abort();
}
}
template <> void fmt_class_string<u32>::format(std::string& out, u64 arg) { out += std::to_string(arg); }
template <> void fmt_class_string<u64>::format(std::string& out, u64 arg) { out += std::to_string(arg); }
namespace utils
{
u64 _get_main_tid() { return ::syscall(SYS_gettid); }
u64 s_tsc_freq = []() -> u64
{
#if defined(__aarch64__)
    u64 result;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(result));
    return result;
#else
    return 1'000'000'000; // Host event branch is compiled out; not a measurement.
#endif
}();
#if defined(__aarch64__)
bool has_lse2() { return false; } // Conservative correct 128-bit atomic path.
bool has_wfe_event_stream() { return (::getauxval(AT_HWCAP) & HWCAP_EVTSTRM) != 0; }
#endif
}

#include "rpcs3/util/atomic.cpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>
#include <vector>

using clock_type = std::chrono::steady_clock;
static constexpr auto short_timeout = atomic_wait_timeout{50'000};
static constexpr auto long_timeout = atomic_wait_timeout{100'000'000};
static thread_local clock_type::time_point callback_deadline;
static thread_local unsigned callback_calls;

static bool callback_until_deadline(u64)
{
    ++callback_calls;
    return clock_type::now() < callback_deadline;
}

static bool reject_initial(const void*, u64 attempt, u64)
{
    ++callback_calls;
    return attempt != 0;
}

static void configure_waiter()
{
    cell::initialize();
    cell::waiter_enabled() = true;
}

static void ping_pong()
{
    constexpr unsigned count = 20'000;
    atomic_t<u32> value{0};
    std::atomic<unsigned> ack{0};
    std::thread writer([&] {
        for (unsigned n = 1; n <= count; ++n)
        {
            while (ack.load() != n - 1) std::this_thread::yield();
            if ((n & 7) == 0) std::this_thread::yield();
            value.release(n);
            value.notify_one();
        }
    });
    configure_waiter();
    for (unsigned n = 1; n <= count; ++n)
    {
        while (value.load() == n - 1) value.wait(n - 1, long_timeout);
        assert(value.load() == n);
        ack.store(n);
    }
    writer.join();
}

static void multi_wait()
{
    constexpr unsigned count = 2000;
    atomic_t<u32> values[3]{};
    std::atomic<unsigned> request{0}, ack{0};
    std::thread writer([&] {
        for (unsigned n = 1; n <= count; ++n)
        {
            while (request.load() != n) std::this_thread::yield();
            const unsigned index = n % 3;
            values[index].release(1);
            values[index].notify_all();
            while (ack.load() != n) std::this_thread::yield();
        }
    });
    for (unsigned n = 1; n <= count; ++n)
    {
        for (auto& value : values) value = 0;
        atomic_wait::list<3> list;
        list.set<0>(values[0], u32{0});
        list.set<1>(values[1], u32{0});
        list.set<2>(values[2], u32{0});
        request.store(n);
        while (!values[0] && !values[1] && !values[2]) list.wait(long_timeout);
        assert(values[n % 3].load() == 1);
        ack.store(n);
    }
    writer.join();
}

static void force_wake()
{
    atomic_t<u32> value{0};
    std::atomic<bool> done{false};
    std::thread waiter([&] {
        configure_waiter();
        value.wait(0);
        assert(value.load() == 0);
        done.store(true);
    });
    bool sent = false;
    while (!sent)
    {
        root_info::slot_search(reinterpret_cast<uptr>(&value), [&](u32 id) {
            sent = alert_sema(id, 0) != 0;
            return sent;
        });
        if (!sent) std::this_thread::yield();
    }
    waiter.join();
    assert(done.load());
}

static void unchanged_normal_notify()
{
    atomic_t<u32> value{0};
    std::atomic<bool> done{false};
    std::thread waiter([&] {
        configure_waiter();
        value.wait(0);
        done.store(true);
    });
    bool registered = false;
    while (!registered)
    {
        root_info::slot_search(reinterpret_cast<uptr>(&value), [&](u32 id) {
            cond_free(id);
            registered = true;
            return true;
        });
        if (!registered) std::this_thread::yield();
    }
    value.notify_one(); // No predicate change: normal infinite wait must continue.
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    assert(!done.load());
    bool sent = false;
    while (!sent)
    {
        root_info::slot_search(reinterpret_cast<uptr>(&value), [&](u32 id) {
            sent = alert_sema(id, 0) != 0;
            return sent;
        });
        if (!sent) std::this_thread::yield();
    }
    waiter.join();
    assert(done.load());
}

static void wake_all()
{
    atomic_t<u32> value{0};
    std::atomic<unsigned> entered{0}, exited{0};
    std::vector<std::thread> waiters;
    for (unsigned n = 0; n < 6; ++n)
        waiters.emplace_back([&] {
            configure_waiter();
            entered.fetch_add(1);
            while (!value.load()) value.wait(0, long_timeout);
            exited.fetch_add(1);
        });
    while (entered.load() != 6) std::this_thread::yield();
    value.release(1);
    value.notify_all();
    for (auto& waiter : waiters) waiter.join();
    assert(exited.load() == 6);
}

static void timeouts_callbacks()
{
    atomic_t<u32> value{0};
    auto start = clock_type::now();
    value.wait(0, atomic_wait_timeout{12'000'000});
    auto elapsed = clock_type::now() - start;
    assert(elapsed >= std::chrono::milliseconds(10));
    assert(elapsed < std::chrono::seconds(1));
    // 50 us uses native mode; 500 us also exercises the event->native handoff
    // on an enabled EVTSTRM AArch64 host while callbacks extend the deadline.
    for (auto timeout : {short_timeout, atomic_wait_timeout{500'000}})
    {
        callback_deadline = clock_type::now() + std::chrono::milliseconds(12);
        callback_calls = 0;
        atomic_wait_engine::set_one_time_use_wait_callback(callback_until_deadline);
        start = clock_type::now();
        value.wait(0, timeout);
        elapsed = clock_type::now() - start;
        assert(elapsed >= std::chrono::milliseconds(10));
        assert(elapsed < std::chrono::seconds(1));
        assert(callback_calls > 1);
    }
    callback_calls = 0;
    atomic_wait_engine::set_wait_callback(reject_initial);
    value.wait(0);
    atomic_wait_engine::set_wait_callback(nullptr);
    assert(callback_calls == 1);
}

int main()
{
    ::alarm(30); // A lost wake must fail the fixture, not hang an agent indefinitely.
    configure_waiter();
    assert(!has_waitv()); // Exercise production Android condition registry, not waitv.
    ping_pong();
    multi_wait();
    force_wake();
    unchanged_normal_notify();
    wake_all();
    timeouts_callbacks();
    for (const auto& root : s_hashtable) assert(root.bits.load().ref == 0);
    ::alarm(0);
#if defined(ARCH_ARM64)
    const bool events = cell::enabled() && utils::has_wfe_event_stream();
#else
    const bool events = false;
#endif
    std::printf("PASS production atomic.cpp: master=%d event_waits=%d, 20000 notifications, 2000 multiwaits, forced/normal unchanged-predicate wake, wake-all, timeout override, callbacks, empty registry\n", cell::enabled(), events);
}
