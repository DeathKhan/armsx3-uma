# Current-runtime SPU replay validation

This is a correctness tool, independent of the `ARMSX3_CELL` production switch,
so the same test can run with the master off and on. It is not a performance
benchmark and has not yet been executed on the Odin.

## Portable capture and comparator checks

`segment.hpp` reads the archived interpreter captures without executing them.
It uses explicit little-endian metadata, retains GPR/FPSCR bytes in RPCS3 host
layout, and reconstructs canonical guest-order initial/final LS images. Exact
length, magic/version, instruction budget, aligned PCs, bounded/unique/aligned
LS differences and final opcode consistency are checked. There is no checksum
field in SSEG v1/v2; a SHA-256 manifest is separate provenance, not an embedded
format check. Output is assigned only after full validation.

`state_compare.hpp` is the comparator used by the runtime harness. It checks
all 128 registers, all 16 FPSCR bytes, every LS quadword (including locations not
listed in capture differences), PC, and the opcode actually present at that PC.
It rejects incomplete/malformed state views before reading their contents.

Build/run from the repository root; all binaries stay inside the worktree:

```sh
mkdir -p dist/cell-tests
c++ -std=c++20 -O2 -Wall -Wextra -Werror tools/cell/spu-replay/segment_test.cpp -o dist/cell-tests/segment_test
c++ -std=c++20 -O2 -Wall -Wextra -Werror tools/cell/spu-replay/state_compare_test.cpp -o dist/cell-tests/state_compare_test
c++ -std=c++20 -O2 -Wall -Wextra -Werror tools/cell/spu-replay/segment_audit.cpp -o dist/cell-tests/segment_audit
dist/cell-tests/segment_test
dist/cell-tests/state_compare_test
dist/cell-tests/segment_audit "$HOME/Android/armsx3-archive/segcap-vectors/segcap"
```

The audit considers only regular `seg-*.bin` files and excludes macOS `._`
metadata. These checks validate format and comparator contracts; they are **not
execution or bit-exact emulator validation**. Parser 39 and comparator 17 assertions
also passed with `-fsanitize=address,undefined` on the Arch host.

On 2026-11-05 all 1,200 captures parsed: version 2=1,200, malformed=0,
235,015 captured instructions (maximum 53,048), 161 segments with LS changes,
and zero FPSCR changes. Endpoints: RDCH 154, RCHCNT 10, WRCH 780, BI 253, BISL 1,
IRET 2. A nonchanging FPSCR still must be compared during runtime replay.
The input SHA-256 manifest is recorded under
`~/AgentOutput/2026-11-05/codex-cell/segment-input-manifest.sha256` on Arch;
its digest is `cabd9a747df617e0929df7290374c31ac2676786d048c59ea1ab198f830e2b08`.

## Isolated current-runtime entry

Generate the inert test input, a 260-byte ELF32 big-endian SPU executable:

```sh
python3 tools/cell/spu-replay/make_test_elf.py
readelf -h -l dist/cell-tests/cell-spu-replay.elf
```

After installing the test-enabled APK on an **idle device**, stage the ELF and
capture files where that app can read them. The default capture directory is
`<app files>/cache/cell-spu-replay/`; output is `result.txt` in that directory.
No capture or test data is embedded in the APK. Add the following to the existing
`driver_env.txt`, preserving its other settings, and cold-start the app:

```text
ARMSX3_CELL_REPLAY=1
```

An optional absolute `ARMSX3_CELL_REPLAY_DIR` overrides the input/output directory.
Environment access waits for `cell::environment_ready()`, which Android publishes
after importing `driver_env.txt`; an early check does not freeze an absent value.
`ARMSX3_CELL=0` or `1` independently selects the production architecture under test.

Boot `cell-spu-replay.elf` through the existing native frontend boot API. This
uses `System.cpp` -> `spu_load_exec`, creating the sole raw `test_spu` host thread
and initializing the actual SPU runtime/JIT. **The standalone loader pauses the
emulator with `Pause(true)`. Resume is required after boot:**
`ARMSX3.instance.resume()` calls
`Java_com_armsx3_core_ARMSX3_resume` -> `_armsx3_resume()` -> `Emu.Resume()`.
This is a source-verified call sequence; the Android boot/resume invocation and
successful replay still need device verification. Do not use the blocking
`Rpcs3Bridge.boot` wrapper on an instrumentation thread that must issue resume.

