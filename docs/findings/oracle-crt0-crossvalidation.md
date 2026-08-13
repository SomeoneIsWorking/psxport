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

## The result: 49 comparisons across 7 images, 49 agree, 0 disagree

`tools/oracle/crossvalidate_crt0.py <exe>` runs both methods and diffs them field by field IN CODE — not
by hand, because hand-comparison is how a session reports agreement it never checked.

Seven fields per image: `gp`, the `jal` target (`libcInit`), the BIOS function number (`$t1`), `InitHeap`
`a0`, and `crt0_plan`'s computed `sp`, `a0` and `a1`.

| image | what it is | left text at step | `gp` | `libcInit` | `sp` | `a0` | `a1` (heap size) |
|---|---|---|---|---|---|---|---|
| `SCUS_942.28` | Spyro (the game) | 26,903 | `0x80075264` | `0x8005DB14` | `0x801FFFF8` | `0x8007AA3C` | `0x184DC0` |
| `MAIN.EXE` | **Tomba! 2 (the game)** | 20,193 | `0x800BE0D4` | `0x80089860` | `0x801FFFF8` | `0x8010622C` | `0xF99D0` |
| `SCUS_944.54` | Tomba! 2's boot STUB | 20,193 | `0x80038458` | `0x80018CEC` | `0x801FFFF8` | `0x8003C35C` | `0x1C38A0` |
| `SLUS_008.75` | Spider-Man | 85,873 | `0x800B47F4` | `0x8008DC98` | `0x807FFFF8` | `0x800C65D8` | `0x731A24` |
| `SLUS_010.40` | Vagrant Story | 65,053 | `0x80033674` | `0x80026864` | `0x801FFFF8` | `0x800401AC` | `0x1BBE50` |
| `SLUS_008.93` | Toy Story 2 | 246,534 | `0x800A0CD8` | `0x80089344` | `0x80200000` | `0x800D12C4` | `0x126D40` |
| `SLUS_005.61` | Mega Man X4 | 361,994 | `0x8012F418` | `0x800EDCDC` | `0x80200000` | `0x80175F3C` | `0x820C8` |

Every column is BOTH methods' value — they are identical, which is the finding. `$t1 = 0x39` for all seven
and is omitted from the table for that reason.

**`SCUS_944.54` is Tomba! 2's boot STUB; the game is `MAIN.EXE`, which the stub `LoadExec`s.** Both are
listed because both have a crt0 and both were checked, but only `MAIN.EXE`'s numbers describe the port —
they are not interchangeable, and the two disagree exactly where you would expect two different programs to
(heap `0xF99D0` vs `0x1C38A0`).

**All seven leave the program's own text at `pc = 0x000000A0`** — the BIOS A-function call vector — with
`$t1 = 0x39`, i.e. `A(39h) = InitHeap`. The step counts differ only because the bss-zeroing loop's length
tracks each program's bss size.

Spyro's boot, read off the trace instruction by instruction, is the shape all seven share:

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

**Spyro's real heap is 0x00184DC0 bytes (1,592,768 — about 1.52 MB), and the heap size is now a COMPARED
field.** It was not, at first: `crt0_extract` printed the scanned boot group but not the `a1` its own
arithmetic derives from it, so the oracle's measurement had nothing to be diffed against. `crt0_extract`
now calls the shipping `crt0_plan` — calls it, not reimplements it, for the same reason both already share
`crt0_scan` — and prints `sp`, `a0` and `a1` with the derivation spelled out. That closed the gap, and all
seven images agree on `a1`.

`crt0_boot.h` cites "Tomba!2 ~0xF9xxx" in a comment; `MAIN.EXE` measures `0xF99D0`, so that comment is
CORRECT. It looked wrong only while the stub was being mistaken for the game.

