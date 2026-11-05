#pragma once

#include <cstddef>
#include <cstdint>
#include <exception>
#include <thread>

// A context primitive only: not wired to PPU execution. See CELL_FIBER_AUDIT.md.
// Linux SysV x86-64 and Android/Linux AArch64; cooperative, non-migrating, no
// exceptions/unwind across the switch boundary. Sanitizers/hardware shadow stacks
// require separate ports. All access must be externally serialized.
namespace cell
{
class fiber;
extern "C" [[noreturn]] void armsx3_cell_fiber_start(fiber* self);

class fiber final
{
public:
    using entry_type = void (*)(fiber&, void*);
    explicit fiber(entry_type entry, void* argument = nullptr, std::size_t stack_size = 1024 * 1024);
    ~fiber();
    fiber(const fiber&) = delete;
    fiber& operator=(const fiber&) = delete;
    fiber(fiber&&) = delete;
    fiber& operator=(fiber&&) = delete;

    // Returns true if suspended, false if complete. Entry exceptions are rethrown
    // here, after returning to the caller stack. First resume binds the host.
    bool resume();
    void yield();
    bool finished() const noexcept { return m_state == state::finished; }
    const void* stack_begin() const noexcept { return m_stack; }
    std::size_t stack_size() const noexcept { return m_stack_size; }

private:
    struct alignas(16) context
    {
        std::uint64_t words[22]{};
    };
    enum class state { created, running, suspended, finished };
    friend void armsx3_cell_fiber_start(fiber* self);
    [[noreturn]] void run() noexcept;
    void initialize_context();

    context m_context{};
    context m_caller{};
    entry_type m_entry;
    void* m_argument;
    void* m_mapping = nullptr;
    std::size_t m_mapping_size = 0;
    void* m_stack = nullptr;
    std::size_t m_stack_size = 0;
    state m_state = state::created;
    std::thread::id m_owner{};
    std::exception_ptr m_exception{};
};
}
