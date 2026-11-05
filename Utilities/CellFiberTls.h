#pragma once

#include <cstddef>

// Per-fiber thread_local swap (CELL_FIBER_AUDIT.md TLS matrix). Guest PPU code
// reads host-thread thread_locals (current thread, log prefix, vm passive-lock
// slot, atomic-wait callbacks, lv2 notify batches). A fiber never migrates
// between hosts, so TLS addresses stay valid; only the values must follow the
// fiber. Each host loads a fiber's saved values before resuming it and saves
// them back when it yields, then restores the host's pristine values.
//
// Register at static-init time only (before any host starts); the block layout
// is fixed afterwards. Values are copied bytewise: register only trivially
// relocatable objects (pointers, integers, std::vector, plain arrays).
namespace cell::fiber_tls
{
using addr_fn = void* (*)();

bool register_slot(addr_fn addr, std::size_t size) noexcept;
std::size_t block_size() noexcept;
void save(std::byte* block) noexcept;
void load(const std::byte* block) noexcept;
}

#define CELL_FIBER_TLS_CONCAT2(a, b) a##b
#define CELL_FIBER_TLS_CONCAT(a, b) CELL_FIBER_TLS_CONCAT2(a, b)
// Register a thread_local visible at this point (namespace scope or member function).
#define CELL_FIBER_TLS_EXPR(var) ::cell::fiber_tls::register_slot([]() -> void* { return static_cast<void*>(&(var)); }, sizeof(var))
#define CELL_FIBER_TLS(var) [[maybe_unused]] static const bool CELL_FIBER_TLS_CONCAT(s_cell_fiber_tls_, __LINE__) = CELL_FIBER_TLS_EXPR(var)
