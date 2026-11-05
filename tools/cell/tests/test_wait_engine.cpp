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
#include "Utilities/CellScheduler.h"
#include "Utilities/CellFiberTls.h"
#include "Utilities/sema.cpp"
#include "Utilities/mutex.cpp"
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

// Guest PPU fibers waiting in the production engine (ARMSX3_CELL_FIBER): the
// fiber hooks route parks through a real two-host scheduler.
static cell::scheduler* g_fiber_sched = nullptr;
namespace cell
{
void* current_fiber() noexcept { return scheduler::current_task(); }
void park_fiber(std::uint64_t deadline_ns)
{
    if (auto* task = scheduler::current_task()) task->park(deadline_ns);
}
void wake_fiber(void* fiber) { g_fiber_sched->wake(*static_cast<fiber_task*>(fiber)); }
std::uint64_t fiber_now_ns() noexcept { return scheduler::now_ns(); }
}

namespace
{
constexpr unsigned fiber_count = 6;
constexpr unsigned fiber_rounds = 5000;
atomic_t<u32> g_fiber_words[fiber_count]{};
atomic_t<u32> g_fiber_chain{0};
std::atomic<unsigned> g_fiber_done{0};
std::atomic<u64> g_fiber_timed_ns{0};

void fiber_waiter(cell::fiber_task&, void* p)
{
    auto& word = g_fiber_words[reinterpret_cast<uptr>(p)];
    for (unsigned r = 0; r < fiber_rounds; ++r)
    {
        // Even: fiber's turn. Odd: a native thread flips it back and notifies.
        while (true)
        {
            const u32 v = word.load();
            if (!(v & 1)) break;
            word.wait(v);
        }
        word += 1;
        word.notify_one();
    }
    g_fiber_done++;
}

// Two fibers on the same host hand a token back and forth through the engine.
void fiber_chain(cell::fiber_task&, void* p)
{
    const u32 parity = static_cast<u32>(reinterpret_cast<uptr>(p));
    for (unsigned r = 0; r < fiber_rounds; ++r)
    {
        while (true)
        {
            const u32 v = g_fiber_chain.load();
            if ((v & 1) == parity) break;
            g_fiber_chain.wait(v);
        }
        g_fiber_chain += 1;
        g_fiber_chain.notify_all();
    }
    g_fiber_done++;
}

void fiber_timed(cell::fiber_task&, void*)
{
    atomic_t<u32> never{0};
    const auto start = clock_type::now();
    never.wait(0, atomic_wait_timeout{20'000'000});
    g_fiber_timed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now() - start).count();
    g_fiber_done++;
}
}

// sys_semaphore shapes on guest fibers (CELL_FIBER_AUDIT wait matrix):
//  - semaphore_base (Utilities/sema.h) ping-pong between two fibers on ONE host
//    and between hosts: wait parks the fiber, post must requeue it
//  - an lv2-style sleep queue: a waiter enqueues its state word under a
//    shared_mutex and waits on it; the poster dequeues, signals, records the
//    word in a deferred notify list (thread_local like lv2_obj::g_to_notify,
//    registered with fiber_tls), yields, and only then flushes notify_one
namespace
{
constexpr unsigned sema_rounds = 4000;
semaphore<> g_sema_a{0}, g_sema_b{0}, g_sema_c{0}, g_sema_d{0};
std::atomic<unsigned> g_sema_done{0};

struct lv2_like_sema
{
    shared_mutex mutex;
    std::vector<atomic_t<u32>*> sleepers;
    u32 value = 0;
};
lv2_like_sema g_lv2_sema;
lv2_like_sema g_lv2_ack;
static thread_local atomic_t<u32>* t_deferred_notify[4]{};
CELL_FIBER_TLS(t_deferred_notify);

void lv2_wait(lv2_like_sema& s)
{
    atomic_t<u32> state{0};
    {
        std::lock_guard lock(s.mutex);
        if (s.value) { s.value--; return; }
        state = 1;
        s.sleepers.push_back(&state);
    }
    while (state.load() == 1)
        state.wait(1);
}

void lv2_post(lv2_like_sema& s, cell::fiber_task* self)
{
    {
        std::lock_guard lock(s.mutex);
        if (s.sleepers.empty()) { s.value++; return; }
        auto* w = s.sleepers.front();
        s.sleepers.erase(s.sleepers.begin());
        w->release(0);
        for (auto& slot : t_deferred_notify)
            if (!slot) { slot = w; break; }
    }
    // A switch between signalling and the deferred flush must not lose the
    // poster's pending list (it is a thread_local of the host).
    if (self) self->yield();
    for (auto& slot : t_deferred_notify)
        if (auto* w = std::exchange(slot, nullptr)) w->notify_one();
}

void sema_ping(cell::fiber_task&, void* p)
{
    const bool cross = reinterpret_cast<uptr>(p) != 0;
    auto& in = cross ? g_sema_c : g_sema_a;
    auto& out = cross ? g_sema_d : g_sema_b;
    for (unsigned r = 0; r < sema_rounds; ++r) { out.unlock(); in.lock(); }
    g_sema_done++;
}

void sema_pong(cell::fiber_task&, void* p)
{
    const bool cross = reinterpret_cast<uptr>(p) != 0;
    auto& in = cross ? g_sema_d : g_sema_b;
    auto& out = cross ? g_sema_c : g_sema_a;
    for (unsigned r = 0; r < sema_rounds; ++r) { in.lock(); out.unlock(); }
    g_sema_done++;
}

void lv2_waiter(cell::fiber_task& task, void*)
{
    for (unsigned r = 0; r < sema_rounds; ++r) { lv2_wait(g_lv2_sema); lv2_post(g_lv2_ack, &task); }
    g_sema_done++;
}

void lv2_poster(cell::fiber_task& task, void*)
{
    for (unsigned r = 0; r < sema_rounds; ++r) { lv2_post(g_lv2_sema, &task); lv2_wait(g_lv2_ack); }
    g_sema_done++;
}
}

