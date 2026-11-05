#pragma once
#include <cstdint>
class spu_thread;

namespace cell_replay
{
// Lazy read: driver_env is applied after the shared library is loaded.
bool enabled();
// Test-only standalone-ELF entry. Returns true when cpu_task must return.
bool run_isolated(spu_thread& spu);
// LLVM boundary hook: no side effects when no replay is active on this host thread.
void intercept_boundary(spu_thread* spu, std::uint32_t opcode);
// Guard the existing fallback and state-check paths during a replay.
void reject_fallback(spu_thread& spu);
void on_state_check(spu_thread& spu);
}
