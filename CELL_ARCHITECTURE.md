# Cell-on-Oryon architecture and checkpoint record

## Status

This is an in-progress architectural rebuild based on `15eff3cc2`, isolated on
`codex/cell`. It is not yet the complete Cell-on-Oryon build and has no measured
performance improvement. The acceptance comparison is the entire integrated build
with `ARMSX3_CELL=1` against the current release; subsystem checkpoints establish
correctness, not independent performance justifications.

| Checkpoint | Source state | Required game gate |
| --- | --- | --- |
| Topology and registered waits | Implemented in APK `phase1-48770e496` (sha256 `9c51e96e…`); installed on Odin 2026-11-05 | LBP reached playable Sackboy stage with `ARMSX3_CELL=1`; topology `SPU/helpers=0x3f PPU/RSX=0xc0`; uclamp unavailable (errno 1, non-fatal). Walk bench and Ratchet still pending in this session. |
| Two-host PPU fibers | Guarded `CellFiber` + isolated `CellScheduler` host fixtures pass on Arch; not in Android CMake/runtime | Not started |
| Host-order managed-SPU local store | Complete accessor audit and architecture-contract tests; conversion pending | Not started |
| PPU optimization and huge-page layout | Design audit pending; production path unchanged | Not started |
| Integration | Pending | 1,200 current-LLVM replay vectors, game/savestate/exit tests and 10 boot loops |

Do not ship the partial checkpoint as the requested architectural rebuild. Do not
advance an integrated phase after a failed correctness gate.

## Master switch and startup ordering

`Utilities/Cell.h` reads exactly `ARMSX3_CELL=1`, using function-local lazy state.
On Android, `cell::initialize()` first publishes that `driver_env.txt` has been
processed. Earlier housekeeping thread creation cannot cache a missing variable.
The Android bridge logs both the applied environment entries and the master state.
Changing the file requires a fresh emulator process, as with existing driver knobs.

With the switch absent, zero, or any other value, the existing affinity and wait
paths remain selected. The final integrated PPU/SPU object cache keys must identify
all changed code-generation and representation ABIs; those later phases are not
implemented yet. No SPU layout change or SPU object-cache wipe is needed for this
first checkpoint.

## Topology and thread placement

Detect capacity tiers from sysfs, intersecting with the actual process affinity
mask. Recognize exactly two tiers with at least four lower-tier and two upper-tier
CPUs, with the lower capacity at least 70% of the upper. Missing capacity data,
third tiers and inadequate restricted CPU sets keep generic placement.

The observed Odin topology is CPUs0-5 at792 and CPUs6-7 at1024. Live SPU host
threads lease separate lower-tier CPUs, releasing leases on thread exit. Overflow
threads share the least-occupied lower-tier CPU. PPU and RSX host threads use the
upper tier. Named helper threads use the lower tier and request uclamp_max256;
CPU/RSX promotion restores1024. These requests preserve scheduler policy and nice
values and report failures rather than assuming kernel/SELinux support. Platform-
owned realtime audio callbacks are not reconfigured inside the callback.

Placement applies even with the existing scheduler mode set to OS. The two PPU
scheduler hosts do not exist yet: the first checkpoint still has one host thread
per PPU.

## Registered wait protocol

Keep `atomic_wait_engine` registration and predicate comparison. Each registered
condition has an event/native phase. Eligible PPU/SPU waits with EVTSTRM use a
bounded WFE phase, capped at3ms and also bounded by the requested timeout. A signal
publishes its condition before SEV. Before entering futex, the waiter publishes
native mode and rechecks both the condition and predicate; FUTEX_WAIT retains its
expected-value check. Finite timeouts account for elapsed event waiting.

RSX pacing, timer/usleep and helper waits retain their native sleep behavior.
Scoped suppression preserves thread_ctrl explicit sleeps and all lv2 timeout
sleeps; logical waits shorter than200us also remain native. Existing bounded
SPU adaptive probes before registration have not been converted.
This checkpoint does not replace host blocking with fiber switching. Review the
actual channel and scheduler callers before claiming complete wait coverage.
Host race tests cover the event-to-futex handoff; they do not prove the Odin's
WFE behavior, device watts or game liveness.

## PPU fibers

