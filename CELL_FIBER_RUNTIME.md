# PPU fiber runtime status

## What is wired (commit `a14e6e458`+)

- `Utilities/CellFiber.{h,cpp}` and `CellScheduler.{h,cpp}` are linked into `rpcs3_emu`.
- Host fixtures still pass: `tools/cell/tests/run_fiber.sh`, `run_scheduler.sh`.
- `CellPpuRuntime` starts **two** scheduler hosts when `ARMSX3_CELL=1` (from
  `thread_ctrl::apply_cell_policy`). Hosts pin to the upper topology mask when
  detect succeeds.
- `ARMSX3_CELL_FIBER=1` is reserved for named_thread\<ppu_thread\> dispatch. It is
  **off** by default; without it PPU guests remain native threads (topology+wait
  path that measured 152 Mc/f on LBP).

## Go/no-go for enabling fiber dispatch

Do **not** set `ARMSX3_CELL_FIBER=1` or convert `named_thread<ppu_thread>` until:

1. Phase1 correctness on device: LBP 10 boot loops, savestate round-trip, clean
   exit (Ratchet skipped: release control also SIGSEGV in SPU LLVM verify).
2. Audit matrix in `CELL_FIBER_AUDIT.md` for waits, TLS activate/deactivate,
   join/exit, and savestate quiescence has code behind the flag.
3. LBP boot+play with fiber dispatch still meets the wait/topology gate (no WFE
   spin regression; playable Sackboy).

## Remaining work (audit scoped plan)

1. PPU-only `thread_base` backend; guest completion must not `pthread_exit` a host.
2. Explicit CPU/Thread/vm activate/deactivate; assert no lv2 mutex across switch.
3. Hook every PPU `state.wait` / lv2 / HLE wait into scheduler-owned records.
4. Guest timeout deadline service while all fibers are parked.
5. Blocking I/O / host lock policy; stop/join/savestate quiescence.

Until that gate passes, local-store and codegen/layout stay disabled as a completed rebuild.
