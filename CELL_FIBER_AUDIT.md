# PPU fiber integration audit

Source base: `15eff3cc2` (`claude/release`). This document records source inspection,
not a claim that PPU execution has been converted or that a game has passed.
`Utilities/CellFiber.{h,cpp}` is an isolated context primitive, deliberately absent
from emulator CMake/runtime until the topology/wait game checkpoint passes.

## Existing execution and blocking path

The public object is `named_thread<ppu_thread>`, not just `ppu_thread`.
`Utilities/Thread.h` forwarding constructors call `thread_base::start()`; their
entry path waits for launch retention, initializes a native thread, invokes
`cpu_thread::operator()`, finalizes and calls platform thread exit. Native handle,
Linux TID, task queue, `m_sync`, joins, destruction and errors all assume a pthread.

`cpu_thread::operator()` (`rpcs3/Emu/CPU/CPUThread.cpp`) installs current CPU TLS,
registers the CPU with suspend-all, and owns a **thread-local cleanup object**.
It runs `ppu_thread::cpu_task()` until stopped. That function drains the PPU command
queue; `fast_call()` enters `exec_task()` and the PPU JIT gateway. On AArch64 the
gateway saves the native SP and registers in **per-PPU `hv_ctx`**, reserves an 8 KiB
GHC scratch area, and escapes by restoring the saved native context. Each fiber
therefore needs a persistent native stack; calling `exec_task()` afresh on another
PPU without retaining the old stack loses HLE callbacks and host continuations.

`lv2_obj::sleep()` in `rpcs3/Emu/Cell/lv2/lv2.cpp` is **not the host blocking call**.
It removes/updates runnable queue entries, sets flags and deadlines, runs
`schedule_all()`, flushes pending notifications, and returns. Blocking occurs later
in `cpu_thread::check_state()` -> `ppu_thread::cpu_wait()` -> `state.wait()`, or in
HLE code which waits directly. Switching from inside `sleep_unlocked()` or
`schedule_all()` would switch while the lv2 scheduler mutex is held.

## Required integration points

