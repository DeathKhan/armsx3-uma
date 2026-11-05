#include "Utilities/Cell.h"
#include "Utilities/CellFiber.h"
#include "Utilities/CellFiberTls.h"
#include "Utilities/CellScheduler.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

// Host-only fixture for the PPU fiber wait protocol:
//  - ping-pong: fibers check-then-park on a guest word that a foreign thread
//    flips and wakes (the lost-wake race the emulator hit at first dispatch)
//  - timed park returns at its deadline with no waker
//  - registered thread_locals follow the fiber across parks on a shared host
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

static thread_local unsigned t_guest_id = 0;
CELL_FIBER_TLS(t_guest_id);

namespace
{
constexpr unsigned n_fibers = 8;
constexpr unsigned n_rounds = 20000;

struct pingpong
{
	std::atomic<unsigned> turn{0}; // even: fiber's move, odd: driver's move
	cell::fiber_task* task = nullptr;
	unsigned id = 0;
};

std::atomic<unsigned> g_done{0};
std::atomic<unsigned> g_tls_bad{0};

void pingpong_work(cell::fiber_task& task, void* p)
{
	auto* pp = static_cast<pingpong*>(p);
	t_guest_id = pp->id;
	for (unsigned r = 0; r < n_rounds; ++r)
	{
		// Wait for an even turn: check, then park (the racy pattern).
		while (pp->turn.load() & 1)
			task.park();
		if (t_guest_id != pp->id)
			g_tls_bad.fetch_add(1);
		pp->turn.fetch_add(1); // hand to the driver
		if (r % 64 == 0)
			task.yield();
	}
	g_done.fetch_add(1);
}

std::atomic<std::uint64_t> g_timed_elapsed{0};

void timed_work(cell::fiber_task& task, void*)
{
	const auto t0 = cell::scheduler::now_ns();
	task.park(t0 + 20'000'000);
	g_timed_elapsed.store(cell::scheduler::now_ns() - t0);
	g_done.fetch_add(1);
}
}

// Preemption: a CPU-bound guest and a 1 ms periodic guest on the same host.
// The tick pokes the spinner after its slice; the spinner's safe point yields.
std::atomic<bool> g_spin_poked{false};
std::atomic<bool> g_spin_stop{false};
std::atomic<std::uint64_t> g_max_late_ns{0};
std::atomic<unsigned> g_periodic_done{0};

void spin_poke(void*) { g_spin_poked = true; }

void spinner_work(cell::fiber_task& task, void*)
{
	while (!g_spin_stop.load())
	{
		if (g_spin_poked.exchange(false) && task.take_preempt())
			task.yield();
	}
}

void periodic_work(cell::fiber_task& task, void*)
{
	for (unsigned i = 0; i < 100; ++i)
	{
		const auto due = cell::scheduler::now_ns() + 1'000'000;
		task.park(due);
		const auto now = cell::scheduler::now_ns();
		const std::uint64_t late = now > due ? now - due : 0;
		std::uint64_t prev = g_max_late_ns.load();
		while (late > prev && !g_max_late_ns.compare_exchange_weak(prev, late)) {}
	}
	g_periodic_done = 1;
	g_spin_stop = true;
}

static void preemption()
{
	cell::scheduler sched(1);
	auto& spin = sched.spawn(0, &spinner_work, nullptr);
	spin.set_poke(&spin_poke, nullptr);
	sched.admit(sched.spawn(0, &periodic_work, nullptr));
	sched.admit(spin);
	std::atomic<bool> tick_stop{false};
	std::thread ticker([&] { while (!tick_stop) { sched.preempt_scan(2'000'000); std::this_thread::sleep_for(std::chrono::microseconds(500)); } });
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!g_periodic_done.load() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	tick_stop = true;
	ticker.join();
	g_spin_stop = true;
	sched.stop();
	sched.join();
	CHECK(g_periodic_done.load());
	CHECK(g_max_late_ns.load() < 20'000'000);
	std::printf("Cell preemption fixture: 100 x 1 ms periodic parks beside a spinning guest, max lateness %.2f ms, preempts=%llu\n",
		g_max_late_ns.load() / 1e6, static_cast<unsigned long long>(sched.preempt_count()));
}

int main()
{
	preemption();
	cell::scheduler sched(2);
	std::vector<pingpong> pps(n_fibers);
	for (unsigned i = 0; i < n_fibers; ++i)
	{
		pps[i].id = i + 1;
		pps[i].turn.store(0);
		auto& t = sched.spawn(i % 2, &pingpong_work, &pps[i]);
		pps[i].task = &t;
		sched.admit(t);
	}
	auto& timed = sched.spawn(0, &timed_work, nullptr);
	sched.admit(timed);

	// Driver: whenever a fiber hands over (odd turn), flip back and wake it.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	std::vector<std::thread> drivers;
	for (unsigned d = 0; d < 2; ++d)
	{
		drivers.emplace_back([&, d]
		{
			unsigned spins = 0;
			while (g_done.load() < n_fibers + 1 && std::chrono::steady_clock::now() < deadline)
			{
				for (unsigned i = d; i < n_fibers; i += 2)
				{
					unsigned v = pps[i].turn.load();
					if ((v & 1) && pps[i].turn.compare_exchange_strong(v, v + 1))
						sched.wake(*pps[i].task);
				}
				if (++spins % 128 == 0)
					std::this_thread::yield();
			}
		});
	}
	for (auto& t : drivers)
		t.join();

	if (g_done.load() < n_fibers + 1)
	{
		std::fprintf(stderr, "timeout: done=%u/%u parks=%llu wakes=%llu\n", g_done.load(), n_fibers + 1,
			static_cast<unsigned long long>(sched.park_count()), static_cast<unsigned long long>(sched.wake_count()));
		for (auto& pp : pps)
			std::fprintf(stderr, "  fiber %u turn=%u parked=%d\n", pp.id, pp.turn.load(), pp.task->parked() ? 1 : 0);
		return 1;
	}
	CHECK(g_tls_bad.load() == 0);
	CHECK(g_timed_elapsed.load() >= 20'000'000);
	CHECK(g_timed_elapsed.load() < 200'000'000);
	CHECK(sched.timeout_count() >= 1);
	sched.stop();
	sched.join();
	std::printf("Cell PPU dispatch fixture: fibers=%u rounds=%u parks=%llu wakes=%llu timeouts=%llu timed=%.1fms ran=%llu tls_ok\n",
		n_fibers, n_rounds,
		static_cast<unsigned long long>(sched.park_count()),
		static_cast<unsigned long long>(sched.wake_count()),
		static_cast<unsigned long long>(sched.timeout_count()),
		g_timed_elapsed.load() / 1e6,
		static_cast<unsigned long long>(sched.ran_count()));
	return 0;
}
