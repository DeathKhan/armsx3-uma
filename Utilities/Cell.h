#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <string_view>

// This must remain lazy: Android loads driver_env.txt after loading the shared library.
namespace cell
{
inline std::atomic<bool>& environment_ready()
{
#if defined(ANDROID) || defined(__ANDROID__)
	static std::atomic<bool> ready{false};
#else
	static std::atomic<bool> ready{true};
#endif
	return ready;
}

// Android calls this after importing driver_env.txt. Named housekeeping threads
// can start earlier; their enabled() checks must not cache an absent environment.
inline void initialize() { environment_ready().store(true); }

inline bool enabled()
{
	if (!environment_ready().load()) return false;
	static const bool value = []
	{
		const char* env = std::getenv("ARMSX3_CELL");
		return env && std::string_view(env) == "1";
	}();
	return value;
}

// Only guest CPU hosts use Cell waits. RSX, timers, usleep and helper waits keep
// their established sleeping behavior; turning all atomic waits into spins was
// previously measured to lose both cycles and power.
inline bool& waiter_enabled()
{
	static thread_local bool enabled = false;
	return enabled;
}


class scoped_native_wait
{
	const bool m_previous;
public:
	scoped_native_wait() : m_previous(waiter_enabled()) { waiter_enabled() = false; }
	~scoped_native_wait() { waiter_enabled() = m_previous; }
	scoped_native_wait(const scoped_native_wait&) = delete;
	scoped_native_wait& operator=(const scoped_native_wait&) = delete;
};

inline bool event_wait_enabled(std::uint64_t timeout_ns, bool event_stream)
{
	// A sub-200 us timeout cannot absorb the periodic event stream's wake bound.
	return waiter_enabled() && enabled() && event_stream && timeout_ns >= 200'000;
}

struct topology
{
	std::uint64_t lower = 0;
	std::uint64_t upper = 0;

	explicit operator bool() const { return lower && upper; }

	// Recognize two near-performance tiers, not a conventional big/LITTLE machine.
	// Missing/third-tier capacity data or a restricted cpuset falls back to OS policy.
	static topology detect(const std::array<std::uint32_t, 64>& capacity, std::uint64_t allowed)
	{
		std::uint32_t low = UINT32_MAX, high = 0;
		for (unsigned i = 0; i != 64; ++i)
		{
			if (!(allowed & (std::uint64_t{1} << i))) continue;
			if (!capacity[i]) return {};
			if (capacity[i] < low) low = capacity[i];
			if (capacity[i] > high) high = capacity[i];
		}
		if (!high || low == high || std::uint64_t{low} * 10 < std::uint64_t{high} * 7) return {};
		topology result;
		for (unsigned i = 0; i != 64; ++i)
		{
			if (!(allowed & (std::uint64_t{1} << i))) continue;
			if (capacity[i] == low) result.lower |= std::uint64_t{1} << i;
			else if (capacity[i] == high) result.upper |= std::uint64_t{1} << i;
			else return {};
		}
		if (std::popcount(result.lower) < 4 || std::popcount(result.upper) < 2) return {};
		return result;
	}
};
}