See `CELL_FIBER_AUDIT.md` for the exact integration matrix. The existing lv2 queue
marks guest state but does not own host thread creation or every blocking point.
The integrated design needs a fiber-aware named_thread lifecycle, two pinned host
workers, guarded per-PPU stacks, explicit fiber-local activation state and timer
service for parked deadlines. Native TLS includes CPU suspension state, VM locks,
notification batches, wait callback state and guest libc callback state. These
must be separated or safely restored at every cooperative switch.

Never switch asynchronously from a signal handler or while carrying host locks,
VM range locks or another fiber's registered waiter. Timers request preemption;
JIT/HLE safe points return to the scheduler. The isolated stack/context primitive
is not a substitute for these runtime changes.

## Local-store representation and verification

See `CELL_LS_AUDIT.md` for the36-family accessor/writer inventory. LQ/STQ access
quadwords, but host consumers also access instructions, smaller scalars, MFC list
elements and big-endian SPURS structures. All must use representation-aware access.
DMA must account for all legacy/native source/destination combinations, partial
transfers and wrap. Raw-SPU title fallback needs a policy for late raw-SPU creation.

A dirty-page bit cannot be cleared after checking only the entering function:
other functions on that page may still be stale. Dirty processing must invalidate
all affected callable entries/branch edges, or retain per-page/per-block generation
validation. Every writer must publish dirtiness without losing concurrent writes.
The normal VM mapping and all five JIT wrap aliases must obey the protection policy;
Android's fault-range filter must recognize protected LS aliases. Anonymous huge-
page remapping cannot silently replace one alias of the current shared backing.

The archived spu-diff compiler tests an abandoned custom AArch64 backend. The
archive also contains in-emulator replay, but that borrows live SPUs and plants a
STOP opcode into LS. Neither is sufficient evidence for the current production
LLVM backend. A strict parser validated all1,200 real captures (allversion2),
including235,015 recorded instructions; an isolated current-LLVM replay adapter
remains required. Format validation is not execution validation.

## Validation and measurements

Initial device observations: installed package `com.armsx3.uma`, version
`1.6.1-uma.2`/109, core stamp`15eff3cc`, APK SHA256
`bd3878a36b0a12010d747756764d37b972040258d7e97d5f884e3085918d59c5`.
The historical `l307-final` uses another APK (`a5acb5d102890d51…`); it is context,
not a same-day control for this candidate.

| Measure | Historical l307-final | Same-day release | Combined CELL build |
| --- | ---: | ---: | ---: |
| Mcycles/frame |169|Pending|Pending|
| Busy CPU cores |1.91|Pending|Pending|
| Late frames |0|Pending|Pending|
| Per-thread syscalls/s |Not recorded here|Pending|Pending|
| Unplugged watts |Not recorded here|Pending|Pending|
| Hottest cpu/cpuss temperature |Not recorded here|Pending|Pending|

Run the release and combined candidate at least twice each, same scene and day.
Use `/data/local/tmp/bench/bench_ax3.sh com.armsx3.uma <run> 90`, inspect screenshots
for actual scene/progress, and analyze frames with the existing analyze.py.
Report per-thread raw syscall counts/rates and task-clock/cycles durations. Record
battery power only when actually unplugged and discharging; charging current is
not whole-device consumption. Use cpu-/cpuss- thermal zones only. Do not launch or
install while another game remains open. The initial device was dozing but still
had Eden guest CPU/GPU threads, so device mutation has been deferred.

## Build and evidence

Use the supplied release-signing build pipeline. Build outputs remain under the
worktree; dated logs and scratch are in
`~/AgentOutput/2026-11-05/codex-cell/` on their respective hosts. Dependencies use
nested detached worktrees sharing existing Git object stores, not second clones.
Additional non-submodule build inputs reuse existing librashader07ac790c and
libadrenotools8fae8ce2 checkouts as detached worktrees (linkernsbypassaa397589).
The signing configuration is a symlink to the existing private configuration; no
key or password was copied into source or printed. No shared branch, production
service, user save, credential or signing key has been modified. The repository has GitHub remotes and no Forgejo remote.

Initial free space: Arch1.2TiB; Mac96GiB. Final cleanup/free-space figures will be
recorded when the task is complete. Keep the active branch/worktree and necessary
build cache until integrated validation is finished; remove test target artifacts
when no longer active. No push or merge has been performed.
