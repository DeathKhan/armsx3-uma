// Included by SPUThread.cpp: debug-only current-runtime replay implementation.
#include "tools/cell/spu-replay/runtime.hpp"
#include "tools/cell/spu-replay/segment.hpp"
#include "tools/cell/spu-replay/state_compare.hpp"
#include "Utilities/Cell.h"
#include "Emu/Cell/SPUInterpreter.h"
#include <atomic>
#include <chrono>
#include <mutex>

namespace cell_replay
{
namespace
{
enum class stop_kind { none, boundary, interrupted, timeout, fallback, budget };
struct run_context
{
    bool active = false; // accessed only by the replay SPU host thread
    stop_kind stopped = stop_kind::none;
    u32 opcode = 0;
    std::atomic<bool> abort{false};
};
thread_local run_context* current_run = nullptr;

struct watchdog
{
    spu_thread& spu;
    run_context& context;
    std::mutex mutex;
    std::chrono::steady_clock::time_point deadline{};
    bool owns_pending = false;
    std::atomic<bool> quit{false};
    std::thread worker;

    watchdog(spu_thread& s, run_context& c) : spu(s), context(c), worker([this]
    {
        while (!quit.load(std::memory_order_relaxed))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::lock_guard lock(mutex);
            if (deadline != std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now() >= deadline)
            {
                deadline = {};
                context.abort.store(true, std::memory_order_release);
                const auto old = spu.state.fetch_op([](bs_t<cpu_flag>& flags) { flags += cpu_flag::pending; });
                owns_pending = !!(cpu_flag::pending - old);
            }
        }
    }) {}

    void begin()
    {
        std::lock_guard lock(mutex);
        context.abort.store(false, std::memory_order_release);
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    }
    void end()
    {
        std::lock_guard lock(mutex);
        deadline = {};
        if (std::exchange(owns_pending, false)) spu.state -= cpu_flag::pending;
    }
    ~watchdog()
    {
        quit.store(true, std::memory_order_relaxed);
        worker.join();
        end();
    }
};

const char* stop_name(stop_kind kind)
{
    switch (kind)
    {
    case stop_kind::boundary: return "boundary";
    case stop_kind::interrupted: return "interrupted";
    case stop_kind::timeout: return "timeout";
    case stop_kind::fallback: return "interpreter_fallback";
    case stop_kind::budget: return "instruction_budget";
    default: return "no_boundary";
    }
}

void import_initial(spu_thread& spu, const segment& capture)
{
    static_assert(sizeof(spu.gpr) == register_bytes && sizeof(spu.fpscr) == fpscr_bytes);
    // Canonical import adapter. A future LS conversion must change this adapter,
    // not the capture format or the expected final-state images.
    std::memcpy(spu.ls, capture.ls_start.data(), ls_size);
    std::memcpy(spu.gpr.data(), capture.gpr_start.data(), register_bytes);
    std::memcpy(&spu.fpscr, capture.fpscr_start.data(), fpscr_bytes);
    spu.pc = capture.pc_start;
    spu.interrupts_enabled = false;
    spu.interp_fallback = false;
    spu.allow_interrupts_in_cpu_work = false;
}

state_comparison compare_final(const spu_thread& spu, const segment& capture)
{
    // Canonical export adapter. Native-order LS must export guest bytes here.
    return compare_state({
        {reinterpret_cast<const byte*>(spu.gpr.data()), register_bytes},
        {reinterpret_cast<const byte*>(&spu.fpscr), fpscr_bytes},
        {spu.ls, ls_size}, spu.pc}, capture);
}
}

bool enabled()
{
    if (!cell::environment_ready().load()) return false;
    static const bool on = []
    {
        const char* value = std::getenv("ARMSX3_CELL_REPLAY");
        return value && std::string_view(value) == "1";
    }();
    return on;
}

void intercept_boundary(spu_thread* spu, std::uint32_t opcode)
{
    if (current_run && current_run->active)
    {
        current_run->opcode = opcode;
        current_run->stopped = current_run->abort.load(std::memory_order_acquire) ? stop_kind::timeout : stop_kind::boundary;
        current_run->active = false;
        // No RAII objects may exist between this helper and the live gateway.
        spu_runtime::g_escape(spu);
    }
}
void reject_fallback(spu_thread& spu)
{
    if (current_run && current_run->active)
    {
        current_run->stopped = stop_kind::fallback;
        current_run->active = false;
        spu_runtime::g_escape(&spu);
    }
}
void on_state_check(spu_thread& spu)
{
    if (current_run && current_run->active && spu.state)
    {
        current_run->stopped = current_run->abort.load(std::memory_order_acquire) ? stop_kind::timeout : stop_kind::interrupted;
        current_run->active = false;
        spu_runtime::g_escape(&spu);
    }
}

