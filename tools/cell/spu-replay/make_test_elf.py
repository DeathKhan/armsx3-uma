#!/usr/bin/env python3
"""Generate an inert standalone SPU ELF for the current-runtime replay entry.

The ELF contains STOP0; the replay hook runs before it and returns parked.
Output must be in the worktree's dist/ or target/ directory.
"""
from pathlib import Path
import struct
import sys

root = Path(__file__).resolve().parents[3]
output = Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else root / "dist/cell-tests/cell-spu-replay.elf"
if not any(output.is_relative_to(root / folder) for folder in ("dist", "target")):
    raise SystemExit("output must stay in this worktree's dist/ or target/")
ident = b"\x7fELF" + bytes((1, 2, 1, 0)) + bytes(8)
header = struct.pack(">16sHHIIIIIHHHHHH", ident, 2, 0x17, 1, 0x100, 52, 0, 0, 52, 32, 1, 40, 0, 0)
program = struct.pack(">IIIIIIII", 1, 0x100, 0x100, 0x100, 4, 16, 7, 16)
image = header + program
image += bytes(0x100 - len(image)) + bytes(4)
output.parent.mkdir(parents=True, exist_ok=True)
output.write_bytes(image)
print(f"Created {output} ({len(image)} bytes)")