| Area | Exact files/functions | Required change/invariant |
| --- | --- | --- |
| Object backend | `Utilities/Thread.h`: `named_thread` constructors, `entry_point2`, assignment, destructor; `Utilities/Thread.cpp`: `start`, `initialize`, `finalize`, `join`, `notify`, `push`, native identity | Preserve `named_thread<ppu_thread>` identity for IDM/API callers, but add a fiber backend selected only after lazy `ARMSX3_CELL` configuration. Guest completion is a state transition, never `pthread_exit`/`pthread_join`. Host worker handles must never be destroyed through guest objects. |
| CPU activation | `rpcs3/Emu/CPU/CPUThread.cpp`: `operator()`, `check_state`, `cpu_counter::{add,remove}`, TLS cleanup | Replace one TLS cleanup `_this` with per-CPU lifetime cleanup. Save/restore current CPU, suspend slot/stamp, log callbacks and VM passive-lock state. Only the actually executing PPU may appear active in suspend-all. |
| Logical runnable queue | `rpcs3/Emu/Cell/lv2/lv2.cpp`: `awake_unlocked`, `sleep_unlocked`, `schedule_all`, `prepare_for_sleep`, `get_running_ppu`, `cleanup`; `sys_sync.h` | Reuse priority/order/deadlines and `g_pending` handshake. Dispatch only PPUs admitted by lv2, with an atomic claimed/running owner so two hosts cannot resume one fiber. Release `g_mutex` before switching. Wakeups must enqueue and signal the owning scheduler. |
| PPU stack boundaries | `rpcs3/Emu/Cell/PPUThread.cpp`: `cpu_task`, `cpu_wait`, `cmd_wait`, `exec_task`, `fast_call`, constructors, deserializer | Per-fiber guarded native stack; no raw native stack serialization. Resume at the same C++ continuation. JIT `hv_ctx` must refer to the live fiber stack. Entry exceptions caught on the same stack before handing results to scheduler. |
| State wakeups | `rpcs3/Emu/CPU/CPUThread.cpp`: `notify`, assignment, `add_remove_flags`; `rpcs3/Emu/System.cpp`: `RunPPU`, pause/resume/stop/savestate | Notifications currently use `state.notify_one` directly, not only `cpu_thread::notify`. A fiber-aware wait registry or explicit scheduler enqueue is needed on **every** notification path. Stop must wake queued, timed and atomic-waiting fibers. |
| Guest timeout wait | `lv2.cpp`: `wait_timeout`; `sys_timer.cpp`: `sys_timer_usleep`; `sys_event.cpp`: receive timing | Convert timeout waits to scheduler deadlines, retaining clock scaling, early signal vs timeout result, `again` and stop semantics. `usleep(0)` must yield another runnable fiber or park until a bounded deadline, not call kernel yield/sleep on a scheduler host. Move its polling history from TLS to per-PPU state. |
| Direct lv2 waits | `sys_mutex.cpp`, `sys_cond.cpp`, `sys_rwlock.cpp`, `sys_semaphore.cpp`, `sys_lwmutex.cpp`, `sys_lwcond.cpp`, `sys_event.cpp`, `sys_event_flag.cpp`, `sys_spu.cpp`, `sys_net.cpp`, `sys_process.cpp`, `sys_usbd.cpp`, `sys_interrupt.cpp` | These contain direct `ppu.state.wait`, group-counter waits, channel/command waits or `thread_ctrl::wait_on`. Convert each wait at a lock-free safe point or provide a fiber-aware atomic wait backend. Preserve old values and multi-address abort/task notifications. |
| Join/detach/exit | `rpcs3/Emu/Cell/lv2/sys_ppu_thread.cpp`: `_sys_ppu_thread_exit`, join/detach, `ppu_thread_cleaner` | A guest join must suspend its fiber, not block its scheduler host. Deferred deletion must retain the target stack until it has returned to host. Never destroy the running fiber. Zombie/guest-ID semantics remain unchanged. |
| Host waits | `Utilities/Thread.cpp`: `wait_for`, `wait_until`, `wait_for_accurate`; `Thread.h`: `wait_on_custom`; `rpcs3/util/atomic.cpp`: `atomic_wait_engine::wait` | New explicit fiber wait hook before platform blocking/registration, with fiber-owned predicates, deadlines and wait records. Existing TLS callback is not a sufficient interception point. Legacy/helper native threads retain current behavior. |
| VM lock/identity | `rpcs3/Emu/Memory/vm.cpp`: `passive_lock`, `temporary_unlock`, writer barrier and `native_tid_of` | Release passive ownership before suspension and reacquire through `check_state`; retain logical CPU association separately from current host TID. Writer boost/statistics must target current host only while this PPU is running, not a stale TID. |
| HLE callbacks | `rpcs3/Emu/Cell/Modules/sys_libc_.cpp`: qsort TLS comparator; `cellMsgDialog.cpp`, `cellOskDialog.cpp`, `sceNpTrophy.cpp`, other HLE waits | `g_tls_cmp` can be overwritten by another fiber during a guest comparator callback. Make it per-call/per-fiber; audit every callback that can re-enter guest execution. Native GUI/compile/audio helper waits must not become PPU waits accidentally. |
| Signals/debugger | `Utilities/Thread.cpp`: signal handler, `initialize`, `get_thread_stack`, emergency/silent exit; CPU debugger hooks | Each host keeps its own alternate signal stack. Signal handlers inspect the active fiber bounds for guard diagnostics and CPU identity. Do not context-switch from signal handlers. Fatal guest termination must not terminate a shared scheduler pthread. |
| Savestate | `PPUThread.cpp`: savable/save/serialize/deserializer, `fast_call`/`again`; `System.cpp`: pause/stop/load | Pause all fibers at the existing serializable guest boundary. C++ stacks cannot be serialized. If an HLE continuation has no replayable `again` state, reject the save through existing semantics rather than serializing stale native pointers. Rebuild native stacks on load. |

Line references useful for the original base: `CPUThread.cpp:34,37,503,652,691`,
`PPUThread.cpp:2931,3157,3631,3656`, `lv2.cpp:1725,1757,1882,2010,2265,2523,2644,2672`,
`Thread.cpp:3981,4107,4255,4276,4466,4486,5416`, `sys_ppu_thread.cpp:24,82,181`,
`sys_timer.cpp:456,502`, `sys_libc_.cpp:481`. Edits can move these lines; function
names above are the durable anchors.

## TLS separation

Keep true host data host-local: native TID cache, signal alternate stack, libc and
JNI/ART state, OS affinity, atomic semaphore cache, per-host scheduling loop.
Make logical CPU data per-fiber or explicitly activate/deactivate it: CPU current
pointer, thread_ctrl current logical object/error callback, CPU suspend slot/stamp,
VM passive-lock pointer, log context, guest poll timing, qsort callback state.
The lv2 notification vectors and RAII barrier must be **empty/flushed before a
switch**, or become per-fiber; never inherit another guest's postponed barrier.
Profiler wait/fault accounting needs an explicit policy separating host totals
from logical guest counters. The existing thread-local cleanup object cannot be
shared between multiple in-flight `operator()` invocations.

