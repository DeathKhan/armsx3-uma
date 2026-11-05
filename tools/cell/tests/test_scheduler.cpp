#include "Utilities/CellScheduler.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

using cell::fiber_task;
using cell::scheduler;

static void require(bool ok, const char* what)
{
	if (!ok)
		throw std::runtime_error(what);
}

static void simple_work(fiber_task&, void* arg)
{
	static_cast<std::atomic<int>*>(arg)->fetch_add(1, std::memory_order_relaxed);
}

static void yielding_work(fiber_task& task, void* arg)
{
	auto* counter = static_cast<std::atomic<int>*>(arg);
	for (int i = 0; i < 20; ++i)
	{
		counter->fetch_add(1, std::memory_order_relaxed);
		task.yield();
	}
}

int main()
{
	std::atomic<int> simple{0};
	{
		scheduler sched(2);
		auto& a = sched.spawn(0, &simple_work, &simple);
		auto& b = sched.spawn(1, &simple_work, &simple);
		auto& c = sched.spawn(0, &simple_work, &simple);
		sched.admit(a);
		sched.admit(b);
		sched.admit(c);
		for (int i = 0; i < 200 && simple.load() < 3; ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		require(simple.load() == 3, "simple completion");
		require(sched.ran_count() >= 3, "ran count");
		sched.stop();
		sched.join();
		require(a.finished() && b.finished() && c.finished(), "finished");
	}

	std::atomic<int> yielded{0};
	{
		scheduler sched(2);
		auto& a = sched.spawn(0, &yielding_work, &yielded);
		auto& b = sched.spawn(1, &yielding_work, &yielded);
		sched.admit(a);
		sched.admit(b);
		for (int i = 0; i < 800 && (!(a.finished() && b.finished())); ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		require(a.finished() && b.finished(), "yield finished");
		require(yielded.load() == 40, "yield counts");
		require(sched.yield_count() >= 40, "scheduler yield count");
		sched.stop();
		sched.join();
	}

	std::atomic<int> many{0};
	{
		scheduler sched(2);
		std::vector<fiber_task*> tasks;
		tasks.reserve(64);
		for (int i = 0; i < 64; ++i)
			tasks.push_back(&sched.spawn(static_cast<unsigned>(i & 1), &simple_work, &many));
		for (auto* t : tasks)
			sched.admit(*t);
		for (int i = 0; i < 400 && many.load() < 64; ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		require(many.load() == 64, "many completion");
		sched.stop();
		sched.join();
	}

	std::puts("cell scheduler fixture ok");
	return 0;
}
