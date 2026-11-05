#include "CellFiber.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <sys/mman.h>
#include <unistd.h>

#if !defined(__linux__) || (!defined(__aarch64__) && !defined(__x86_64__))
#error "CellFiber requires a Linux/Android AArch64 or Linux SysV x86-64 port"
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#error "CellFiber requires sanitizer fiber-switch hooks before an instrumented build can use it"
#endif
#if defined(__CET__) || defined(__ARM_FEATURE_GCS)
#error "CellFiber requires dedicated hardware shadow-stack support"
#endif
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(shadow_call_stack)
#error "CellFiber requires explicit sanitizer/SCS stack-switch support"
#endif
#endif

extern "C" void armsx3_cell_fiber_swap(void* from, const void* to);
extern "C" void armsx3_cell_fiber_bootstrap();

#if defined(__aarch64__)
// AAPCS64 call-preserved registers. x18 stays the host's platform register;
// migration and ShadowCallStack builds are deliberately unsupported.
asm(R"(
.text
.p2align 4
.global armsx3_cell_fiber_swap
.hidden armsx3_cell_fiber_swap
.type armsx3_cell_fiber_swap,%function
armsx3_cell_fiber_swap:
    hint #34
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x2, sp
    str x2, [x0, #96]
    mrs x2, fpcr
    str w2, [x0, #104]
    mrs x2, fpsr
    str w2, [x0, #108]
    stp d8, d9, [x0, #112]
    stp d10, d11, [x0, #128]
    stp d12, d13, [x0, #144]
    stp d14, d15, [x0, #160]
    ldp x19, x20, [x1, #0]
    ldp x21, x22, [x1, #16]
    ldp x23, x24, [x1, #32]
    ldp x25, x26, [x1, #48]
    ldp x27, x28, [x1, #64]
    ldp x29, x30, [x1, #80]
    ldr x2, [x1, #96]
    mov sp, x2
    ldr w2, [x1, #104]
    msr fpcr, x2
    ldr w2, [x1, #108]
    msr fpsr, x2
    ldp d8, d9, [x1, #112]
    ldp d10, d11, [x1, #128]
    ldp d12, d13, [x1, #144]
    ldp d14, d15, [x1, #160]
    ret
.size armsx3_cell_fiber_swap, .-armsx3_cell_fiber_swap
.p2align 4
.global armsx3_cell_fiber_bootstrap
.hidden armsx3_cell_fiber_bootstrap
.type armsx3_cell_fiber_bootstrap,%function
armsx3_cell_fiber_bootstrap:
    hint #34
    mov x0, x19
    bl armsx3_cell_fiber_start
    brk #0
.size armsx3_cell_fiber_bootstrap, .-armsx3_cell_fiber_bootstrap
)");
#else
asm(R"(
.text
.p2align 4
.global armsx3_cell_fiber_swap
.hidden armsx3_cell_fiber_swap
.type armsx3_cell_fiber_swap,@function
armsx3_cell_fiber_swap:
    movq %rbx, 0(%rdi)
    movq %rbp, 8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rax
    movq %rax, 48(%rdi)
    movq (%rsp), %rax
    movq %rax, 56(%rdi)
    stmxcsr 64(%rdi)
    fnstcw 68(%rdi)
    movq 0(%rsi), %rbx
    movq 8(%rsi), %rbp
    movq 16(%rsi), %r12
    movq 24(%rsi), %r13
    movq 32(%rsi), %r14
    movq 40(%rsi), %r15
    ldmxcsr 64(%rsi)
    fldcw 68(%rsi)
    movq 48(%rsi), %rsp
    jmp *56(%rsi)
.size armsx3_cell_fiber_swap, .-armsx3_cell_fiber_swap
.p2align 4
.global armsx3_cell_fiber_bootstrap
.hidden armsx3_cell_fiber_bootstrap
.type armsx3_cell_fiber_bootstrap,@function
armsx3_cell_fiber_bootstrap:
    subq $8, %rsp
    movq %r12, %rdi
    call armsx3_cell_fiber_start
    ud2
.size armsx3_cell_fiber_bootstrap, .-armsx3_cell_fiber_bootstrap
)");
#endif

namespace cell
{
fiber::fiber(entry_type entry, void* argument, std::size_t requested)
    : m_entry(entry), m_argument(argument)
{
    if (!entry || requested < 64 * 1024 || requested > 64 * 1024 * 1024)
        throw std::invalid_argument("Cell fiber: invalid entry or stack size");
    const long page_query = ::sysconf(_SC_PAGESIZE);
    if (page_query <= 0)
        throw std::runtime_error("Cell fiber: cannot query page size");
    const auto page = static_cast<std::size_t>(page_query);
    m_stack_size = (requested + page - 1) / page * page;
    m_mapping_size = m_stack_size + page * 2;
    m_mapping = ::mmap(nullptr, m_mapping_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m_mapping == MAP_FAILED)
    {
        m_mapping = nullptr;
        throw std::system_error(errno, std::generic_category(), "Cell fiber mmap");
    }
    m_stack = static_cast<char*>(m_mapping) + page;
    if (::mprotect(m_stack, m_stack_size, PROT_READ | PROT_WRITE) != 0)
    {
        const int error = errno;
        ::munmap(m_mapping, m_mapping_size);
        m_mapping = nullptr;
        throw std::system_error(error, std::generic_category(), "Cell fiber mprotect");
    }
}

fiber::~fiber()
{
    // Freeing a suspended C++ stack would abandon destructors/owned resources.
    // Cancellation belongs to the eventual PPU adapter and must unwind normally.
    if (m_state == state::running || m_state == state::suspended)
        std::terminate();
    if (m_mapping)
        ::munmap(m_mapping, m_mapping_size);
}

void fiber::initialize_context()
{
    static_assert(sizeof(context) == 176);
    auto top = reinterpret_cast<std::uintptr_t>(m_stack) + m_stack_size;
    top &= ~std::uintptr_t{15};
#if defined(__aarch64__)
    m_context.words[0] = reinterpret_cast<std::uintptr_t>(this); // x19
    m_context.words[11] = reinterpret_cast<std::uintptr_t>(&armsx3_cell_fiber_bootstrap); // x30
    m_context.words[12] = top;
    std::uint64_t fpcr, fpsr;
    asm volatile("mrs %0, fpcr" : "=r"(fpcr));
    asm volatile("mrs %0, fpsr" : "=r"(fpsr));
    m_context.words[13] = (fpsr << 32) | static_cast<std::uint32_t>(fpcr);
#else
    m_context.words[2] = reinterpret_cast<std::uintptr_t>(this); // r12
    top -= sizeof(std::uintptr_t); // SysV function entry: rsp % 16 == 8
    *reinterpret_cast<std::uintptr_t*>(top) = 0;
    m_context.words[6] = top;
    m_context.words[7] = reinterpret_cast<std::uintptr_t>(&armsx3_cell_fiber_bootstrap);
    std::uint32_t mxcsr;
    std::uint16_t fpcw;
    asm volatile("stmxcsr %0" : "=m"(mxcsr));
    asm volatile("fnstcw %0" : "=m"(fpcw));
    m_context.words[8] = (std::uint64_t{fpcw} << 32) | mxcsr;
#endif
}

bool fiber::resume()
{
    const auto host = std::this_thread::get_id();
    if (m_owner != std::thread::id{} && host != m_owner)
        throw std::logic_error("Cell fiber: migration is unsupported");
    if (m_state == state::running)
        throw std::logic_error("Cell fiber: recursive resume");
    if (m_state == state::finished)
        return false;
    if (m_state == state::created)
    {
        m_owner = host;
        initialize_context();
    }
    m_state = state::running;
    armsx3_cell_fiber_swap(&m_caller, &m_context);
    if (m_exception)
    {
        auto error = std::exchange(m_exception, {});
        std::rethrow_exception(error);
    }
    return m_state != state::finished;
}

void fiber::yield()
{
    if (m_owner != std::this_thread::get_id() || m_state != state::running)
        throw std::logic_error("Cell fiber: yield outside its running host");
    m_state = state::suspended;
    armsx3_cell_fiber_swap(&m_context, &m_caller);
}

[[noreturn]] void fiber::run() noexcept
{
    try { m_entry(*this, m_argument); }
    catch (...) { m_exception = std::current_exception(); }
    m_state = state::finished;
    armsx3_cell_fiber_swap(&m_context, &m_caller);
    std::abort();
}

extern "C" [[noreturn]] void armsx3_cell_fiber_start(fiber* self)
{
    self->run();
}
}
