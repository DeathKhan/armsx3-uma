#pragma once

#include "util/asm.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace rsx
{
	// Paces a wait on something another agent produces with no wakeup we can block on: a
	// semaphore the guest writes with a plain store, a FIFO PUT pointer, a GPU query result.
	//
	// The waits this replaces parked with WFE. On this device that is not a park: between
	// event-stream ticks the thread never leaves the CPU, so the scheduler sees a core that is
	// 100% busy, the governor holds the cluster at its top clock, and the core is not available
	// to the SPU threads that are producing the very thing being waited on. Measured in
	// LittleBigPlanet gameplay on Snapdragon 8 Elite: 93% of the RSX thread's cycles, 42% of
	// the whole process, inside nv406e::semaphore_acquire, while the game held a steady 30 fps.
	//
	// So: a short hot spin, which still catches a value that lands within microseconds with no
	// wake latency at all, then real sleeps in the kernel that start short and double up to a
	// cap. The cap bounds the latency added to a long wait (0.2 ms against a 16-33 ms frame);
	// the doubling keeps a wait that ends soon after the spin from paying the whole cap. The sleeps
	// are close to what they ask for because startup sets a 1 ns timer slack (armsx3-core.cpp),
	// which every emulator thread inherits; Linux's default would add up to 50 us to each.
	class paced_wait
	{
		static constexpr auto spin_window = std::chrono::microseconds(30);
		static constexpr u32 first_sleep_us = 20;
		static constexpr u32 max_sleep_us = 200;

		std::chrono::steady_clock::time_point m_start{};
		u32 m_sleep_us = 0;

	public:
		// Start a fresh wait: the next step spins again.
		void reset()
		{
			m_start = {};
			m_sleep_us = 0;
		}

		// One step of the wait. Call it in the loop that re-checks the condition.
		void wait()
		{
			if (m_sleep_us == 0)
			{
				const auto now = std::chrono::steady_clock::now();

				if (m_start == std::chrono::steady_clock::time_point{})
				{
					m_start = now;
				}

				if (now - m_start < spin_window)
				{
					utils::pause();
					return;
				}

				m_sleep_us = first_sleep_us;
			}

			std::this_thread::sleep_for(std::chrono::microseconds(m_sleep_us));
			m_sleep_us = std::min(m_sleep_us * 2, max_sleep_us);
		}
	};
}
