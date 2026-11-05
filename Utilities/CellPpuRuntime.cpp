#include "Utilities/CellPpuRuntime.h"
#include "Utilities/CellWait.h"
#include "Utilities/Thread.h"

#include "util/logs.hpp"

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <cstdlib>
#include <algorithm>
#include <dlfcn.h>

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
	if (fiber_dispatch_enabled() && !m_watchdog.joinable())
	{
		m_watchdog_stop = false;
		m_watchdog = std::thread([this] { watchdog_main(); });
		// ARMSX3_CELL_SLICE_US (driver_env): guest time slice before a waiting
		// guest on the same host gets the core. 0 disables preemption.
		const char* e = std::getenv("ARMSX3_CELL_SLICE_US");
		m_slice_ns = (e && *e ? std::strtoull(e, nullptr, 10) : 2000) * 1000;
		if (m_slice_ns)
			m_ticker = std::thread([this] { ticker_main(); });
		cell_fiber.notice("ARMSX3_CELL fiber preemption slice=%lluus", static_cast<unsigned long long>(m_slice_ns / 1000));
	}
	cell_fiber.notice("ARMSX3_CELL fiber hosts: started count=%u dispatch=%d upper=0x%x",
		m_scheduler->host_count(), fiber_dispatch_enabled() ? 1 : 0, static_cast<unsigned>(m_upper_mask));
	request_host_pins_locked();
}

void ppu_runtime::ticker_main()
{
	// 1 kHz is enough for a 2 ms slice; the scan is two locked size checks.
	while (!m_watchdog_stop.load(std::memory_order_relaxed))
	{
		std::this_thread::sleep_for(std::chrono::microseconds(std::min<std::uint64_t>(1000, std::max<std::uint64_t>(m_slice_ns / 2000, 100))));
		if (auto* s = m_scheduler.get())
			s->preempt_scan(m_slice_ns);
	}
}

void ppu_runtime::watchdog_main()
{
	std::uint64_t last_ran = ~0ull, stalled_ran = ~0ull;
	unsigned idle_s = 0;
	while (!m_watchdog_stop.load())
	{
		std::this_thread::sleep_for(std::chrono::seconds(1));
		std::lock_guard lock(m_mutex);
		if (!m_scheduler)
			continue;
		const auto ran = m_scheduler->ran_count();
		idle_s = ran == last_ran ? idle_s + 1 : 0;
		last_ran = ran;
		bool live = false;
		for (auto& g : m_guests)
			live |= !g->done;
		if (live && idle_s >= 5 && ran != stalled_ran)
		{
			stalled_ran = ran;
			dump_stall(idle_s * 1000ull);
		}
	}
}

void ppu_runtime::dump_stall(std::uint64_t idle_ms)
{
	const auto now = scheduler::now_ns();
	cell_fiber.error("ARMSX3_CELL fiber STALL: no dispatch for %llu ms; preempts=%llu parks=%llu wakes=%llu timeouts=%llu guests=%u",
		static_cast<unsigned long long>(idle_ms), static_cast<unsigned long long>(m_scheduler->preempt_count()),
		static_cast<unsigned long long>(m_scheduler->park_count()),
		static_cast<unsigned long long>(m_scheduler->wake_count()), static_cast<unsigned long long>(m_scheduler->timeout_count()),
		static_cast<unsigned>(m_guests.size()));
	for (unsigned h = 0; h < m_scheduler->host_count(); ++h)
	{
		std::size_t ready = 0, timers = 0;
		m_scheduler->host_depth(h, ready, timers);
		cell_fiber.error("ARMSX3_CELL fiber host=%u ready=%u timers=%u", h, static_cast<unsigned>(ready), static_cast<unsigned>(timers));
	}
	for (auto& g : m_guests)
	{
		if (g->done)
			continue;
		const auto* task = g->task;
		std::string frames;
		for (unsigned i = 0; i < fiber_task::park_frame_count && task->park_frames()[i]; ++i)
		{
			const auto pc = task->park_frames()[i];
			Dl_info info{};
			if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fbase)
				frames += fmt::format(" %s+0x%x", std::strrchr(info.dli_fname, '/') ? std::strrchr(info.dli_fname, '/') + 1 : info.dli_fname,
					static_cast<u64>(pc - reinterpret_cast<std::uintptr_t>(info.dli_fbase)));
			else
				frames += fmt::format(" 0x%x", static_cast<u64>(pc));
		}
		std::string desc = g->describe ? g->describe(g->thread) : std::string("?");
		for (auto& c : desc)
			if (c == '\n') c = '|';
		std::string wait;
		if (const void* data = task->wait_data())
		{
			// Guest/emulator words stay mapped while their waiter is alive.
			const u32 cur = static_cast<const std::atomic<u32>*>(data)->load();
			const u32 sync = task->wait_sync() ? task->wait_sync()->load() : 0xffffffffu;
			wait = fmt::format(" wait=0x%x old=0x%x cur=0x%x sync=%u%s", reinterpret_cast<std::uintptr_t>(data), task->wait_old(), cur, sync,
				cur != task->wait_old() ? " LOST-WAKE?" : "");
		}
		cell_fiber.error("ARMSX3_CELL fiber guest host=%u state=%u wake=%d parked_ms=%llu%s desc=%s frames:%s",
			task->host_index(), task->state(), task->wake_pending() ? 1 : 0,
			static_cast<unsigned long long>(task->park_ns() ? (now - task->park_ns()) / 1000000 : 0),
			wait, desc, frames);
	}
}

