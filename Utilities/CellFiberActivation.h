#pragma once

#include "Emu/CPU/CPUThread.h"

#include <cstdint>
#include <string>

// Activation/deactivation helpers for CELL_FIBER_AUDIT.md scoped plan step 2.
// These do not switch stacks; they define the TLS contract a fiber switch must
// obey. Call sites that park a PPU fiber must deactivate before yield and
// activate on resume. Native PPU threads never call these.
namespace cell::fiber_tls
{
struct cpu_activation
{
	cpu_thread* cpu = nullptr;
	void* vm_passive = nullptr;
	std::string (*log_prefix)() = nullptr;
};

cpu_activation capture();
void restore(const cpu_activation& snap);
void deactivate_current();
bool on_fiber_host() noexcept;
}
