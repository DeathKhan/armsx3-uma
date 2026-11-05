#include "Utilities/CellScheduler.h"

#include <stdexcept>
#include <utility>

namespace cell
{
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

void scheduler::admit(fiber_task& task)
{
	if (task.m_scheduler != this)
		throw std::logic_error("Cell scheduler: foreign task");
	if (m_stopping.load(std::memory_order_acquire))
		throw std::logic_error("Cell scheduler: stopped");
	bool expected = false;
	if (!task.m_admitted.compare_exchange_strong(expected, true))
		throw std::logic_error("Cell scheduler: already admitted");
	auto& host = *m_hosts.at(task.m_host);
	{
		std::lock_guard lock(host.mutex);
		host.ready.push_back(&task);
	}
	host.cv.notify_one();
}

void scheduler::requeue(fiber_task& task)
{
	if (task.m_scheduler != this)
		throw std::logic_error("Cell scheduler: foreign task");
	if (m_stopping.load(std::memory_order_acquire))
		return;
	task.m_parked.store(false, std::memory_order_release);
	auto& host = *m_hosts.at(task.m_host);
	{
		std::lock_guard lock(host.mutex);
		host.ready.push_back(&task);
	}
	host.cv.notify_one();
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
	for (;;)
	{
		fiber_task* task = nullptr;
		{
			std::unique_lock lock(host.mutex);
			host.cv.wait(lock, [&] { return host.stop || !host.ready.empty(); });
			if (host.stop && host.ready.empty())
				return;
			task = host.ready.front();
			host.ready.erase(host.ready.begin());
		}

		const auto ticket = m_ticket.fetch_add(1, std::memory_order_relaxed);
		if (!claim(*task, ticket))
			continue;

		t_current = task;
		bool alive = false;
		try
		{
			alive = task->m_fiber.resume();
			m_ran.fetch_add(1, std::memory_order_relaxed);
		}
		catch (...)
		{
			release(*task, ticket);
			t_current = nullptr;
			throw;
		}
		release(*task, ticket);
		t_current = nullptr;

		if (alive && !m_stopping.load(std::memory_order_acquire) && !task->m_cancel.load(std::memory_order_acquire)
			&& !task->m_parked.load(std::memory_order_acquire))
		{
			std::lock_guard lock(host.mutex);
			host.ready.push_back(task);
			host.cv.notify_one();
		}
	}
}
}
