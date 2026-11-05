# Cell local-store conversion audit

Audit base: `15eff3cc2`, branch `codex/cell`, 2026-11-05. This document records
source inspection and architectural contracts. No runtime LS conversion, MMU
tracking, cache replacement, or device validation has been performed. A guarded
current-LLVM replay harness was subsequently implemented under
`tools/cell/spu-replay`, compiled, and documented; it has not been executed on
the device.
Phase 3 remains gated on the topology/wait and fiber game checkpoints.

## Representation contract

Current `spu_thread::ls` is guest byte order. The interpreter accesses it through
`to_be_t<T>*`, and the LLVM LQ/STQ emitter reverses a whole 16-byte vector when
crossing between LS and a host register (`SPULLVMRecompiler.cpp:10385`). The useful
new representation is therefore **byte reversal within each aligned 16-byte
quadword**, not a swap of each 32-bit lane. For a logical guest byte address `a`,
physical address is `(a & ~15) | ((a & 15) ^ 15)`; equivalently `a ^ 15` after LS
wrapping. An aligned scalar of width `n` in {1,2,4,8} can use a native scalar load
at `a ^ (16-n)`. Instruction fetch is a native 32-bit load at `pc ^ 12`.

The quadword-only assumption applies to SPU LQ/STQ instructions. It does not apply
to all emulated accessors: syscalls accept 1/2/4/8-byte LS operations, MFC supports
small transfers and 8-byte list elements, instruction fetch uses 4-byte words,
and C++ HLE code accesses arbitrary packed structures. A pointer cast cannot
translate a structure that spans one or several reversed quadwords.

Introduce explicit APIs before conversion: guest-byte read/write, guest scalar
read/write, native quadword load/store, canonical snapshot/import, and a transfer
endpoint carrying representation and logical offset. Preserve the existing raw
pointer APIs only for legacy mode. Every writer must publish invalidation, even
when a transfer is optimized or a patch is applied during loading.

## Aliases and protection domains

| Alias | Current backing and consumer | Required treatment |
|---|---|---|
| `ls - 2*LS_SIZE` through `ls + 2*LS_SIZE` | Five mappings of one SHM object; `SPUThread.cpp:2049`; JIT address arithmetic deliberately uses wrap mirrors | All five aliases must participate in write tracking, or replace mirror-based arithmetic everywhere. Protecting the center alias alone is incomplete. |
| `vm::g_base_addr + vm_offset()` | Normal VM mapping of the same SHM; managed allocation has `alloc_hidden`, raw allocation readable/writable | Managed normal mapping is already inaccessible (`sys_spu.cpp:870`, `vm.cpp:1616`); making only this mapping read-only does not track JIT writes. |
| `vm::g_sudo_addr + vm_offset()` | Unprotected VM mirror, mapped RW; `vm.cpp:1245` | C++ writers through this alias need explicit dirty publication. It cannot simultaneously expose canonical bytes and native-order bytes through an ordinary alias. |
| `shm->map_self()` / `vm::get_super_ptr` | Additional RW mapping used by serialization, debugger, reservation helpers | Convert snapshot/import and ensure write tracking; do not assume normal-VM permissions cover it. |
| Cross-SPU redirected LS pointers | `SPUThread.cpp:3078` redirects DMA to another thread's `ls + offset` | Endpoint must carry target representation and dirty ownership; source thread's dirty mask is not the target's mask. |

`utils::shm` uses `memfd_create` (`rpcs3/util/vm_native.cpp:544`) and `MAP_SHARED`
fixed mappings (`:873`). Replacing just one mapping with anonymous copy/remap
severs coherence with every other alias. Sub-page byte reversal also cannot be
provided by `mmap`: page aliases preserve offsets within a page.

Android's signal filter accepts only guest VM, executable, and segment-map ranges
(`Utilities/Thread.cpp:3156`). Independently reserved `ls` aliases are outside
those ranges. A protected JIT LS store would be forwarded as a foreign fault,
not handled as code invalidation, unless LS ranges are explicitly registered in
the fault domain first. Registration/unregistration needs an allocation-free,
signal-safe lookup and safe lifetime management during SPU destruction.

## Accessor and writer inventory

