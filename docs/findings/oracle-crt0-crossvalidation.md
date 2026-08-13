# The crt0 boot group, checked by EXECUTION against an independent emulator

**Measured 2026-08-13.** Every number below came from running the code, not from reading it.

## What was wrong with how the crt0 was verified before

The boot group each game ships in `game_config.cpp` — `bssZeroLo/Hi`, `stackTopBase`, `stackBias`,
`heapBase`, `gp`, `libcInit` — is derived by `tools/crt0_extract`, which DECODES the prologue
symbolically. `crt0_audit` then re-derives it from the guest instruction stream on every boot and refuses
on disagreement. Three checks, and **all three run the same decoder** (`crt0_scan` in
`runtime/recomp/crt0_verify.h`). A misreading of the prologue would be confirmed by every one of them
forever. That is this workspace's recurring failure in its purest form: an instrument that cannot show
the other answer.

`tools/oracle/oracle_trace` breaks it. It derives nothing — it injects the real executable into the
vendored Mednafen PSX CPU (no `libretro.c`, no BIOS mapped) and records what the registers actually
held, instruction by instruction. The two methods share no code: one reads instructions, the other runs
them.

## The result: 24 comparisons across 6 executables, 24 agree, 0 disagree

`tools/oracle/crossvalidate_crt0.py <exe>` runs both methods and diffs them field by field IN CODE — not
by hand, because hand-comparison is how a session reports agreement it never checked.

| executable | game | left text at step | `gp` | `libcInit` | `$t1` | `InitHeap a0` |
|---|---|---|---|---|---|---|
| `SCUS_942.28` | Spyro | 26,903 | `0x80075264` | `0x8005DB14` | `0x39` | `0x8007AA3C` |
| `SCUS_944.54` | Tomba! 2 | 20,193 | `0x80038458` | `0x80018CEC` | `0x39` | `0x8003C35C` |
| `SLUS_008.75` | Spider-Man | 85,873 | `0x800B47F4` | `0x8008DC98` | `0x39` | `0x800C65D8` |
| `SLUS_010.40` | Vagrant Story | 65,053 | `0x80033674` | `0x80026864` | `0x39` | `0x800401AC` |
| `SLUS_008.93` | Toy Story 2 | 246,534 | `0x800A0CD8` | `0x80089344` | `0x39` | `0x800D12C4` |
| `SLUS_005.61` | Mega Man X4 | 361,994 | `0x8012F418` | `0x800EDCDC` | `0x39` | `0x80175F3C` |

Every column is BOTH methods' value — they are identical, which is the finding. **All six leave the
game's own text at `pc = 0x000000A0`** (the BIOS A-function call vector) with `$t1 = 0x39`, i.e.
`A(39h) = InitHeap`. The step counts differ only because the bss-zeroing loop's length tracks each game's
bss size.

Spyro's boot, read off the trace instruction by instruction, is the shape of all six:

```
26879  sp = 0x801FFFF8        stack top 0x80200000 with the measured -8 bias
26887  a1 = 0x001FF7F8        stack top masked, minus the 0x800 reserve
26888  a1 = 0x00184DC0        ... minus the masked heap base -> the heap SIZE
26896  gp = 0x80070000        the lui half
26897  gp = 0x80075264        ... and the ori half
26898  fp = 0x801FFFF8        fp = sp
26899  ra = 0x8005B970        the jal's return address
26900  pc = 0x8005DB14        -> libcInit
26900  a0 = 0x8007AA3C        the delay slot: addi a0,a0,4 over heapBase
26901  t2 = 0x000000A0        the BIOS call vector
26903  pc = 0x000000A0, t1 = 0x39     InitHeap
```

## What this independently confirms about the framework

**`crt0_plan`'s arithmetic is right, checked against execution.** `runtime/recomp/crt0_boot.h` computes
`a0 = (heapBase & 0x1FFFFFFF | 0x80000000) + 4` and
`a1 = (mem[stackTopBase] + bias - mem[stackTopBase2]) - (heapBase & 0x1FFFFFFF)`. For Spyro that gives
`a0 = 0x8007AA3C` and `a1 = 0x001FF7F8 - 0x0007AA38 = 0x00184DC0` — and the executed registers hold
exactly those values. So the earlier fix to the InitHeap call (every port had been passing size `0`) is
confirmed by an emulator that knows nothing about our code.

**Spyro's real heap is 0x00184DC0 bytes (1,592,256 — about 1.52 MB).** This value is visible ONLY by
execution: `crt0_extract` reports Spyro's `heapSizePtr` as ABSENT, meaning this crt0 keeps the size in a
register and never stores it, so no symbolic decode of the prologue can report it. The oracle read it out
of `$a1` at the call.

## What this does NOT cover — the blind spots, named

- **`a1` (the heap SIZE) is not yet a compared field.** The oracle measures it; `crt0_extract` does not
  print its own computed value, so there is nothing to diff it against. This is the field that was
  actually wrong before, which makes it the most valuable one to add: `crt0_extract` should report the
  `a1` its own arithmetic produces, and the cross-check should compare it. Until then the agreement above
  covers `a0` but not `a1`.
- **`bssZeroLo/Hi`, `stackTopBase`, `stackTopBase2` and `heapBase` are not directly compared.** They are
  addresses the crt0 READS, not values it leaves in a register, so the boundary register file cannot
  confirm them. `bssZeroLo/Hi` are partially corroborated — the trace shows `$v0`/`$v1` loaded with
  exactly those two values in the first four instructions, and the zeroing loop's iteration count matches
  the span — but the script does not assert that yet.
- **Nothing here compares our PORT against anything.** This validates the crt0 CONSTANTS. The
  register-level differential between psxport's own execution and this reference is still ahead
  (`docs/plans/oracle-against-beetle.md`, milestone 2).
- **The reference is Beetle, not a console.** Where Beetle and real hardware disagree the console wins.
  Nothing in this window is timing-sensitive or device-dependent, which is why it is a safe first window —
  but that is a property of this window, not a general licence.

## Reproducing it

```sh
cmake -S . -B build && cmake --build build --target oracle_trace crt0_extract -j
python3 tools/oracle/crossvalidate_crt0.py <path-to-extracted-exe> --steps 900000
```

`--steps` must be large enough to get through the bss-zeroing loop; Mega Man X4 needs ~362k instructions.
If the window is too short the script REFUSES (exit 2) and says the boundary was never reached — it does
not report "0 mismatches", because zero comparisons and zero disagreements must never look alike.
