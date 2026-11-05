#pragma once

#include "Utilities/CellFiber.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Two-host cooperative PPU fiber scheduler foundation. Not wired into the
// emulator runtime; see CELL_FIBER_AUDIT.md. Host fixtures own the lifecycle.
// Fibers never migrate between hosts while a native continuation exists.
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
	bool parked() const noexcept { return m_parked.load(std::memory_order_acquire); }
	void set_parked(bool v) noexcept { m_parked.store(v, std::memory_order_release); }

private:
	friend class scheduler;
	static void fiber_entry(fiber& self, void* argument);

	scheduler* m_scheduler = nullptr;
	unsigned m_host = 0;
	work_type m_work = nullptr;
	void* m_argument = nullptr;
	fiber m_fiber;
	std::atomic<std::uint64_t> m_owner_ticket{0};
	std::atomic<bool> m_admitted{false};
	std::atomic<bool> m_cancel{false};
	std::atomic<bool> m_parked{false};
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
	void requeue(fiber_task& task); // admit again after a wait-park
	void signal_host(unsigned host_index);
	void stop();
	void join();

	std::uint64_t ran_count() const noexcept { return m_ran.load(std::memory_order_relaxed); }
	std::uint64_t yield_count() const noexcept { return m_yields.load(std::memory_order_relaxed); }
	static fiber_task* current_task() noexcept { return t_current; }

private:
	friend class fiber_task;
	struct host_state
	{
		unsigned index = 0;
		std::thread thread;
		std::mutex mutex;
		std::condition_variable cv;
		std::vector<fiber_task*> ready;
		bool stop = false;
	};

	void host_main(host_state& host);
	bool claim(fiber_task& task, std::uint64_t ticket);
	void release(fiber_task& task, std::uint64_t ticket);
	void park_current(fiber_task& task);

	std::vector<std::unique_ptr<host_state>> m_hosts;
	std::mutex m_tasks_mutex;
	std::vector<std::unique_ptr<fiber_task>> m_tasks;
	std::atomic<std::uint64_t> m_ticket{1};
	std::atomic<std::uint64_t> m_ran{0};
	std::atomic<std::uint64_t> m_yields{0};
	std::atomic<bool> m_stopping{false};
	static thread_local fiber_task* t_current;
};
}