Line numbers refer to the audit base; neighboring phase edits can move them.
The table covers the observed accessor families, including pointer escapes.
A text search alone is not a proof that every raw-pointer-derived writer has
been migrated; conversion acceptance requires removing or auditing each escape.

| File / sites | Reads or writes | Migration obligation |
|---|---|---|
| `SPUThread.h:794,971-980` | Public `ls`, `_ptr<T>`, `_ref<T>` expose arbitrary BE pointers/references | Replace call sites with explicit access APIs; a universal native-order `_ptr<T>` replacement is unsound for structs, spans, and pointer arithmetic. |
| `SPUThread.cpp:2049,2132,2195` | Initial mapping, restored mapping, construction | Immutable representation flag, alias registration, dirty-state initialization; preserve old constructors in disabled mode. |
| `SPUInterpreter.cpp:612,765,1654,1694,1708,1750,1856,1863` | All eight LQ/STQ effective-address forms | Native quadword loads/stores plus dirty publication for STQ; fallback interpreter must obey current SPU mode. |
| `SPUThread.cpp:1842,1868`; `SPUCommonRecompiler.cpp:339` | LLVM gateway, generated interpreter, old interpreter pass raw LS pointers | Mode-aware entry ABI, opcode access, and compatibility fallback; not only the main LLVM backend. |
| `SPULLVMRecompiler.cpp:10385-10554` | `make_store_ls`, `make_load_ls`, all LQ/STQ forms | Remove vector reversal for native mode only; maintain raw legacy variant; STQ writer coverage. |
| `SPULLVMRecompiler.cpp:2119+` | Single-word, two-word, vector/checksum entry verification | Replace only after invalidation design is proven; expected bytes and masks must use correct mode and full dependency pages, including holes and wrap. |
| `SPULLVMRecompiler.cpp:4557,4625,4789` | LLVM-generated interpreter opcode fetch and check paths | `pc ^ 12` native fetch; avoid reusing legacy instruction tables against native bytes. |
| `SPULLVMRecompiler.cpp:5951-6100` | Inlined normal MFC GET/PUT/SNDSIG with scalar and vector copies | Convert partial/scalar copies and bulk quadwords, mark GET destination pages. Do not leave this fast path bypassing C++ conversion. |
| `SPULLVMRecompiler.cpp:1452,1529,1638+` | Specialized PUTLLC16/PUTLLC0 source reads and native LLVM compare/store paths | Convert LS-side values while keeping reservation/main-memory data canonical; test success and failure paths. |
| `SPUThread.cpp:2831-3914` | Normal DMA, accurate DMA, MMIO, zero-fill and copy fallbacks | Cover GET and PUT, all sizes/alignment, retry/temp buffers, SDCRZ, accurate-store loops, cross-SPU and raw endpoints, error zero-fills. A dirty hook at function start alone does not cover all writers. |
| `SPUThread.cpp:3047-3085` | Raw-MMIO register and cross-thread LS/SNR transfers | Distinguish MMIO value endian from LS representation; raw-to-managed and managed-to-raw need conversion. |
| `SPUThread.cpp:3916-4473` | List descriptors and optimized six-element GET, optimized GET/PUT batches | Decode 8-byte descriptors from logical LS; all `MOV_T`/`MOV_128` cases, direct `this->ls + arg_lsa`, and stalled-list resume offsets need conversion/tracking. |
| `SPUThread.cpp:4590,5069` | PUTLLC and PUTLLUC/PUTQLLUC source buffers | Convert native LS to canonical reservation-store buffer; preserve lock/fence/RSX semantics. |
| `SPUThread.cpp:5618,5647,6013` | GETLLAR writes 128-byte reservation snapshots into LS | Convert each quadword and publish all affected pages; keep `rdata` canonical for memory comparison. |
| `SPUThread.cpp:4848,6040,6146` | SPURS idle-byte heuristics read LS bytes directly | Logical-byte access; otherwise busy-wait decisions and sleep behavior change even when guest instructions are correct. |
| `SPUThread.cpp:6204,6218,6234` | MFC debug/history snapshots | Emit canonical bytes for diagnostics. |
| `SPUThread.cpp:934-942,1040+,1196+,1373-1390` | Reservation-state comparisons, callstack/function scans, diagnostic LS dump | Canonical snapshot or scalar accessor; preserve logical address calculations and dump format. |
| `SPUThread.cpp:5243-5414` | `is_exec_code` and branch-target scans | Use canonical snapshot or mode-aware instruction reader. |
| `SPUThread.cpp:8734` | `fast_call` writes STOP instruction at LS0 | Use tracked scalar writer; restore semantics and invalidate page0. |
| `SPUThread.cpp:8757-8924` | ELF/capture export and rewind import | Canonical export/import; restored memory invalidates all relevant blocks and resets mapping protections safely. |
| `SPUCommonRecompiler.cpp:619,700+` | Global dispatcher, ARM64 trampoline uses first instruction as index | Mode-aware instruction load, key and dispatch partition; native code cannot share a legacy entry by program hash alone. |
| `SPUCommonRecompiler.cpp:2613,2644,2865-2970` | Runtime content lookup, verification-failure recovery, analyze and branch patchpoints | Representation-aware canonical identity and per-SPU invalidation; retire patched direct edges or validate versions on every entry. |
| `SPUCommonRecompiler.cpp:3181-3303,3695-9915` | Function discovery, full analyzer, opcode lookahead and generated program bytes | Canonical opcode reader/snapshot; retain guest-order `spu_program` identity. Program extraction must not preserve physically reversed instruction order. |
| `SPUCommonRecompiler.cpp:10031-10144` | Cache-worker reconstructed LS and analyze | These temporary canonical buffers must remain canonical; do not apply native indexing to every `ls` variable indiscriminately. |
| `SPUCommonRecompiler.cpp:11004` | ASMJIT pattern analysis | Preserve canonical source or legacy-only path. |
| `SPUASMJITRecompiler.cpp` | x64 LQ/STQ, verification and fast helpers | Keep current representation when master is disabled or on unsupported hosts. If native mode is ever enabled here, requires a separate full audit/conversion. |
| `lv2/sys_spu.cpp:131-234` | Image COPY/FILL deployment, executable hash, patch-engine raw translation callback | Apply patches to canonical staging bytes, hash canonical content, then import through tracked API. Arbitrary patch-engine pointers cannot refer directly to a native-order view. |
| `lv2/sys_spu.cpp:1206-1318` | Loader memset, loader instruction bytes, image deployment | Build complete canonical image and convert once before running; track writes on restart and repeated deployment. |
| `lv2/sys_spu.cpp:1918-2000` | Public read/write LS syscalls, widths1/2/4/8 | Validate logical address then mode-aware scalar API; publish dirty pages on writes. |
| `lv2/sys_spu.cpp:2549-2623`; `Modules/sys_spu_.cpp:408` | Raw/isolated creation and raw image load | Preserve raw representation and MMIO byte compatibility; decide title-wide fallback before mixed execution or perform a safe stop-all migration. |
| `RawSPUThread.cpp:166` | Raw proxy MFC with `_this == nullptr` and raw LS pointer | Existing raw path stays canonical; endpoint-mode inference cannot rely only on `_this`. |
| `RawSPUThread.cpp:500-589` | Test/standalone executable and relocatable SPU loading | Preserve legacy mode or canonical import API, including direct memcpy segment writes. |
| `Modules/cellSpursSpu.cpp:99-2072` | HLE kernel/taskset structures, direct field mutations, memcpy/memset, ELF loads, context save/restore | Many typed pointers escape. Audit callers to determine reachability (some HLE registration is commented), then convert or explicitly keep this entire SPU path legacy; do not silently retain BE structs over native storage. |
| `SPUDisAsm.cpp:14-81` and further const-value scans | Disassembly and backward opcode reads from `m_offset` | Provide canonical snapshot / instruction reader; `get_memory_span` callers need an explicit representation contract. |
| `util/emu_utils.cpp:40`; `RSX/rsx_profiler.cpp:1177` | Disassembler construction from raw `spu.ls` | Supply mode-aware source instead of a plain pointer. |
| `rpcs3qt/instruction_editor_dialog.cpp:31,134`; `debugger_frame.cpp:736` | Direct opcode edit and branch analysis | Canonical UI and tracked write API; no stale code after debugger edits. |
| `rpcs3qt/memory_viewer_panel.cpp:59,739` | Independent `shm->map_self()` reader and derived pointers | Canonical display/search/edit access; representation must travel with pointer. |
| `rpcs3qt/memory_string_searcher.cpp:196,250` | Raw instruction/string scans from memory viewer pointer | Canonical read semantics; embedded SPU images in main memory remain canonical. |
| `rpcs3qt/guest_memory_dumper.cpp:263+` | SPU dump metadata and full SHM/VM images | Export canonical guest bytes without mutating running storage. |
| `Memory/vm.cpp:2796-2866` | Whole shared-memory save/load serializes raw SHM images | Canonicalize SPU-owned regions and record/interpret save format explicitly. SPUThread::save alone does not own LS payload serialization. |
| `Utilities/Thread.cpp:3156,3250,3627` | Android fault classification/recovery | Explicit LS alias ownership and write-fault handling before foreign-fault forwarding; lifetime-safe and async-signal-safe. |

