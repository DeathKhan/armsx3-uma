#pragma once

#include "Utilities/CellFiber.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// Two-host cooperative PPU fiber scheduler (CELL_FIBER_AUDIT.md).
// Fibers never migrate between hosts while a native continuation exists.
//
// Wait protocol: a fiber parks itself (park/park_until) and any thread wakes it
// (scheduler::wake). Wakes are sticky: a wake that lands before the park makes
// the park return immediately, so check-then-park races never lose a wake.
// Spurious wakes are allowed; every caller rechecks its predicate.
namespace cell
{
class scheduler;

class fiber_task final
{
public:
	using work_type = void (*)(fiber_task&, void*);

	fiber_task(scheduler& owner, unsigned host_index, work_type work, void* argument = nullptr,
		std::size_t stack_size = 1024 * 1024);
	~fiber_task();
	fiber_task(const fiber_task&) = delete;
	fiber_task& operator=(const fiber_task&) = delete;

	unsigned host_index() const noexcept { return m_host; }
	bool finished() const noexcept { return m_fiber.finished(); }
	bool claimed() const noexcept { return m_owner_ticket.load(std::memory_order_acquire) != 0; }
	std::uint64_t owner_ticket() const noexcept { return m_owner_ticket.load(std::memory_order_acquire); }
	void yield();
	void request_cancel() noexcept { m_cancel.store(true, std::memory_order_release); }

	// Called by the running fiber. Returns after a wake (possibly one that
	// arrived earlier), or at deadline_ns (steady clock) when non-zero.
	void park(std::uint64_t deadline_ns = 0);
	bool parked() const noexcept { return m_state.load(std::memory_order_acquire) != state_run; }

	// Diagnostics: return addresses of the latest park (frame-pointer walk) and when it began.
	static constexpr unsigned park_frame_count = 14;
	const std::uintptr_t* park_frames() const noexcept { return m_park_frames; }
	std::uint64_t park_ns() const noexcept { return m_park_ns.load(std::memory_order_relaxed); }
	unsigned state() const noexcept { return m_state.load(std::memory_order_relaxed); }

private:
	friend class scheduler;
	static void fiber_entry(fiber& self, void* argument);

	static constexpr unsigned state_run = 0;     // runnable or running
	static constexpr unsigned state_parking = 1; // yielded to park; host decides
	static constexpr unsigned state_parked = 2;  // off every queue until a wake

	scheduler* m_scheduler = nullptr;
	unsigned m_host = 0;
	work_type m_work = nullptr;
	void* m_argument = nullptr;
	fiber m_fiber;
	std::atomic<std::uint64_t> m_owner_ticket{0};
	std::atomic<bool> m_admitted{false};
	std::atomic<bool> m_cancel{false};
	std::atomic<unsigned> m_state{state_run};
	std::atomic<bool> m_wake{false};
	std::vector<std::byte> m_tls; // fiber_tls block, filled on first run
	std::uintptr_t m_park_frames[park_frame_count]{};
	std::atomic<std::uint64_t> m_park_ns{0};
};

class scheduler final
{
public:
	explicit scheduler(unsigned host_count = 2);
	~scheduler();
	scheduler(const scheduler&) = delete;
	scheduler& operator=(const scheduler&) = delete;

	unsigned host_count() const noexcept { return static_cast<unsigned>(m_hosts.size()); }
	fiber_task& spawn(unsigned host_index, fiber_task::work_type work, void* argument = nullptr,
		std::size_t stack_size = 1024 * 1024);
	void admit(fiber_task& task);
	// Any thread. Requeues a parked fiber, or makes its next park return at once.
	void wake(fiber_task& task);
	void signal_host(unsigned host_index);
	void stop();
	void join();

	std::uint64_t ran_count() const noexcept { return m_ran.load(std::memory_order_relaxed); }
	std::uint64_t yield_count() const noexcept { return m_yields.load(std::memory_order_relaxed); }
	std::uint64_t park_count() const noexcept { return m_parks.load(std::memory_order_relaxed); }
	std::uint64_t wake_count() const noexcept { return m_wakes.load(std::memory_order_relaxed); }
	std::uint64_t timeout_count() const noexcept { return m_timeouts.load(std::memory_order_relaxed); }
	static fiber_task* current_task() noexcept { return t_current; }
	static std::uint64_t now_ns() noexcept;

private:
	friend class fiber_task;
	struct host_state
	{
		unsigned index = 0;
		std::thread thread;
		std::mutex mutex;
		std::condition_variable cv;
		std::vector<fiber_task*> ready;
		std::vector<std::pair<std::uint64_t, fiber_task*>> timers; // min-heap on deadline
		std::vector<std::byte> pristine_tls;
		bool stop = false;
	};

	void host_main(host_state& host);
	void push_ready(fiber_task& task);
	bool claim(fiber_task& task, std::uint64_t ticket);
	void release(fiber_task& task, std::uint64_t ticket);
	void park_current(fiber_task& task);
	void add_timer(fiber_task& task, std::uint64_t deadline_ns);

	std::vector<std::unique_ptr<host_state>> m_hosts;
	std::mutex m_tasks_mutex;
	std::vector<std::unique_ptr<fiber_task>> m_tasks;
	std::atomic<std::uint64_t> m_ticket{1};
	std::atomic<std::uint64_t> m_ran{0};
	std::atomic<std::uint64_t> m_yields{0};
	std::atomic<std::uint64_t> m_parks{0};
	std::atomic<std::uint64_t> m_wakes{0};
	std::atomic<std::uint64_t> m_timeouts{0};
	std::atomic<bool> m_stopping{false};
	static thread_local fiber_task* t_current;
};
}
