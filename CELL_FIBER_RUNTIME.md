# PPU fiber runtime status

## What is wired (commit tip)

- `Utilities/CellFiber.{h,cpp}` and `CellScheduler.{h,cpp}` are linked into `rpcs3_emu`.
- Host fixtures: `tools/cell/tests/run_fiber.sh`, `run_scheduler.sh`, `run_ppu_dispatch.sh`.
- `CellPpuRuntime` starts **two** scheduler hosts when `ARMSX3_CELL=1`, pinned to the
  upper topology mask when detect succeeds (lazy `Cpus_allowed_list` + capacity fallback).
- `ARMSX3_CELL_FIBER=1` enables the PPU `named_thread` fiber backend:
  - `ppu_thread::cell_fiber_backend` opts in
  - `thread_base::start_fiber` admits a guest fiber (no `pthread_exit` on the host)
  - `thread_ctrl::wait_for` parks the fiber and `notify` requeues it
  - `CellFiberActivation` captures/restores TLS around park
- Without `ARMSX3_CELL_FIBER`, PPU guests remain native threads (topology+wait path).

## Go/no-go remaining (audit)

Still incomplete relative to `CELL_FIBER_AUDIT.md`:

1. Full TLS activate/deactivate for suspend-all slot/stamp and every wait path
   (`state.wait`, lv2/HLE direct waits, `atomic_wait_engine` registration).
2. Guest timeout deadline service while all fibers are parked.
3. Blocking I/O / host lock policy; stop/join/savestate quiescence for suspended C++ stacks.
4. Device gate: LBP boot+play with `ARMSX3_CELL_FIBER=1` must stay playable before LS/codegen.

Until those land, treat fiber dispatch as experimental behind the env flag.

## Device smoke expectations

- Topology: `SPU/helpers=0x3f PPU/RSX=0xc0` and `fiber host pinned cpu=6` / `cpu=7`.
- With `ARMSX3_CELL_FIBER=1`: log `fiber dispatch: guest start` and `dispatch_count` growth.