`SPUAnalyser.h` describes decoded instruction flags/types; the byte-consuming
analysis implementation is primarily in `SPUCommonRecompiler.cpp`. Updating
only a file named Analyser would miss the actual memory consumers.

## Dirty bitmap and code validity

A 256KiB LS has exactly 64 4KiB pages. A per-SPU 64-bit dirty bitmap is a useful
notification summary, but is not by itself a block validity version.

Counterexample: functions A and B use different bytes of page0. B changes;
page0 becomes dirty. A enters, its bytes still match, and clears page0. B then
enters stale compiled code while observing a clean bitmap. Per-function clear
is unsound even with perfect writer coverage.

Two viable contracts:

1. Snapshot/exchange pending dirty bits, invalidate **every** compiled block
   dependent on any affected page, and retire **all callable edges** to those
   stale blocks before clearing/publishing clean state. Existing direct branch
   patchpoints and the globally shared SPU runtime mean a lookup-table clear
   alone does not meet this contract.
2. Keep per-page generations (or per-block validity tied to those generations)
   per SPU. Each entry checks its entire code dependency set against the versions
   under which that SPU validated the block. Dirty bitmap remains a work summary;
   clearing it never makes other blocks implicitly valid.

Both need correct writer publication ordering, synchronization with code
protection/unprotection and concurrent transfers, and wrap-aware masks. Publishing
only before a long DMA is not enough to prove an entry cannot validate halfway
through the write. A post-write publication or write-in-progress epoch and a
stable validation snapshot is necessary. Every block generation must include
all source instruction pages, including holes/overlaps and PC-relative wrap.

