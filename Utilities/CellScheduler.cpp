#include "Utilities/CellScheduler.h"
#include "Utilities/CellFiberTls.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <utility>

namespace cell
{
namespace fiber_tls
{
namespace
{
struct slot
{
	addr_fn addr;
	std::size_t size;
	std::size_t offset;
};

std::vector<slot>& slots()
{
	static std::vector<slot> s_slots;
	return s_slots;
}

std::size_t g_block_size = 0;
}

bool register_slot(addr_fn addr, std::size_t size) noexcept
{
	slots().push_back(slot{addr, size, g_block_size});
	g_block_size += (size + 15) & ~std::size_t{15};
	return true;
}

std::size_t block_size() noexcept
{
	return g_block_size;
}

void save(std::byte* block) noexcept
{
	for (const auto& s : slots())
		std::memcpy(block + s.offset, s.addr(), s.size);
}

void load(const std::byte* block) noexcept
{
	for (const auto& s : slots())
		std::memcpy(s.addr(), block + s.offset, s.size);
}
}

thread_local fiber_task* scheduler::t_current = nullptr;

fiber_task::fiber_task(scheduler& owner, unsigned host_index, work_type work, void* argument, std::size_t stack_size)
	: m_scheduler(&owner)
	, m_host(host_index)
	, m_work(work)
	, m_argument(argument)
	, m_fiber(&fiber_task::fiber_entry, this, stack_size)
{
	if (!work)
		throw std::invalid_argument("Cell scheduler: null fiber work");
	if (host_index >= owner.host_count())
		throw std::out_of_range("Cell scheduler: host index");
}

fiber_task::~fiber_task()
{
	if (!m_fiber.finished() && m_admitted.load(std::memory_order_acquire))
		std::terminate();
}

void fiber_task::yield()
{
	if (!m_scheduler)
		throw std::logic_error("Cell scheduler: yield without scheduler");
	m_scheduler->park_current(*this);
}

void fiber_task::park(std::uint64_t deadline_ns)
{
	if (!m_scheduler)
		throw std::logic_error("Cell scheduler: park without scheduler");
	if (m_wake.exchange(false))
		return;

	// Dekker pair with scheduler::wake: publish PARKING, then re-read the wake
	// flag. A waker stores the flag, then reads the state. Both seq_cst.
	m_state.store(state_parking);
	if (m_wake.exchange(false))
	{
		unsigned expected = state_parking;
		m_state.compare_exchange_strong(expected, state_run); // a waker may have set RUN already
		return;
	}

	if (deadline_ns)
		m_scheduler->add_timer(*this, deadline_ns);
	m_scheduler->m_parks.fetch_add(1, std::memory_order_relaxed);

	// Record the parking call chain for stall dumps (AArch64 keeps frame records).
	{
		unsigned n = 0;
#if defined(__aarch64__)
		auto fp = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
		const auto lo = reinterpret_cast<std::uintptr_t>(m_fiber.stack_begin());
		const auto hi = lo + m_fiber.stack_size();
		while (n < park_frame_count && fp >= lo && fp + 16 <= hi && !(fp & 7))
		{
			const auto* rec = reinterpret_cast<const std::uintptr_t*>(fp);
			m_park_frames[n++] = rec[1];
			if (rec[0] <= fp)
				break;
			fp = rec[0];
		}
#endif
		for (unsigned i = n; i < park_frame_count; ++i)
			m_park_frames[i] = 0;
		m_park_ns.store(scheduler::now_ns(), std::memory_order_relaxed);
	}
	m_scheduler->park_current(*this);

	// Resumed by a wake (state is RUN again). Callers recheck their predicate
	// after every park, and wakers change it before waking, so the flag the
	// wake left behind is redundant now.
	m_wake.store(false);
}

void fiber_task::fiber_entry(fiber& self, void* argument)
{
	auto* task = static_cast<fiber_task*>(argument);
	scheduler::t_current = task;
	try
	{
		task->m_work(*task, task->m_argument);
	}
	catch (...)
	{
		scheduler::t_current = nullptr;
		throw;
	}
	scheduler::t_current = nullptr;
	(void)self;
}

std::uint64_t scheduler::now_ns() noexcept
{
	return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

scheduler::scheduler(unsigned host_count)
{
	if (host_count == 0 || host_count > 8)
		throw std::invalid_argument("Cell scheduler: host_count");
	m_hosts.reserve(host_count);
	for (unsigned i = 0; i < host_count; ++i)
	{
		auto host = std::make_unique<host_state>();
		host->index = i;
		host->thread = std::thread([this, h = host.get()] { host_main(*h); });
		m_hosts.push_back(std::move(host));
	}
}

scheduler::~scheduler()
{
	stop();
	join();
}

fiber_task& scheduler::spawn(unsigned host_index, fiber_task::work_type work, void* argument, std::size_t stack_size)
{
	auto task = std::make_unique<fiber_task>(*this, host_index, work, argument, stack_size);
	fiber_task* raw = task.get();
	std::lock_guard lock(m_tasks_mutex);
	m_tasks.push_back(std::move(task));
	return *raw;
}

void scheduler::push_ready(fiber_task& task)
{
	auto& host = *m_hosts.at(task.m_host);
	{
		std::lock_guard lock(host.mutex);
		host.ready.push_back(&task);
	}
	host.cv.notify_one();
}

void scheduler::admit(fiber_task& task)
{
	if (task.m_scheduler != this)
		throw std::logic_error("Cell scheduler: foreign task");
	if (m_stopping.load(std::memory_order_acquire))
		throw std::logic_error("Cell scheduler: stopped");
	bool expected = false;
	if (!task.m_admitted.compare_exchange_strong(expected, true))
		throw std::logic_error("Cell scheduler: already admitted");
	push_ready(task);
}

void scheduler::wake(fiber_task& task)
{
	if (task.m_scheduler != this)
		throw std::logic_error("Cell scheduler: foreign task");
	m_wakes.fetch_add(1, std::memory_order_relaxed);
	task.m_wake.store(true);
	unsigned state = task.m_state.load();
	for (;;)
	{
		if (state == fiber_task::state_run)
			return; // running or queued: its next park sees the flag
		if (task.m_state.compare_exchange_weak(state, fiber_task::state_run))
		{
			// PARKING: the host that is switching it out requeues it.
			// PARKED: it is on no queue; this waker owns the requeue.
			if (state == fiber_task::state_parked && !m_stopping.load(std::memory_order_acquire))
				push_ready(task);
			return;
		}
	}
}

void scheduler::add_timer(fiber_task& task, std::uint64_t deadline_ns)
{
	auto& host = *m_hosts.at(task.m_host);
	std::lock_guard lock(host.mutex);
	host.timers.emplace_back(deadline_ns, &task);
	std::push_heap(host.timers.begin(), host.timers.end(), std::greater<>{});
}

unsigned scheduler::preempt_scan(std::uint64_t slice_ns)
{
	unsigned sent = 0;
	const std::uint64_t now = now_ns();
	for (auto& h : m_hosts)
	{
		auto& host = *h;
		fiber_task* task = host.running.load(std::memory_order_acquire);
		if (!task || !task->m_poke)
			continue;
		if (now - host.run_start_ns.load(std::memory_order_relaxed) < slice_ns)
			continue;
		bool others = false;
		{
			std::lock_guard lock(host.mutex);
			others = !host.ready.empty() || (!host.timers.empty() && host.timers.front().first <= now);
		}
		if (!others || task->m_preempt.exchange(true))
			continue;
		// The task stays alive while it is running on this host.
		task->m_poke(task->m_poke_arg);
		m_preempts.fetch_add(1, std::memory_order_relaxed);
		sent++;
	}
	return sent;
}

void scheduler::host_depth(unsigned host_index, std::size_t& ready, std::size_t& timers)
{
	auto& host = *m_hosts.at(host_index);
	std::lock_guard lock(host.mutex);
	ready = host.ready.size();
	timers = host.timers.size();
}

void scheduler::signal_host(unsigned host_index)
{
	auto& host = *m_hosts.at(host_index);
	host.cv.notify_one();
}

void scheduler::stop()
{
	m_stopping.store(true, std::memory_order_release);
	for (auto& host : m_hosts)
	{
		{
			std::lock_guard lock(host->mutex);
			host->stop = true;
		}
		host->cv.notify_all();
	}
}

void scheduler::join()
{
	for (auto& host : m_hosts)
	{
		if (host->thread.joinable())
			host->thread.join();
	}
}

bool scheduler::claim(fiber_task& task, std::uint64_t ticket)
{
	std::uint64_t expected = 0;
	return task.m_owner_ticket.compare_exchange_strong(expected, ticket, std::memory_order_acq_rel);
}

void scheduler::release(fiber_task& task, std::uint64_t ticket)
{
	std::uint64_t expected = ticket;
	if (!task.m_owner_ticket.compare_exchange_strong(expected, 0, std::memory_order_acq_rel))
		throw std::logic_error("Cell scheduler: owner mismatch on release");
}

void scheduler::park_current(fiber_task& task)
{
	if (t_current != &task)
		throw std::logic_error("Cell scheduler: park of non-current fiber");
	m_yields.fetch_add(1, std::memory_order_relaxed);
	task.m_fiber.yield();
}

void scheduler::host_main(host_state& host)
{
	host.pristine_tls.resize(fiber_tls::block_size());
	fiber_tls::save(host.pristine_tls.data());

	std::vector<fiber_task*> expired;

	// Wake every guest whose timed park is due. Runs on every scheduling pass,
	// not only when the host idles: a busy host must still release timed waits.
	// Called with the host lock held; drops it around the wakes.
	const auto expire_timers = [&](std::unique_lock<std::mutex>& lock)
	{
		const std::uint64_t now = now_ns();
		while (!host.timers.empty() && host.timers.front().first <= now)
		{
			std::pop_heap(host.timers.begin(), host.timers.end(), std::greater<>{});
			expired.push_back(host.timers.back().second);
			host.timers.pop_back();
		}
		if (expired.empty())
			return;
		lock.unlock();
		for (auto* t : expired)
		{
			// Stale entries (woken early) only cause a spurious wake.
			if (t->m_state.load(std::memory_order_acquire) != fiber_task::state_run)
				m_timeouts.fetch_add(1, std::memory_order_relaxed);
			wake(*t);
		}
		expired.clear();
		lock.lock();
	};

	for (;;)
	{
		fiber_task* task = nullptr;
		{
			std::unique_lock lock(host.mutex);
			for (;;)
			{
				if (host.stop && host.ready.empty())
					return;
				expire_timers(lock);
				if (!host.ready.empty())
					break;
				if (host.timers.empty())
				{
					host.cv.wait(lock);
					continue;
				}
				const std::uint64_t now = now_ns();
				const std::uint64_t first = host.timers.front().first;
				if (first > now)
					host.cv.wait_for(lock, std::chrono::nanoseconds(first - now));
			}
			task = host.ready.front();
			host.ready.erase(host.ready.begin());
		}

		const auto ticket = m_ticket.fetch_add(1, std::memory_order_relaxed);
		if (!claim(*task, ticket))
			continue;

		if (task->m_tls.empty())
			task->m_tls = host.pristine_tls;
		fiber_tls::load(task->m_tls.data());
		t_current = task;
		task->m_preempt.store(false, std::memory_order_relaxed);
		host.run_start_ns.store(now_ns(), std::memory_order_relaxed);
		host.running.store(task, std::memory_order_release);
		bool alive = false;
		try
		{
			alive = task->m_fiber.resume();
			m_ran.fetch_add(1, std::memory_order_relaxed);
		}
		catch (...)
		{
			host.running.store(nullptr, std::memory_order_release);
			fiber_tls::save(task->m_tls.data());
			fiber_tls::load(host.pristine_tls.data());
			release(*task, ticket);
			t_current = nullptr;
			throw;
		}
		host.running.store(nullptr, std::memory_order_release);
		fiber_tls::save(task->m_tls.data());
		fiber_tls::load(host.pristine_tls.data());
		release(*task, ticket);
		t_current = nullptr;

		if (!alive || m_stopping.load(std::memory_order_acquire) || task->m_cancel.load(std::memory_order_acquire))
			continue;

		// A parking fiber goes off-queue unless a wake already flipped it to RUN.
		unsigned expected = fiber_task::state_parking;
		if (task->m_state.compare_exchange_strong(expected, fiber_task::state_parked))
			continue;

		// Requeue a yielded/preempted guest behind any guest whose timer is due.
		std::unique_lock lock(host.mutex);
		expire_timers(lock);
		host.ready.push_back(task);
	}
}
}
