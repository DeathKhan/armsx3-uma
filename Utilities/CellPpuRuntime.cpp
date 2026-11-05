#include "Utilities/CellPpuRuntime.h"

#include "util/logs.hpp"

#include <pthread.h>

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
	CPU_SET(a->cpu, &set);
	pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
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

void ppu_runtime::ensure_started()
{
	if (!enabled()) return;
	std::lock_guard lock(m_mutex);
	if (m_scheduler) return;

	m_scheduler = std::make_unique<scheduler>(2);
	m_live.store(true, std::memory_order_release);
	cell_fiber.notice("ARMSX3_CELL fiber hosts: started count=%u dispatch=%d upper=0x%x",
		m_scheduler->host_count(), fiber_dispatch_enabled() ? 1 : 0, static_cast<unsigned>(m_upper_mask));

	if (m_upper_mask)
	{
		unsigned host = 0;
		for (unsigned cpu = 0; cpu < 64 && host < m_scheduler->host_count(); ++cpu)
		{
			if (!(m_upper_mask & (std::uint64_t{1} << cpu))) continue;
			auto* arg = new pin_arg{cpu};
			auto& task = m_scheduler->spawn(host, &pin_work, arg);
			m_scheduler->admit(task);
			++host;
		}
	}
}

void ppu_runtime::stop()
{
	std::lock_guard lock(m_mutex);
	if (!m_scheduler) return;
	m_scheduler->stop();
	m_scheduler->join();
	m_scheduler.reset();
	m_live.store(false, std::memory_order_release);
	cell_fiber.notice("ARMSX3_CELL fiber hosts: stopped");
}

void ppu_runtime_on_cell_policy(std::uint64_t upper_mask)
{
	auto& rt = ppu_runtime::get();
	rt.pin_hosts_to_upper(upper_mask);
	rt.ensure_started();
}
}
