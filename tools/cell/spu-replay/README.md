# Current-runtime SPU replay validation

`segment.hpp` reads the archived interpreter captures without executing them.
It uses explicit little-endian metadata, retains GPR/FPSCR bytes in RPCS3 host
layout, and reconstructs canonical guest-order initial/final LS images. Exact
length, magic/version, instruction budget, aligned PCs, bounded/unique/aligned
LS differences and final opcode consistency are checked. There is no checksum
field in SSEG v1/v2; a SHA-256 manifest is separate provenance, not an embedded
format check. Output is assigned only after full validation.

Build/run from the repository root (outputs stay inside the worktree):

```sh
mkdir -p dist/cell-tests
c++ -std=c++20 -O2 -Wall -Wextra -Werror tools/cell/spu-replay/segment_test.cpp -o dist/cell-tests/segment_test
c++ -std=c++20 -O2 -Wall -Wextra -Werror tools/cell/spu-replay/segment_audit.cpp -o dist/cell-tests/segment_audit
dist/cell-tests/segment_test
dist/cell-tests/segment_audit "$HOME/Android/armsx3-archive/segcap-vectors/segcap"
```

The audit considers only regular `seg-*.bin` files and excludes macOS `._`
metadata. These checks validate the capture format; they are **not execution or
bit-exact emulator validation**.

On 2026-11-05 all 1,200 captures parsed: version2=1,200, malformed=0,
235,015 captured instructions (maximum53,048), 161 segments with LS changes,
and zero FPSCR changes. Endpoints: RDCH154, RCHCNT10, WRCH780, BI253, BISL1,
IRET2. A nonchanging FPSCR still must be compared during runtime replay.

The archive's `tools/spu-diff/spujit` drives the abandoned custom AArch64
compiler. Its results must not be substituted for a current LLVM replay.
The archived in-app replay plants a STOP opcode into LS and ignores final
FPSCR; it is not a suitable acceptance harness either.

A current runtime harness is being designed around the existing standalone
SPU ELF boot path (`System.cpp` -> `spu_load_exec` -> sole `test_spu`). It must
reject a normal game context (any guest PPU or more than one guest SPU), use
current LLVM emitters, stop before actual channel/IRQ boundaries without
changing LS opcodes, compare all GPR/FPSCR/LS/PC state, reject interpreter
fallback during the LLVM run, and use a separate instrumented object identity.
A full instruction counter must not be claimed unless implemented; the
current interpreter can establish the captured instruction count independently.
No runtime or device execution result has been obtained yet.
