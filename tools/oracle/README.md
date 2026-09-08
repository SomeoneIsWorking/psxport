# Isolated CPU windows and snapshots

These executables run the independent Mednafen CPU in a separate process. They do **not** implement
full-console saves or gameplay-frame comparison: GPU, CD, SPU, timers, SIO and DMA channels remain
unsupported, and touching them stops and taints the window. No tool here is a player CPU selector.

Build with the repository's Clang-configured CMake tree:

```sh
cmake --build build/ci --target oracle_spike oracle_trace oracle_window test_oracle_snapshot
build/ci/tools/oracle/test_oracle_snapshot scratch/oracle-snapshot
```

`oracle_spike` remains the independent CPU/device discriminator. `test_oracle_snapshot` exercises
actual CPU load/branch delay continuations across snapshot restore, complete owner-state replay,
malformed-input atomic refusal, hardware-taint preservation, and explicit boundary normalization.

## Exact isolated-oracle snapshot

```sh
build/ci/tools/oracle/oracle_window \
  --load scratch/oracle/before.snapshot --steps 100 \
  --stop-at 0x80012340 --save scratch/oracle/after.snapshot
```

`--steps` counts attempted calls to the oracle's existing single-step owner. A hardware/event refusal
is reported and returns failure; its final snapshot retains the sticky taint. `--stop-at` compares the
committed PC before executing that instruction and fails if it is not reached inside the cap.
`--steps 0` roundtrips state without executing guest instructions. Input and output names must differ.

The snapshot includes the CPU owner's registered state: GPRs, HI/LO, PC/next-PC, branch-delay bits,
pending load target/value and absorption, read timing, GTE/multiply deadlines, event deadline,
interrupt cache, halt state, BIU, instruction cache, CP0 and scratch RAM. Supplemental fields use the
public `PS_CPU` owner definition for its omitted load-dummy/absorption slots, address masks and dummy
fetch page. Raw `GteRegs::REG/FLAGS` retain bits that the compatibility GTE savestate schema normalizes.
Main RAM, IRQ asserted/mask/status, DPCR, and shim timestamp/stop/taint/device-write provenance are
included. FastMap and GTE register pointers remain process-local bindings, never serialized pointers.
PGXP is disabled in this oracle; its diagnostic history is not CPU execution state.

Restore obtains each field's storage from the current owner, validates the entire file/schema and
execution indexes, then copies. It does not call the compatibility load path that normalizes
pending-load and raw GTE values. Malformed input preserves all live state. The format is exact for
this isolated emulator; it does not assert equivalence to an arbitrary production Core snapshot.

## Explicit normalized architectural boundary

When the product cannot supply emulator-private history, a different command declares the loss:

```sh
build/ci/tools/oracle/oracle_window \
  --boundary scratch/oracle/registers.txt \
  --ram scratch/oracle/native.ram --scratch scratch/oracle/native.spad \
  --steps 100 --stop-at 0x80012340 --save scratch/oracle/window.snapshot
```

The register file is strict text. Each field is required, in this order; numeric tokens are unsigned
32-bit hexadecimal, optionally prefixed with `0x`. Arrays have exactly the indicated number of words:

```text
ORACLE_BOUNDARY_V1
pc <instruction PC>
next_pc <PC plus 4>
branch_pending <0>
load_pending <0>
irq_pending <0>
device_pending <0>
gpr <32 words: r0 through r31>
lo <word>
hi <word>
cp0 <32 words in architectural register order>
gte <64 raw words: DR0..DR31, CR0..CR31>
gte_flags <raw FLAGS word>
```

RAM must be exactly 2 MiB and scratch exactly 1 KiB. Supply captured original title memory, including
the current resident overlay. The caller must establish that this is a clean instruction boundary:
no pending branch/load/IRQ/device service, sequential next-PC, zero r0, a main-RAM PC, and no cache
isolation. Pending interrupt bits in CP0 Cause also refuse import. These declarations are not inferred
from a frame number or silently filled in by the importer.

The importer reconstructs the oracle through its lifecycle owner, then loads the supplied architectural
state. Constructor-owned halt, address-map and dummy state therefore cannot leak between windows.
Timing starts at zero with a zero event deadline (the step owner sets each actual budget); cache, IRQ
and DPCR start from their reset owners' values.
This is useful for bounded hardware-free game-function semantics, **not** cycle/device parity or
reconstruction of the complete original console. The production side must use the same declared
normalization or compare only state unaffected by it and justify each exclusion. Include original
image identity, the reached function and input boundary in the comparison report. A normalized input
must never be labelled an exact console snapshot.

## Binary format, version 1

All integers and numeric elements use little-endian encoding. The 24-byte header is:

- bytes 0..7: ASCII `ORASNAP1`;
- u32 version (1), u32 field count, u32 complete file size, u32 payload checksum.

The checksum is FNV-1a32 over bytes 24 through EOF; it detects accidental corruption and is not an
authentication mechanism. Files are bounded to 3 MiB and 64 fields. Every payload record contains u32
name-byte-count, u32 data-byte-count, u32 numeric-element-width (1/2/4/8), the exact un-terminated UTF-8
owner name, then the data bytes. All fields must match the current owner's order, names, sizes and
widths; missing, duplicate, reordered, extra and trailing data refuse. There is no tolerant migration
or host-structure padding. The named schema and version must change deliberately when ownership does.
