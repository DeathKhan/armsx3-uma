#include "Utilities/CellFiberActivation.h"
#include "Utilities/CellScheduler.h"

#include "Emu/Memory/vm_locking.h"

extern thread_local std::string (*g_tls_log_prefix)();

namespace cell::fiber_tls
{
bool on_fiber_host() noexcept
{
	return cell::scheduler::current_task() != nullptr;
}

cpu_activation capture()
{
	cpu_activation snap;
	snap.cpu = cpu_thread::get_current();
	snap.log_prefix = g_tls_log_prefix;
	snap.vm_passive = vm::g_tls_locked;
	return snap;
}

void deactivate_current()
{
	if (auto* ptr = vm::g_tls_locked)
	{
		if (auto* cpu = cpu_thread::get_current())
			ptr->compare_and_swap(cpu, nullptr);
	}
}

void restore(const cpu_activation& snap)
{
	if (snap.log_prefix)
		g_tls_log_prefix = snap.log_prefix;
	(void)snap.cpu;
	(void)snap.vm_passive;
}
}
