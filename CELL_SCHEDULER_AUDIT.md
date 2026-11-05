# PPU fiber scheduler foundation

Source base: current `codex/cell` tip. This document records an isolated
two-host cooperative scheduler built on `CellFiber`. It is **not** wired into
`named_thread`, `cpu_thread`, lv2, or Android CMake. See `CELL_FIBER_AUDIT.md`
for the full runtime integration matrix that remains before enabling fibers in
production.

## What landed

- `Utilities/CellScheduler.{h,cpp}`: two (configurable) host worker threads,
  per-host ready queues, atomic owner tickets so two hosts cannot resume one
  fiber, non-migrating bind on first resume (inherited from `CellFiber`),
  cooperative `fiber_task::yield()` that parks back to the owning host, and
  stop/join that drains hosts without destroying suspended stacks (destructor
  still terminates if an admitted fiber is unfinished — cancellation must unwind).
- Host fixture `tools/cell/tests/run_scheduler.sh`: simple completion across
  both hosts, 20-yield round-trips on each host, and 64 one-shot tasks.
  Verified on the Arch host with the system C++ toolchain.

## Deliberately not done here

- No `thread_base` / `named_thread<ppu_thread>` backend selection.
- No CPU TLS activation/deactivation, VM passive-lock release, lv2 mutex
  assertions, or wait-registry interception.
- No scheduler deadline service for guest timeouts.
- No savestate/join/exit integration.
- Not added to `rpcs3/Emu/CMakeLists.txt`; production APK is unchanged.

## Next integration order (after phase-one game gate)

Follow `CELL_FIBER_AUDIT.md` scoped runtime plan steps 1–6. Do not enable the
local-store or codegen phases as a completed rebuild until fiber lifecycle,
both games, savestate and exit checkpoints pass.