static void fiber_semaphores()
{
    cell::scheduler sched(2);
    g_fiber_sched = &sched;
    // same host
    sched.admit(sched.spawn(0, &sema_ping, nullptr));
    sched.admit(sched.spawn(0, &sema_pong, nullptr));
    // cross host
    sched.admit(sched.spawn(0, &sema_ping, reinterpret_cast<void*>(uptr{1})));
    sched.admit(sched.spawn(1, &sema_pong, reinterpret_cast<void*>(uptr{1})));
    // lv2-style queue, same host
    sched.admit(sched.spawn(1, &lv2_waiter, nullptr));
    sched.admit(sched.spawn(1, &lv2_poster, nullptr));
    const auto deadline = clock_type::now() + std::chrono::seconds(20);
    while (g_sema_done.load() < 6)
    {
        if (clock_type::now() > deadline)
        {
            std::fprintf(stderr, "fiber semaphores: STALL done=%u/6 parks=%llu wakes=%llu\n", g_sema_done.load(),
                static_cast<unsigned long long>(sched.park_count()), static_cast<unsigned long long>(sched.wake_count()));
            std::abort();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::printf("fiber semaphores: same-host + cross-host semaphore_base and lv2-style deferred-notify queue x %u rounds; parks=%llu wakes=%llu\n",
        sema_rounds, static_cast<unsigned long long>(sched.park_count()), static_cast<unsigned long long>(sched.wake_count()));
    sched.stop();
    sched.join();
    g_fiber_sched = nullptr;
}

static void fiber_waits()
{
    cell::scheduler sched(2);
    g_fiber_sched = &sched;
    for (unsigned i = 0; i < fiber_count; ++i)
        sched.admit(sched.spawn(i % 2, &fiber_waiter, reinterpret_cast<void*>(uptr{i})));
    sched.admit(sched.spawn(0, &fiber_chain, reinterpret_cast<void*>(uptr{0})));
    sched.admit(sched.spawn(0, &fiber_chain, reinterpret_cast<void*>(uptr{1})));
    sched.admit(sched.spawn(1, &fiber_timed, nullptr));
    constexpr unsigned total = fiber_count + 3;
    std::thread driver([]
    {
        while (g_fiber_done.load() < total)
        {
            for (auto& word : g_fiber_words)
            {
                u32 v = word.load();
                if ((v & 1) && word.compare_and_swap_test(v, v + 1))
                    word.notify_one();
            }
        }
    });
    driver.join();
    assert(g_fiber_timed_ns.load() >= 20'000'000);
    assert(g_fiber_timed_ns.load() < 1'000'000'000);
    assert(sched.park_count() > 0 && sched.timeout_count() >= 1);
    std::printf("fiber waits: %u fibers x %u rounds + same-host chain + timed %.1f ms; parks=%llu wakes=%llu\n",
        fiber_count, fiber_rounds, g_fiber_timed_ns.load() / 1e6,
        static_cast<unsigned long long>(sched.park_count()), static_cast<unsigned long long>(sched.wake_count()));
    sched.stop();
    sched.join();
    g_fiber_sched = nullptr;
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
    fiber_waits();
    fiber_semaphores();
    for (const auto& root : s_hashtable) assert(root.bits.load().ref == 0);
    ::alarm(0);
#if defined(ARCH_ARM64)
    const bool events = cell::enabled() && utils::has_wfe_event_stream();
#else
    const bool events = false;
#endif
    std::printf("PASS production atomic.cpp: master=%d event_waits=%d, 20000 notifications, 2000 multiwaits, forced/normal unchanged-predicate wake, wake-all, timeout override, callbacks, empty registry\n", cell::enabled(), events);
}