**Which games have heap globals at all**, measured: Spyro (`0x800730C4`), Tomba! 2 (`0x800ABEF8`),
Spider-Man (`0x800B1240`) and Vagrant Story (`0x80030FB8`) store the heap size to an absolute address;
Toy Story 2 and Mega Man X4 store nothing (`heapSizePtr == 0`, genuinely ABSENT — they keep it in a
register only). So the ABSENT case is real and affects two of six games, which is why `crt0_plan`
distinguishes ABSENT from UNSET with an explicit flag.

## Spider-Man's 8 MB stack pointer is CORRECT — and chasing it found a bug in the oracle

`SLUS_008.75` is the only image whose `sp` is not in the low 2 MB: `0x807FFFF8`, from a stack-top global at
`0x800B3E70` holding `0x00800000`. That global sits BELOW the bss span (`[0x800B5994, 0x800C65D4)`), so it
is initialised data read straight out of the image — nothing patches it at runtime, and the crt0 really does
build that pointer. Its heap is `0x731A24` (7.2 MB) on the same reasoning.

**It is nonetheless right, because main RAM is MIRRORED.** The reference's own decoder is
`if (A < 0x00800000) ... MASMEM_*(MainRAM, A & 0x1FFFFF)` (`libretro.c:1085-1108`): the region test is 8 MB
wide and the offset wraps at 2 MB, so the 2 MB of physical RAM appears FOUR TIMES across
`0x80000000-0x807FFFFF`. `0x807FFFF8` is the top of RAM through the fourth mirror. Spider-Man's config is
fine and so is `spider1`'s `game_config.cpp`; nothing needs changing there.

**What did need changing was this oracle.** `oracle_shim.c` tested `phys < 0x00200000` and would have
reported every Spider-Man stack access as a HARDWARE access, ending the window at a boundary that does not
exist. It was also INCONSISTENT with its own `oracle_init`, which mirrors 4× because that loop was copied
from `libretro.c` — so instruction FETCH was correct while the data path was not, and only a game with a
high stack would ever have revealed it. Fixed to match the reference exactly (`RAM_WINDOW` /
`RAM_MASK`), with `oracle_spike` gaining a mirroring class: it stores through `0x807FFFF8`, reads it back,
and checks the same physical word is visible at `0x801FFFF8` — the last part matters, because a shim that
gave the high address its own separate storage would pass a read-back-only test.
`prove_spike_can_fail.sh` gained a matching mutation, which 3 checks catch.

The lesson is about the METHOD, not the address: a question about the guest turned out to be a defect in
the instrument, and only a real executable with an unusual value could surface it. The synthetic fixtures
all use low stacks.

## What this does NOT cover — the blind spots, named

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

## Why the agreement is not vacuous

A cross-check that accidentally read both numbers out of the SAME source would report perfect agreement on
every game forever and look exactly like success. `tools/oracle/prove_crossvalidate_discriminates.py`
rules that out: it feeds game A's symbolic decode against game B's executed boundary and requires every
game-specific field to DISAGREE. Measured on Spyro vs Spider-Man — 5 of 6 fields discriminate, and the
sixth is `$t1 = 0x39`, which is asserted to be the SAME because every PS-X crt0 makes that same call.

That script has three outcomes per field, not two, and the reason is measured: `crt0_plan sp` is
`0x801FFFF8` for both Spyro and Tomba! 2, so that PAIR cannot test it — while across the corpus `sp` takes
three distinct values, so it does discriminate in general. A coincidental collision is reported
INCONCLUSIVE (exit 2, "not a pass") rather than FAIL, because a false alarm trains everyone to ignore the
check.

## Reproducing it

```sh
cmake -S . -B build && cmake --build build --target oracle_trace crt0_extract -j
python3 tools/oracle/crossvalidate_crt0.py <path-to-extracted-exe> --steps 900000
python3 tools/oracle/prove_crossvalidate_discriminates.py <exe-A> <exe-B>   # validate the checker itself
```

`--steps` must be large enough to get through the bss-zeroing loop; Mega Man X4 needs ~362k instructions.
If the window is too short the script REFUSES (exit 2) and says the boundary was never reached — it does
not report "0 mismatches", because zero comparisons and zero disagreements must never look alike.
