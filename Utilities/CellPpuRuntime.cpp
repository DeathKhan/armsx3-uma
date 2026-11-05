#include "Utilities/CellPpuRuntime.h"
#include "Utilities/CellFiberActivation.h"
#include "Utilities/Thread.h"

#include "util/logs.hpp"

#include <stdexcept>

#if defined(__linux__)
#include <sched.h>
#endif

LOG_CHANNEL(cell_fiber);

namespace cell
{
namespace
{
struct pin_arg
{
	unsigned cpu = 0;
};

void pin_work(fiber_task& task, void* p)
{
	auto* a = static_cast<pin_arg*>(p);
#if defined(__linux__)
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(static_cast<int>(a->cpu), &set);
	sched_setaffinity(0, sizeof(set), &set);
#endif
	cell_fiber.notice("ARMSX3_CELL fiber host pinned cpu=%u", a->cpu);
	delete a;
	(void)task;
}
}

ppu_runtime& ppu_runtime::get()
{
	static ppu_runtime runtime;
	return runtime;
}

void ppu_runtime::pin_hosts_to_upper(std::uint64_t upper_mask)
{
	m_upper_mask = upper_mask;
}

void ppu_runtime::request_host_pins_locked()
{
	if (!m_scheduler || !m_upper_mask || m_pinned) return;
	unsigned host = 0;
	for (unsigned cpu = 0; cpu < 64 && host < m_scheduler->host_count(); ++cpu)
	{
		if (!(m_upper_mask & (std::uint64_t{1} << cpu))) continue;
		auto* arg = new pin_arg{cpu};
		auto& task = m_scheduler->spawn(host, &pin_work, arg);
		m_scheduler->admit(task);
		++host;
	}
	if (host)
	{
		m_pinned = true;
		cell_fiber.notice("ARMSX3_CELL fiber hosts: pin requested count=%u upper=0x%x",
			host, static_cast<unsigned>(m_upper_mask));
	}
}

void ppu_runtime::ensure_started()
{
	if (!enabled()) return;
	std::lock_guard lock(m_mutex);
	if (m_scheduler)
	{
		request_host_pins_locked();
		return;
	}

	m_scheduler = std::make_unique<scheduler>(2);
	m_live.store(true, std::memory_order_release);
	cell_fiber.notice("ARMSX3_CELL fiber hosts: started count=%u dispatch=%d upper=0x%x",
		m_scheduler->host_count(), fiber_dispatch_enabled() ? 1 : 0, static_cast<unsigned>(m_upper_mask));
	request_host_pins_locked();
}

void ppu_runtime::stop()
{
	std::lock_guard lock(m_mutex);
	if (!m_scheduler) return;
	m_scheduler->stop();
	m_scheduler->join();
	m_scheduler.reset();
	m_pinned = false;
	m_upper_mask = 0;
	m_live.store(false, std::memory_order_release);
	cell_fiber.notice("ARMSX3_CELL fiber hosts: stopped");
}

void ppu_runtime::guest_work(fiber_task& task, void* p)
{
	auto* arg = static_cast<guest_arg*>(p);
	auto* rt = arg->runtime;
	auto* thread = arg->thread;
	auto entry = arg->entry;
	delete arg;

	rt->m_dispatch.fetch_add(1, std::memory_order_relaxed);
	cell_fiber.notice("ARMSX3_CELL fiber dispatch: guest start host=%u total=%u",
		task.host_index(), static_cast<unsigned>(rt->m_dispatch.load(std::memory_order_relaxed)));

	// Run the named_thread entry (initialize + Context::operator() + finalize state).
	// Must NOT call the pthread trampoline that pthread_exit's the host.
	(void)entry(thread);

	cell_fiber.notice("ARMSX3_CELL fiber dispatch: guest done host=%u", task.host_index());
}

fiber_task& ppu_runtime::spawn_guest(thread_base& thread, guest_entry entry)
{
	ensure_started();
	std::lock_guard lock(m_mutex);
	if (!m_scheduler)
		throw std::logic_error("Cell PPU runtime: no scheduler");

	const unsigned hosts = m_scheduler->host_count();
	const unsigned host = hosts ? (m_rr.fetch_add(1, std::memory_order_relaxed) % hosts) : 0;
	auto* arg = new guest_arg{&thread, entry, this};
	auto& task = m_scheduler->spawn(host, &guest_work, arg);
	thread.bind_cell_fiber(&task);
	m_scheduler->admit(task);
	return task;
}

void ppu_runtime::park_current_for_wait()
{
	auto* task = scheduler::current_task();
	if (!task) return;
	const auto snap = fiber_tls::capture();
	fiber_tls::deactivate_current();
	task->set_parked(true);
	task->yield();
	fiber_tls::restore(snap);
}

void ppu_runtime::wake_guest(thread_base& thread)
{
	if (auto* task = thread.cell_fiber_task())
	{
		if (task->parked())
		{
			std::lock_guard lock(m_mutex);
			if (m_scheduler)
				m_scheduler->requeue(*task);
		}
	}
}

void ppu_runtime_on_cell_policy(std::uint64_t upper_mask)
{
	auto& rt = ppu_runtime::get();
	rt.pin_hosts_to_upper(upper_mask);
	rt.ensure_started();
}

bool try_fiber_park()
{
	if (!fiber_dispatch_enabled()) return false;
	if (!scheduler::current_task()) return false;
	ppu_runtime::get().park_current_for_wait();
	return true;
}
}