`atomic_wait_engine` has TLS one-time/general callbacks and cached condition IDs.
The wait callback runs after registration and sometimes after kernel wait. Merely
yielding there does not establish fiber ownership or wakeup routing. A new backend
must atomically install waiter -> recheck predicate -> suspend with a generation
counter; notification races before/after suspension must both enqueue exactly once.

## Scoped runtime plan after the phase-one game gate

1. Introduce a PPU-only backend for `thread_base`, keeping native backend unchanged.
   Retain the concrete `named_thread<ppu_thread>` type and queue semantics. Create
   two scheduler host threads, each pinned individually to a prime core. Bind each
   fiber to one host initially; never migrate it while native continuations exist.
   Dispatch only admitted lv2 PPUs, with priority ordering and an owner claim.
2. Add explicit activation/deactivation context APIs in CPUThread, Thread and vm.
   Factor logical cleanup from native-thread cleanup. At every switch, assert no
   lv2/IDM mutex or postponed notification barrier is held and release passive VM
   locks. Keep host TLS (especially bionic/ART TLS and x18) on its native host.
3. Hook PPU state/channel/command and thread_ctrl waits into scheduler-owned waiter
   records; convert the direct lv2 and HLE waits listed above. A shared native wait
   implementation must remain for SPUs/helpers. Unit stress wake-before-park,
   wake-after-park, duplicate wake, timeout, abort, and object-deletion races.
4. Add a scheduler deadline service for **guest lv2 timeouts**, not just created
   `lv2_timer` event objects. Existing `g_waiting` is advanced by `schedule_all` and
   native blocked callers currently supply their own timeout. If every fiber is
   parked, the new service must still advance deadlines and signal the hosts.
   Timer preemption sets existing preempt/yield state flags; PPU JIT state-check
   safe points reach `check_state` and switch cooperatively. Never swap from a
   timer signal handler or while GHC scratch state is live without its own stack.
5. Handle host locks and blocking I/O explicitly. A fiber cannot park owning a
   pthread mutex if another fiber on the same host may need it. Audit lock scopes;
   release locks before a wait or use fiber-aware lock wait queues. `sys_fs` has
   synchronous host file operations, and HLE may block inside libraries. Route
   long blocking operations to bounded worker tasks with copied/retained buffers,
   or retain a dedicated-native fallback for unsupported guest entry paths with
   an explicit diagnostic. Do not silently claim two-host execution while every
   blocking syscall gets a replacement guest pthread.
6. Integrate stop/join/deletion and savestate quiescence before enabling runtime.
   Cancellation is cooperative: request exit, wake the fiber, let its stack unwind
   to the scheduler, then free it. A destructor cannot cancel a suspended C++ stack.
   Verify nested guest callbacks, interrupt PPUs, process exit, pause, stop during
   I/O/timed waits and repeated emulator restarts.

## Primitive and checkpoint status

`CellFiber` provides mmap-backed read/write stacks with PROT_NONE guards on both
sides; saves ABI call-preserved integer/vector and FP control state; binds to the
host on first resume; catches entry exceptions on the fiber stack and rethrows on
the caller stack; rejects recursive resume, migration and external yield. It does
**not** provide scheduling, PPU TLS, cancellation, wait interception or savestates.
Destroying a suspended fiber terminates intentionally instead of abandoning RAII.
ASan/TSan, ShadowCallStack and hardware shadow-stack builds fail compilation
until their switch hooks are implemented. The primitive is not internally
synchronized; callers must serialize access. Android bionic's NDK headers provide signal `ucontext_t`, but no
`getcontext`/`makecontext`/`swapcontext` API, hence the dedicated assembly primitive.

Verified on the Arch host with clang++ and g++: 20,000 yields/resumes, independent
stack locals and FP rounding, RAII completion, exception forwarding, both guard
faults and misuse rejection. NDK r29/API35 AArch64 compilation and linking passed;
the resulting binary has not been executed on Android.

Run `tools/cell/tests/run_fiber.sh` on Linux. This is a focused primitive fixture,
not the PPU/game correctness gate. AArch64 cross-compilation proves syntax/linking
only; it does not prove execution or Android integration. Until topology/wait,
fiber lifecycle/TLS/wait races, both games, savestate and exit checkpoints pass,
the local-store and code-layout phases must not be enabled as a completed rebuild.
