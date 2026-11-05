#pragma once

#include "Utilities/Cell.h"
#include "Utilities/CellScheduler.h"

#include <atomic>
#include <cstdlib>
#include <string_view>
#include <cstdint>
#include <memory>
#include <mutex>

// Two prime-core scheduler hosts for PPU fibers (CELL_FIBER_AUDIT.md §scoped plan 1).
// Hosts are created when ARMSX3_CELL=1; PPU named_thread conversion is staged behind
// fiber_dispatch_enabled() until wait/TLS/savestate hooks land. Destroying a suspended
// guest fiber is intentionally fatal (CellFiber contract).
namespace cell
{
class ppu_runtime final
{
public:
	static ppu_runtime& get();

	// Idempotent. Safe after driver_env import (cell::initialize).
	void ensure_started();
	void stop();

	bool hosts_live() const noexcept { return m_live.load(std::memory_order_acquire); }
	unsigned host_count() const noexcept { return m_scheduler ? m_scheduler->host_count() : 0; }
	scheduler* sched() noexcept { return m_scheduler.get(); }

	// Stays false until named_thread<ppu_thread> is converted and every audit wait path
	// is hooked. Topology + wait continue to use native PPU threads meanwhile.
	static bool fiber_dispatch_enabled() noexcept
	{
		if (!enabled()) return false;
		const char* env = std::getenv("ARMSX3_CELL_FIBER");
		return env && std::string_view(env) == "1";
	}

	void pin_hosts_to_upper(std::uint64_t upper_mask);

private:
	ppu_runtime() = default;
	void request_host_pins_locked();
	std::mutex m_mutex;
	std::unique_ptr<scheduler> m_scheduler;
	std::atomic<bool> m_live{false};
	std::uint64_t m_upper_mask{0};
	bool m_pinned{false};
};

void ppu_runtime_on_cell_policy(std::uint64_t upper_mask);
}