Protecting code pages after first verification can intercept ordinary JIT STQ
stores, but only if all actual JIT aliases are protected and C++ writers use an
unprotected tracked accessor. First-write unprotection must invalidate every
cached dependency on that page. A deliberate monotonic dirty mask can be safe
if it never clears after writes, but is likely to lose most benefit on SPURS
code overlays; it must not be presented as the requested complete replacement.

## Raw-SPU fallback and huge pages

Raw local stores are directly exposed at MMIO addresses. Ratchet & Clank is a
known raw-SPU consumer in comments around `SPUThread.cpp:3124`. Raw mode must
keep canonical bytes and legacy verification. If the requirement is **title-wide**
legacy representation whenever raw SPUs are used, detecting raw creation is not
sufficient unless creation precedes all managed execution. A late first raw SPU
requires stopping all guest CPUs, canonicalizing existing native LS regions,
invalidating both runtime and disk code identities, and restarting dispatch.
A per-SPU immutable mode avoids live conversion but then mixed-representation
transfers must be fully supported; it differs from title-wide legacy fallback.

An anonymous huge-page LS allocation is a second mapping redesign, not a local
`madvise` addition. Current 256KiB LS uses a memfd and alias mirrors; replacing
one alias with anonymous pages disconnects it from VM/savestate/debugger/raw
views. Six LS buffers can fit within a 2MiB arena, but 4KiB read-only protection
for code tracking can split a PMD mapping, and alias semantics still need a
coherent replacement. A claim of huge-page success requires `/proc/self/smaps`
`AnonHugePages`/kernel page-size evidence after code protection is active, not
only `MADV_COLLAPSE` returning success before page permissions change.