The replay entry refuses normal game contexts: it requires standalone `test_spu`,
no guest PPUs, exactly one guest SPU, no pending MFC work/reservation, and the LLVM
decoder with verification enabled. It never borrows a game's SPU and never runs
inside `suspend_all`, whose callback could execute under another CPU's TLS.
After the test it restores initial LS/GPR/FPSCR/PC and interpreter-control fields,
then parks the inert SPU with `cpu_flag::stop`. This is a terminal test session;
it does not automatically resume any game or continue the ELF's STOP instruction.
Remove `ARMSX3_CELL_REPLAY` and cold-start before normal gameplay.

## What is executed and checked

For each strict version 2 capture, the current production C++ interpreter first
runs pure instructions up to the first channel/IRQ boundary. It checks the exact
captured instruction count and all final state. The same initial state is then
imported for the current LLVM JIT through `spu_runtime::g_gateway`.

When replay mode is enabled, LLVM emits a hook immediately before every original
version 2 boundary, before optimized-channel/pattern handling. It flushes the
register context and updates PC only at that boundary. All ordinary arithmetic,
LS emitters, analyzer attributes and LLVM passes remain enabled. It does not
plant STOP opcodes or modify code/data to force an exit. Outside an active replay
the hook has no effect; with the mode disabled no additional LLVM IR is emitted.
Instrumented objects have a `-cell-replay-boundary-v1` module-name suffix, so they
cannot be mistaken for normal objects. The mode is immutable for the process.
No `spu_thread` layout fields are added.

Interpreter fallback during the LLVM run produces `interpreter_fallback`, never
a silent success. A 30-second watchdog asks the normal JIT state-check path to
escape; external pauses/stops abort the suite. This is a cooperative watchdog,
not protection against a host compiler deadlock or a loop lacking state checks;
an external process deadline is still required for automation.

`complete=1` means all inputs were processed without an interruption.
`passed=1` additionally requires no malformed input and exact interpreter and
LLVM final-state results for every input. Per-capture records include mismatch
counts for registers, FPSCR, LS quadwords, PC and endpoint opcode. Merely finding
`complete=1` is insufficient.

The LLVM instruction count is explicitly `UNMEASURED`; no exact execution-count
claim is made for LLVM. Endpoint instrumentation necessarily introduces the
final register flush. Fused channel/MFC side effects are not executed because
the capture ends before them, so this suite cannot validate those writers or
channel semantics. Existing captures cannot cover canonical save/load,
cross-representation DMA, late raw fallback, debugger edits or MMU invalidation.
Those need additional phase 3 tests and real-game gates.

The standalone loader constructs `test_spu` with `group == nullptr`, so the
current test context has **raw-SPU type and legacy guest-byte LS representation**.
Running this harness with the master on does not by itself exercise a future
managed-SPU host-order path: the required raw-title fallback could keep every
replay on legacy bytes. Before phase 3 acceptance, add either an isolated
synthetic managed group (still no guest PPUs) or an explicit test-only native
representation selector for pure captures. Record the actual representation in
each result and require both legacy and native suites. Canonical import/export
adapters and cache identity must follow that selection. This extension has not
been implemented and must not be inferred from a future 1,200-result legacy pass.

## Current evidence and remaining gate

The three modified Android translation units compile successfully with the
existing release configuration. The generated ELF's header/program table is
valid according to `readelf`. Portable parser/comparator sanitizer tests passed.
**No current-interpreter or current-LLVM execution result has been obtained on
the Odin yet.** A 1,200-record format pass must not be reported as 1,200 exact
emulation results.

The archive's `tools/spu-diff/spujit` drives the abandoned custom AArch64 compiler.
Its results must not be substituted for this current LLVM replay. The archived
in-app replay plants a STOP opcode into LS and ignores final FPSCR; that harness
was not reused.