void ppu_runtime::stop()
{
	m_watchdog_stop = true;
	if (m_watchdog.joinable() && m_watchdog.get_id() != std::this_thread::get_id())
		m_watchdog.join();
	if (m_ticker.joinable())
		m_ticker.join();
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
	{
		std::lock_guard lock(rt->m_mutex);
		for (auto& g : rt->m_guests)
			if (g->task == &task)
				g->done = true;
	}
}

fiber_task& ppu_runtime::spawn_guest(thread_base& thread, guest_entry entry, std::string (*describe)(thread_base*), void (*poke)(thread_base*))
{
	ensure_started();
	std::lock_guard lock(m_mutex);
	if (!m_scheduler)
		throw std::logic_error("Cell PPU runtime: no scheduler");

	const unsigned hosts = m_scheduler->host_count();
	const unsigned host = hosts ? (m_rr.fetch_add(1, std::memory_order_relaxed) % hosts) : 0;
	auto* arg = new guest_arg{&thread, entry, this};
	auto rec = std::make_unique<guest_rec>();
	rec->thread = &thread;
	rec->describe = describe;
	rec->poke = poke;
	// Native PPU threads get 8 MiB stacks on Android; guest HLE and LLVM frames
	// are deep. Only touched pages are committed.
	auto& task = m_scheduler->spawn(host, &guest_work, arg, 8 * 1024 * 1024);
	if (poke)
		task.set_poke(reinterpret_cast<void (*)(void*)>(poke), &thread);
	rec->task = &task;
	m_guests.push_back(std::move(rec));
	thread.bind_cell_fiber(&task);
	m_scheduler->admit(task);
	return task;
}


void ppu_runtime_on_cell_policy(std::uint64_t upper_mask)
{
	auto& rt = ppu_runtime::get();
	rt.pin_hosts_to_upper(upper_mask);
	rt.ensure_started();
}

void* current_fiber() noexcept
{
	// Host helper tasks (pinning) never wait; any task here is a guest.
	return scheduler::current_task();
}

void park_fiber(std::uint64_t deadline_ns, const void* data, std::uint32_t old_value, const void* sync)
{
	if (auto* task = scheduler::current_task())
	{
		task->note_wait(data, old_value, static_cast<const std::atomic<std::uint32_t>*>(sync));
		task->park(deadline_ns);
	}
}

void wake_fiber(void* fiber)
{
	auto* task = static_cast<fiber_task*>(fiber);
	// The scheduler outlives every guest fiber (stop() runs after PPU teardown).
	if (auto* sched = ppu_runtime::get().sched())
		sched->wake(*task);
}

std::uint64_t fiber_now_ns() noexcept
{
	return scheduler::now_ns();
}

void fiber_preempt_point() noexcept
{
	if (auto* task = scheduler::current_task(); task && task->take_preempt())
		task->yield();
}
}