## Code cache requirements

The base object cache is `spuobj-v4-*`, built in
`SPUCommonRecompiler.cpp:1858-1962`. It already folds `__DATE__ __TIME__` and
`spu_llvm_codegen_build_stamp()`, plus settings and hardware features. This is
stronger for uncommitted builds than a git hash alone; preserve it. Fold the
master/native-representation and verification ABI choices into both persistent
and in-memory runtime identities, and bump the object version when production
conversion changes the ABI/codegen. A version bump without a runtime partition
still permits incompatible shared functions within one process.

Do not add fields to `spu_thread` without retiring old objects. User-requested
removal of old `spuobj-v4-*` directories must be restricted to this package's
cache and performed only during authorized idle-device installation, never
while a game is open. No cache files were deleted by this audit.

## Differential validation gap and plan

`~/Android/armsx3-archive/segcap-vectors/segcap` contains **1,200 `seg-*.bin`**
files plus 1,200 macOS `._` metadata files; metadata files are not vectors.
`tools/spu-diff` exists on `archive/spu-compiler-2026-11-03`, not this branch.
Its `spujit` drives the archived custom `SPUA64Compiler.hpp`, not the current
LLVM SPU implementation. Running that tool would not validate new LLVM code.
The archive README also notes version1 interrupt-enable boundary captures.

A meaningful new harness must:

- Read the existing capture format and validate version/bounds/checksum before
  replay. Establish baseline interpreter/reference results first and explicitly
  classify any existing incompatible/version1 records.
- Construct the current production SPU context/JIT, import canonical initial LS
  through the selected representation, restore all captured registers/PC and
  relevant FP semantics, and stop at the same channel/STOP/interrupt boundary.
- Export canonical final LS and compare **all** captured state, not just touched
  registers; verify the expected boundary and instruction count. Run master off
  and on against the same vectors; a custom-compiler-only result is insufficient.
- Add vectors for scalar LS syscalls, small/list DMA, GETLLAR/PUTLLC success/fail,
  self-modifying STQ, two functions on one page, cross-SPU transfer in both modes,
  late raw fallback, debugger writes, and canonical save/load cross-mode roundtrip.
  Existing between-channel pure segments cannot exercise these external writers.
- Gate phase3 on the 1,200-vector outcome plus LBP and Ratchet boot/play/save,
  applet exit and ten boot loops. Do not use a host model test as this gate.

## Feasible conversion sequence and scope

1. Establish canonical access/transfer APIs with mode still legacy. Migrate
   every raw pointer escape in the table, retaining byte-exact old behavior.
2. Add the actual current-LLVM differential harness and external-writer tests.
3. Add immutable representation selection, runtime/disk cache partition, native
   interpreter/opcode access, and canonical deployment/snapshot/import.
4. Convert C++ and inline LLVM MFC/atomic paths plus HLE/debugger consumers in
   the same integration phase. Enable native LQ/STQ only once the full endpoint
   matrix passes, retaining raw legacy and disabled master paths.
5. Add code-alias fault registration, per-page invalidation generations and
   entry stubs, then prove no stale patched-edge path remains. Re-run all prior
   correctness gates after verification changes.
6. Rework LS mapping/huge-page arena only with alias coherence and actual PMD
   evidence; repeat save/debugger/fault tests after permission changes.

This is at least a **23-file runtime consumer/mapping migration plus a current
LLVM replay harness**, not a two-function byte-swap edit. The table includes
additional desktop-only consumers because the disabled path must remain sound
and raw pointer APIs must have a clear contract. Scope can be staged inside the
single architecture branch without claiming independent performance wins.
The final acceptance comparison remains the one combined build versus release.

## Executable model evidence

`python3 tools/cell/tests/test_ls_architecture_model.py -v` passed eight tests
on the Arch host. They exercise native quadword/scalar address contracts,
480 deterministic partial/boundary/wrap transfer cases across all four
legacy/native endpoint combinations, overlapping copy snapshot semantics,
page63/full-mask handling, the shared-page stale-code counterexample, complete
dependency invalidation, and writes published after dirty-bit exchange.

These are executable design contracts only. They exercise no production
emulator code, provide no timing claim, and do not replace any phase gate.
