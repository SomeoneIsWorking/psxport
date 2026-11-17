"""Compare two executor traces (PSXPORT_EXEC_TRACE) and name the first block entry where they disagree.

The recompiler differential: run one recipe twice, once with PSXPORT_EXEC_INTERPRET=1 (the reference) and once
on the recompiler, each with PSXPORT_EXEC_TRACE set, then

    uv run --frozen python tools/exec_trace_compare.py --reference interp.trace --candidate jit.trace

Run the two sides one after the other, not at once: runs that share a memory card or other files outside the
guest (a route's card is blanked at start, then saved to) see each other's writes and diverge where the guest
reads them, which looks like an engine difference at a BIOS file call.

A checkpoint records the committed CPU state at one block entry (runtime/cpu/executor_trace.h). The first
checkpoint that differs points at the block run just before it (`previous pc`). When checkpoints are sparse, the
tool prints the PSXPORT_EXEC_TRACE_FROM/_TO window to rerun both sides with, which records every block entry
between the last agreeing checkpoint and the first disagreeing one, so the second comparison names the block.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

RECORD = struct.Struct("<QQII32I2I16I4IQQII")
assert RECORD.size == 264
FIELDS = (["block", "executed", "pc", "previous_pc"] + [f"r{i}" for i in range(32)] + ["hi", "lo"]
          + [f"cop0_{i}" for i in range(16)]
          + ["pending_reg", "pending_value", "arriving_reg", "arriving_value", "gte_hash", "ram_hash",
             "ram_hashed", "reserved"])
GPR_NAMES = ("zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra"
             .split())


def records(path: Path):
    data = path.read_bytes()
    if len(data) % RECORD.size:
        sys.exit(f"{path}: {len(data)} bytes is not a whole number of {RECORD.size}-byte checkpoints")
    for offset in range(0, len(data), RECORD.size):
        yield dict(zip(FIELDS, RECORD.unpack_from(data, offset)))


def label(field: str) -> str:
    if field.startswith("r") and field[1:].isdigit():
        return f"{field} (${GPR_NAMES[int(field[1:])]})"
    return field


def differences(a: dict, b: dict) -> list[str]:
    out = []
    for field in FIELDS:
        if field in ("ram_hash", "ram_hashed", "reserved"):
            continue
        if a[field] != b[field]:
            out.append(f"  {label(field)}: reference 0x{a[field]:X}, candidate 0x{b[field]:X}")
    if a["ram_hashed"] and b["ram_hashed"] and a["ram_hash"] != b["ram_hash"]:
        out.append("  main RAM or scratchpad differs (hash)")
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--reference", type=Path, required=True, help="the interpreter run's trace")
    parser.add_argument("--candidate", type=Path, required=True, help="the recompiler run's trace")
    args = parser.parse_args()
    last_agreeing = None
    compared = 0
    for a, b in zip(records(args.reference), records(args.candidate)):
        if a["block"] != b["block"]:
            sys.exit(f"the traces checkpoint different blocks ({a['block']} vs {b['block']}): "
                     "were they recorded with the same _EVERY and _FROM/_TO?")
        found = differences(a, b)
        if not found:
            last_agreeing = a
            compared += 1
            continue
        print(f"first difference at block entry {a['block']} (pc 0x{a['pc']:08X}, "
              f"after {a['executed']} reference instructions); the block run before it entered at "
              f"previous pc 0x{a['previous_pc']:08X} (candidate 0x{b['previous_pc']:08X}):")
        print("\n".join(found))
        if last_agreeing is None:
            print("no earlier checkpoint agrees")
        elif a["block"] - last_agreeing["block"] > 1:
            print(f"last agreeing checkpoint: block entry {last_agreeing['block']}. To name the block, rerun both "
                  f"sides with PSXPORT_EXEC_TRACE_FROM={last_agreeing['block']} "
                  f"PSXPORT_EXEC_TRACE_TO={a['block'] + 1}")
        else:
            print(f"the block entered at 0x{a['previous_pc']:08X} (entry {a['block'] - 1}) is the first divergent "
                  "block: the state agreed when it was entered")
        return 1
    print(f"{compared} checkpoint(s) agree"
          + (f", through block entry {last_agreeing['block']}" if last_agreeing else ""))
    ref_count = args.reference.stat().st_size // RECORD.size
    cand_count = args.candidate.stat().st_size // RECORD.size
    if ref_count != cand_count:
        print(f"the traces differ in length: reference {ref_count}, candidate {cand_count} checkpoint(s)")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
