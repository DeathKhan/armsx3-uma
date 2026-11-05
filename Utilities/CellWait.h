#pragma once

#include <atomic>
#include <cstdint>

namespace cell
{
// Paired with the registered waiter's condition word, not the guest channel.
// All phase operations are sequentially consistent. Before a kernel sleep the
// waiter publishes native mode and re-reads the condition. A notifier that saw
// event mode must have changed that condition before the re-read; FUTEX_WAIT's
// expected-value check closes the remaining check-to-sleep window.
class wait_phase
{
	std::atomic<unsigned> m_native{1};
public:
	void begin(bool events) { m_native.store(events ? 0 : 1); }
	void enter_native() { m_native.store(1); }
	void handoff_to_native()
	{
		m_native.store(1);
		// atomic_t's condition load is acquire-only in this codebase. A full
		// StoreLoad barrier is required before that recheck on weak ARM memory.
		std::atomic_thread_fence(std::memory_order_seq_cst);
	}
	bool event_mode() const { return m_native.load() == 0; }
};

// Only call with EVTSTRM available and an outer wall-clock budget. Oryon's
// exclusive-monitor WFE returns immediately, so do not arm LDAXR here. A SEV
// between the caller's predicate check and this WFE remains in the event latch.
inline void cell_wait()
{
#if defined(ARCH_ARM64) && defined(__linux__)
	__asm__ volatile("clrex\n\twfe" ::: "memory");
#endif
}

// Return true only when the notification was delivered without a syscall.
inline bool cell_signal(const wait_phase& phase)
{
#if defined(ARCH_ARM64) && defined(__linux__)
	if (phase.event_mode())
	{
		// The condition publication precedes the cross-core event.
		__asm__ volatile("dmb ishst\n\tsev" ::: "memory");
		return true;
	}
#else
	(void)phase;
#endif
	return false;
}

// PPU fiber waits (ARMSX3_CELL_FIBER). Implemented in CellPpuRuntime.cpp.
// current_fiber: the guest fiber running on this host thread, or null on any
// native thread. park_fiber parks it until wake_fiber or deadline_ns (steady
// clock, 0 = none); wakes are sticky, so a wake between the caller's predicate
// check and the park is never lost.
void* current_fiber() noexcept;
void park_fiber(std::uint64_t deadline_ns);
void wake_fiber(void* fiber);
std::uint64_t fiber_now_ns() noexcept;
}

