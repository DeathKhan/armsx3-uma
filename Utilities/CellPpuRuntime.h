#pragma once

#include "Utilities/Cell.h"
#include "Utilities/CellScheduler.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class thread_base;

// Two prime-core scheduler hosts for PPU fibers (CELL_FIBER_AUDIT.md §scoped plan 1).
// Hosts are created when ARMSX3_CELL=1. PPU named_thread conversion runs when
// fiber_dispatch_enabled() (ARMSX3_CELL_FIBER=1). Destroying a suspended guest
// fiber is intentionally fatal (CellFiber contract).
namespace cell
{
class ppu_runtime final
{
public:
	using guest_entry = std::uint64_t (*)(thread_base*);

	static ppu_runtime& get();

	void ensure_started();
	void stop();

	bool hosts_live() const noexcept { return m_live.load(std::memory_order_acquire); }
	unsigned host_count() const noexcept { return m_scheduler ? m_scheduler->host_count() : 0; }
	scheduler* sched() noexcept { return m_scheduler.get(); }

	static bool fiber_dispatch_enabled() noexcept { return cell::fiber_dispatch_enabled(); }

	void pin_hosts_to_upper(std::uint64_t upper_mask);

	// Spawn a guest PPU fiber bound to one host. Returns the fiber task.
	fiber_task& spawn_guest(thread_base& thread, guest_entry entry, std::string (*describe)(thread_base*) = nullptr);

	std::uint64_t dispatch_count() const noexcept { return m_dispatch.load(std::memory_order_relaxed); }

private:
	ppu_runtime() = default;
	void request_host_pins_locked();

	struct guest_arg
	{
		thread_base* thread = nullptr;
		guest_entry entry = nullptr;
		ppu_runtime* runtime = nullptr;
	};
	static void guest_work(fiber_task& task, void* p);

	// Stall watchdog: when no fiber has run for a while but guests are live,
	// log every guest's state, park call chain and dump_misc once per stall.
	struct guest_rec
	{
		fiber_task* task = nullptr;
		thread_base* thread = nullptr;
		std::string (*describe)(thread_base*) = nullptr;
		std::atomic<bool> done{false};
	};
	void watchdog_main();
	void dump_stall(std::uint64_t idle_ms);
	std::vector<std::unique_ptr<guest_rec>> m_guests;
	std::thread m_watchdog;
	std::atomic<bool> m_watchdog_stop{false};

	std::mutex m_mutex;
	std::unique_ptr<scheduler> m_scheduler;
	std::atomic<bool> m_live{false};
	std::uint64_t m_upper_mask{0};
	bool m_pinned{false};
	std::atomic<std::uint64_t> m_dispatch{0};
	std::atomic<unsigned> m_rr{0};
};

void ppu_runtime_on_cell_policy(std::uint64_t upper_mask);
}