bool run_isolated(spu_thread& spu)
{
    if (!enabled()) return false;
    // Use only System.cpp's standalone SPU ELF loader. Never borrow a game's SPU,
    // nor run via suspend_all: that callback may execute under another CPU's TLS.
    const u32 ppus = idm::select<named_thread<ppu_thread>>([](u32, ppu_thread&) {});
    const u32 spus = idm::select<named_thread<spu_thread>>([](u32, spu_thread&) {});
    if (spu.group || spu.lv2_id || *spu.spu_tname.load() != "test_spu" || ppus || spus != 1 ||
        spu.mfc_size || spu.mfc_queued_ops_timeout || spu.raddr ||
        g_cfg.core.spu_decoder != spu_decoder_type::llvm || !spu.jit || !g_cfg.core.spu_verification)
    {
        spu_log.error("ARMSX3_CELL_REPLAY: REFUSED context (standalone test_spu only, PPU=%u SPU=%u, LLVM+verification required)", ppus, spus);
        spu.state += cpu_flag::stop;
        return true;
    }

    const char* requested_dir = std::getenv("ARMSX3_CELL_REPLAY_DIR");
    std::string dir = requested_dir && *requested_dir ? requested_dir : fs::get_cache_dir() + "cell-spu-replay/";
    if (!dir.ends_with('/')) dir += '/';
    std::vector<std::string> names;
    for (auto&& entry : fs::dir(dir))
    {
        if (!entry.is_directory && entry.name.starts_with("seg-") && entry.name.ends_with(".bin")) names.push_back(entry.name);
    }
    std::sort(names.begin(), names.end());
    fs::file output(dir + "result.txt", fs::rewrite);
    if (!output || names.empty() || names.size() > 4096)
    {
        spu_log.error("ARMSX3_CELL_REPLAY: no bounded capture set/output in %s", dir);
        spu.state += cpu_flag::stop;
        return true;
    }
    const auto log = [&](const std::string& line) { output.write(line); output.sync(); };
    log(fmt::format("# current LLVM boundary replay: captures=%u xfloat=%s; LLVM instruction count UNMEASURED; no timing claim\n",
                    names.size(), g_cfg.core.spu_xfloat_accuracy));
    const auto saved_gpr = spu.gpr;
    const auto saved_fpscr = spu.fpscr;
    const u32 saved_pc = spu.pc;
    const bool saved_irq = spu.interrupts_enabled;
    const bool saved_fallback = spu.interp_fallback;
    const u32 saved_fallback_begin = spu.interp_fallback_begin;
    const u32 saved_fallback_end = spu.interp_fallback_end;
    const bool saved_allow_interrupts = spu.allow_interrupts_in_cpu_work;
    const bool saved_unsavable = spu.unsavable;
    const std::vector<u8> saved_ls(spu.ls, spu.ls + ls_size);
    run_context context;
    watchdog timer(spu, context);
    current_run = &context;
    u32 parsed = 0, malformed = 0, interpreter_exact = 0, llvm_exact = 0, stopped = 0;
    const auto& interpreter = g_fxo->get<spu_interpreter_rt>();

    for (const auto& name : names)
    {
        segment capture;
        std::string error;
        if (!read(dir + name, capture, error))
        {
            malformed++;
            log(fmt::format("%s malformed=%s\n", name, error));
            continue;
        }
        if (capture.version != 2)
        {
            malformed++;
            log(fmt::format("%s unsupported-replay-version=%u (format-only support for v1)\n", name, capture.version));
            continue;
        }
        parsed++;
        import_initial(spu, capture);
        u64 count = 0;
        stop_kind interpreter_stop = stop_kind::none;
        while (true)
        {
            if (spu.state) { interpreter_stop = stop_kind::interrupted; break; }
            if (spu.pc >= ls_size || (spu.pc & 3)) { interpreter_stop = stop_kind::budget; break; }
            const u32 opcode = big32(spu.ls + spu.pc);
            if (boundary(opcode)) { interpreter_stop = stop_kind::boundary; break; }
            if (count >= capture.instructions) { interpreter_stop = stop_kind::budget; break; }
            if (interpreter.decode(opcode)(spu, {opcode})) spu.pc += 4;
            count++;
        }
        const auto interp = compare_final(spu, capture);
        const bool interp_ok = interpreter_stop == stop_kind::boundary && count == capture.instructions && interp.exact();
        interpreter_exact += interp_ok;
        if (interpreter_stop == stop_kind::interrupted)
        {
            stopped++;
            log(fmt::format("%s interpreter=interrupted llvm=NOT_RUN\n", name));
            break;
        }

        import_initial(spu, capture);
        context.stopped = stop_kind::none;
        context.opcode = 0;
        context.active = true;
        timer.begin();
        while (context.active)
        {
            if (spu.state)
            {
                context.stopped = context.abort.load(std::memory_order_acquire) ? stop_kind::timeout : stop_kind::interrupted;
                context.active = false;
                break;
            }
            if (spu.pc >= ls_size || (spu.pc & 3))
            {
                context.stopped = stop_kind::budget;
                context.active = false;
                break;
            }
            const u32 opcode = big32(spu.ls + spu.pc);
            if (boundary(opcode))
            {
                context.opcode = opcode;
                context.stopped = stop_kind::boundary;
                context.active = false;
                break;
            }
            spu_runtime::g_gateway(spu, spu.ls, nullptr);
        }
        timer.end();
        const auto llvm = compare_final(spu, capture);
        const bool llvm_ok = context.stopped == stop_kind::boundary && context.opcode == capture.end_opcode && llvm.exact();
        llvm_exact += llvm_ok;
        log(fmt::format("%s interpreter=%s count=%u/%u reason=%s regs=%u fpscr=%u ls=%u pc=%u opcode=%u LLVM=%s reason=%s regs=%u fpscr=%u ls=%u pc=%u opcode=%u LLVM_count=UNMEASURED\n",
            name, interp_ok ? "exact" : "FAIL", count, capture.instructions, stop_name(interpreter_stop),
            interp.bad_registers, interp.bad_fpscr, interp.bad_ls_quadwords, interp.bad_pc, interp.bad_opcode,
            llvm_ok ? "exact-final-state" : "FAIL", stop_name(context.stopped), llvm.bad_registers, llvm.bad_fpscr,
            llvm.bad_ls_quadwords, llvm.bad_pc, llvm.bad_opcode));
        if (parsed % 50 == 0) spu_log.notice("ARMSX3_CELL_REPLAY: %u/%u parsed; interpreter exact=%u LLVM final-state exact=%u", parsed, names.size(), interpreter_exact, llvm_exact);
        if (context.stopped == stop_kind::interrupted || context.stopped == stop_kind::timeout)
        {
            stopped++;
            break;
        }
    }
    timer.end();
    current_run = nullptr;
    std::memcpy(spu.ls, saved_ls.data(), ls_size);
    spu.gpr = saved_gpr;
    spu.fpscr = saved_fpscr;
    spu.pc = saved_pc;
    spu.interrupts_enabled = saved_irq;
    spu.interp_fallback = saved_fallback;
    spu.interp_fallback_begin = saved_fallback_begin;
    spu.interp_fallback_end = saved_fallback_end;
    spu.allow_interrupts_in_cpu_work = saved_allow_interrupts;
    spu.unsavable = saved_unsavable;
    const bool complete = parsed + malformed == names.size() && !stopped;
    const bool passed = complete && !malformed && interpreter_exact == names.size() && llvm_exact == names.size();
    log(fmt::format("# complete=%u passed=%u input=%u parsed=%u malformed=%u stopped=%u interpreter_exact=%u LLVM_exact_final_state=%u LLVM_instruction_count=UNMEASURED\n",
                    complete, passed, names.size(), parsed, malformed, stopped, interpreter_exact, llvm_exact));
    spu_log.notice("ARMSX3_CELL_REPLAY: complete=%u passed=%u input=%u interpreter exact=%u LLVM final-state exact=%u; %sresult.txt",
                    complete, passed, names.size(), interpreter_exact, llvm_exact, dir);
    // The harness ELF is inert and stays parked. Never resume a game after a test.
    spu.state += cpu_flag::stop;
    return true;
}
} // namespace cell_replay
