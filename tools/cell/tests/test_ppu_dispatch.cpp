#include "Utilities/Cell.h"
#include "Utilities/CellFiber.h"
#include "Utilities/CellScheduler.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

// Host-only fixture: two hosts run N cooperative "PPU-like" fibers that yield
// and park/unpark, proving dispatch>0 without the emulator runtime.
static std::atomic<unsigned> g_ran{0};
static std::atomic<unsigned> g_parks{0};

struct work_arg
{
	unsigned id = 0;
	unsigned yields = 0;
	cell::scheduler* sched = nullptr;
};

static void ppu_like_work(cell::fiber_task& task, void* p)
{
	auto* a = static_cast<work_arg*>(p);
	g_ran.fetch_add(1, std::memory_order_relaxed);
	for (unsigned i = 0; i < a->yields; ++i)
	{
		task.yield();
	}
	// Park once, then expect requeue from the driver thread.
	task.set_parked(true);
	g_parks.fetch_add(1, std::memory_order_relaxed);
	task.yield();
	assert(!task.parked());
	g_ran.fetch_add(1, std::memory_order_relaxed);
	(void)a;
}

int main()
{
	cell::initialize();
	setenv("ARMSX3_CELL", "1", 1);
	setenv("ARMSX3_CELL_FIBER", "1", 1);

	cell::scheduler sched(2);
	constexpr unsigned n = 8;
	std::vector<work_arg> args(n);
	std::vector<cell::fiber_task*> tasks;
	tasks.reserve(n);
	for (unsigned i = 0; i < n; ++i)
	{
		args[i] = work_arg{i, 4, &sched};
		auto& t = sched.spawn(i % 2, &ppu_like_work, &args[i]);
		tasks.push_back(&t);
		sched.admit(t);
	}

	// Wait until all have parked once.
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (g_parks.load() < n)
	{
		if (std::chrono::steady_clock::now() > deadline)
		{
			std::fprintf(stderr, "timeout waiting for parks (%u/%u) ran=%u\n",
				g_parks.load(), n, g_ran.load());
			return 1;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	for (auto* t : tasks)
		sched.requeue(*t);

	while (g_ran.load() < n * 2)
	{
		if (std::chrono::steady_clock::now() > deadline)
		{
			std::fprintf(stderr, "timeout waiting for completion ran=%u\n", g_ran.load());
			return 1;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	sched.stop();
	sched.join();
	assert(sched.ran_count() > 0);
	std::printf("Cell PPU dispatch fixture: fibers=%u parks=%u scheduler_ran=%llu yields=%llu\n",
		n, g_parks.load(),
		static_cast<unsigned long long>(sched.ran_count()),
		static_cast<unsigned long long>(sched.yield_count()));
	return 0;
}
