# The oracle must be an INDEPENDENT emulator, not our own faithful path

> USER, 2026-08-12: *"oracle compare should be done against a verified emulator like beetle imo, not
> our unverified 'faithful' path"*

## The defect in what exists today

`PSXPORT_SBS_MODE=full` compares **core A = pc_faithful** against **core B = recomp_path**. Both are
OURS. `Tomba2Engine/CLAUDE.md` even calls B "the oracle" and forbids allowlists, which makes the
discipline around it sound — but the reference itself is unverified code, so:

- **A shared wrong assumption reads as SUCCESS.** If the recompiler and the hand-written faithful port
  mistranslate the same instruction the same way — likely, since both were written from the same
  reading of the same disassembly — the compare says "identical" and certifies the bug.
- It is the architecture-level form of the failure this workspace keeps hitting: **an instrument that
  cannot show the other answer.** Nothing in the loop was ever independently right.

The harness is still worth keeping: it is byte-exact, lockstep, refuses allowlists, and its
`PSXPORT_SBS_CANARY` self-test genuinely works — MEASURED 2026-08-12, canary at f60 flipped one byte on
core A and the compare tripped on that exact address and frame, having reported identical at f0 and f30.
So the *mechanism* is sound and only the *reference* is wrong. This plan replaces the reference.

## Feasibility: MEASURED, not assumed (2026-08-12)

- The complete Mednafen PSX core is vendored: `cpu.c` (3,765 lines), `dma.c`, `irq.c`, `timer.c`,
  `cdc.c` (3,201), `gpu.c` + its polygon/line/sprite units, `sio.c`, `frontio.c`, `spu.c`, `gte.c`,
  `mdec.c`. `vendor/beetle-psx/libretro.c` is the system glue and owns `MainRAM`.
- **THE CORE BUILDS. Measured by compiling it, not by reading it:** with
  `-Ivendor/beetle-psx{,/mednafen,/mednafen/psx,/include,/libretro-common/include,/deps/libchdr/include}`,
  **10 of the 15 `.c` files compile to objects**, `cpu.o` among them (73,512 bytes, 16 exported
  functions). The five that do not are explained and none is a real obstacle: `gpu_polygon.c`,
  `gpu_sprite.c` and `gpu_line.c` are `#include`d INTO `gpu.c` (lines 64-66) and were never separate
  translation units; `gte.c` needs psxport's own `runtime/recomp/gte_state.h` on the path, which is why
  psxport already builds it; only `gpu.c` itself has a genuine missing declaration to resolve, and the
  GPU is not needed for milestones 1-3 below.
- **COMPILING IS NOT WORKING.** This settles the toolchain question and nothing else: no object here has
  been linked, initialised, or stepped for a single instruction.
- A BIOS exists and is not a licensing problem: `bios/openbios-fast.bin` plus
  `vendor/beetle-psx/deps/openbios/openbios.bin` (OpenBIOS, open source). No retail Sony image needed.
- Beetle is a **committed GPL-2 fork** on the submodule's `psxport` branch (`patches/beetle-psx/README.md`),
  already carrying `#ifdef PSXPORT_HOOKS` taps in `cpu.c` (per-instruction PC hook + executed-PC coverage
  bitmap), `cdc.c`, `gte.c`, `gpu.c`. The per-instruction PC hook is most of what an oracle needs and it
  is already there — it was built for the RETIRED architecture where psxport hooked into Beetle
  (`runtime/psxport_hooks.*`, now absent), so the hooks compile out today and the consumers are gone.

## THE HARD PART, and it is not the emulator

**Our port HLEs the BIOS; Beetle EXECUTES it.** `runtime/recomp/hle.cpp` implements the A0/B0/C0 calls
natively (`InitHeap`, `malloc`, the libc leaves, the CD callbacks), while Beetle jumps into real BIOS
code that writes kernel structures, its own scratch, and the exception vectors. A naive whole-RAM compare
therefore diverges at the first BIOS call for a reason that is CORRECT in both programs.

Three ways out, in preference order:

1. **Sync at the game's entry and compare only while GAME code is executing.** Inject the extracted
   executable into Beetle's `MainRAM` at `t_addr` and set `pc`/`gp`/`sp` exactly as `crt0_plan` does —
   which is what our port already does, so neither side executes a BIOS boot at all. Then step both and
   compare. At a `jal` into the BIOS thunk region, STOP comparing, let each side service it its own way,
   and resume at the return address, comparing the RESULT (return register + any buffer the call is
   documented to fill). This tests exactly what we want tested — our translation of GAME instructions —
   and it turns every HLE into an explicitly-scoped, individually-checkable contract instead of a
   silent difference. `crt0_verify.h` already proves we can locate the thunks (`libcInit` is measured as
   the `A(39h)` thunk in all six executables).
2. **Compare registers per instruction rather than RAM per frame.** Beetle's `psxport_on_pc` hook makes
   this cheap to try and localises a fault to one instruction instead of one frame. Best first
   milestone: it needs no CD, no GPU, no timers to be meaningful over a straight-line run.
3. **Let Beetle run the real boot and compare only the game's own data regions**, BIOS-owned pages
   excluded. Listed for completeness and NOT recommended — an exclusion list is the allowlist the
   current harness rightly forbids, and it would grow every time it was inconvenient.

## DETERMINISM IS A PRECONDITION, and this plan originally omitted it

A diff only means something if both sides are reproducible, and identical starting RAM is NOT enough.
Three sources of drift, each of which would make the oracle lie rather than fail:

- **Free-running counters.** Beetle advances root counters and the CPU cycle count on real hardware
  timing (`timer.c`, the event scheduler in `psx_events.h`); our runtime has its own notion of a frame.
  Any game code that reads a timer, waits on a counter, or spins on VSync will legitimately differ. So
  the compare must either run in a window that touches none of them (milestone 2's straight-line window)
  or drive both from ONE synthetic clock. Do not discover this at milestone 4.
- **Input.** Pinned and scripted on both sides, never live. The SBS harness already has
  `PSXPORT_SBS_PAD_REPLAY` for the mirrored-lockstep case, with a recorded caveat that a frame-indexed
  capture lands inputs at the wrong moments when `pc_skip` differs — the same trap applies here.
- **Uninitialised memory.** Both sides must start from a bit-identical image, which is what injecting
  the executable buys. Mednafen also has a savestate layer (`mednafen/state.h`, `state_helpers.h`)
  usable to freeze a reached point instead of re-deriving it — worth using once a window past boot is
  interesting.

**On a residual list:** the general recomp-harness discipline allows one, provided every entry records
WHY it is benign. `Tomba2Engine/CLAUDE.md` is STRICTER — no allowlist, no residual list, every diff
fatal — and that stricter rule WINS here, because this project's failures have come from exclusions
that grew. The correct way to handle a free-running counter is therefore to REMOVE the divergence by
sharing a clock, not to list the address as expected.

## Milestone 1 is DONE — measured 2026-08-13

`tools/oracle/` builds `psxport_oracle` (a static library) and `oracle_spike`, both wired into CMake and
into `ctest`. What was actually established, by running it:

- **The vendored Mednafen CPU executes MIPS we inject, with no `libretro.c` at all.** The spike loads 8
  hand-assembled instructions at `0x80010000`, runs a 200-cycle window, and every result matches a value
  derived by hand in the fixture's own comments: `$t0 = 0x12345678` from `lui`+`ori`, `$t2 = 0x123456DC`
  from `addu`, `$t3` round-tripped through a `sw`/`lw` pair, the stored word visible in main RAM itself,
  and PC advanced 160 bytes while staying inside RAM. 12 of 12 checks pass.
- **The glue surface is 15 symbols, not 53.** Measured by linking rather than by reading: a CPU that only
  has to STEP needs `cpu.c`, `gte.c` and the six PGXP translation units, and those leave undefined only
  `ScratchRAM`, the eight `PSX_MemRead/Write*`, `PSX_EventHandler`, `psx_gte_overclock`,
  `MDFNSS_StateAction`, `widescreen_hack`, `widescreen_hack_aspect_ratio_setting`. The CD, GPU, DMA,
  timer, SIO and filestream layers are not in the stepping path at all. `gte.c` compiles once
  `runtime/recomp` is on the include path, exactly as the feasibility section predicted.
- **A hardware access is REPORTED, not absorbed.** The spike's second program reads GPUSTAT at
  `0x1F801814`; the run must come back `ORACLE_STOP_HARDWARE` naming that address. A shim that returned 0
  for device reads would report a clean window instead, and milestone 2 would then compare instructions
  nobody executed.
- **The spike has shown BOTH answers.** `tools/oracle/prove_spike_can_fail.sh` (ctest:
  `oracle_spike_discriminates`) rebuilds the shim with its FastMap population loop disabled — in a
  throwaway copy under `scratch/`, never the shipping file — and requires the spike to FAIL: 9 of 12
  checks fail there. Without that, 12/12 would have meant nothing.

Three things milestone 1 taught that were NOT in the design:

1. **Instruction fetch bypasses `PSX_MemRead32` entirely.** `cpu.c` reads opcodes straight out of
   `FastMap` (lines 794, 810), so a core with an unpopulated FastMap fetches from `DummyPage`, executes
   zeros, takes a bus error and lands at `0xBFC00180` — while still reporting a clean cycle-budget stop.
   Correct memory callbacks are not enough; the mirrors from `libretro.c:2720-2725` must be replicated.
2. **`cpu.c:111` is `PS_CPU *PSX_CPU = &s_cpu;` — non-NULL before anything initialises.** An `if (PSX_CPU)
   return 1;` idempotence guard therefore reported successful init having allocated no RAM, and the first
   `memcpy` into main RAM segfaulted. Lifecycle state must be owned by whoever owns the lifecycle.
3. **`PSX_EventHandler` returning false is the documented way to end a timeslice**, not a stub pretending
   to work: `CPU_Run`'s outer loop is `do { ... } while (PSX_EventHandler(timestamp))`. Combined with a
   `PSX_SetEventNT` that refuses to schedule anything, that makes "no counter influenced this window" a
   checked property rather than an assumption — which is the DETERMINISM section's requirement, satisfied.

**Single-stepping works and is EXACT, which is milestone 2's substrate.** `oracle_step()` advances the
core by a 1-cycle budget with the timestamp carried forward, and the spike requires a step-by-step trace of
the fixture to land exactly where one bulk run landed: 40 steps, 40 distinct PCs, identical registers and
final PC. No vendor patch was needed — `cpu.c`'s `PSXPORT_HOOKS` per-instruction hook belongs to the
retired architecture and its `psxport_hooks.h` no longer exists in this tree, so "use the existing hook"
would have meant reviving a dead surface. The oracle owns the run loop, so it does not need one.

The clock has to be carried, not reset: the core keeps cycle-relative deadlines (`gte_ts_done`,
`muldiv_ts_done`, the load-absorb counters) as absolute values against its own timestamp. That is not an
argument, it is a MEASURED mutation — `prove_spike_can_fail.sh` builds a variant whose slice restarts the
clock at 0, and the stepped trace ends at `0x80010C84` instead of `0x800100A0`. One check catches it, the
stepping-vs-bulk PC comparison, and nothing else does.

**What milestone 1 still does not prove: anything about the port.** No comparison has been run. The spike
says so in its own output.

### The collision milestone 2 has to solve deliberately

`libpsxport` already compiles `gte.c` for its own GTE backend, so linking both archives into one
executable hits duplicate symbols — and worse, would have the reference and the thing being tested sharing
state, which destroys the independence that is the whole point. Two honest options, to be chosen rather
than stumbled into: run the oracle in a **separate process** and compare over a pipe (which also isolates
determinism), or **prefix the archive's symbols** (`objcopy --prefix-symbols=oracle_`).

## Order of work

1. ~~**Spike:** build the mednafen core into a `psxport_oracle` static library, no game, no port. Prove it
   steps N instructions from an injected executable and can read `MainRAM`. Nothing else.~~ **DONE, above.**
2. **Register-level differential** over a straight-line window from the game entry (option 2), driven by
   `oracle_step()` — NOT by "the existing PC hook", which no longer has a header in this tree. First real
   result: does our interpreter agree with Beetle instruction for instruction before any BIOS call?
   Two things are already known about where that window ends. `crt0_extract` reports Spyro's prologue as
   35 instructions from `0x8005B8E0`, stopping on `jal (libcInit)` — the `A(39h)` `InitHeap` thunk at
   `0x8005DB14`. And the oracle maps NO BIOS, so the thunk's jump into the kernel jump table at `0xA0`
   lands in zeroed RAM. That is not a defect to patch around: it is precisely the boundary milestone 3
   exists to model, reached by measurement instead of by assumption.
3. **BIOS-call boundary** (option 1): detect the thunk, suspend the compare, resume at the return, and
   assert on the documented result. Each HLE becomes its own case.
4. **Then** extend to RAM-per-frame with the CD path in, which is where `cdc.c`'s instant-read tap and
   the pacing differences start to matter.
5. **Retire `pc_faithful` as "the oracle"** in `Tomba2Engine/CLAUDE.md` once Beetle is the reference.
   Keep the SBS harness, its canary, and its no-allowlist rule — swap what B is.

## What must NOT be claimed along the way

- Beetle is *more* verified than our code, not *correct*. Where Beetle and a real console disagree the
  console wins, and Beetle has its own bugs (the current doc already says recomp bugs are rare-but-real;
  the same caution now applies to the new reference).
- OpenBIOS is NOT the retail BIOS. If a game behaves differently under it, that is a difference between
  OpenBIOS and Sony's, not evidence about our port. Since option 1 executes NO BIOS on either side, this
  is mostly designed out — say so rather than relying on it silently.
- Until step 2 produces a real disagreement or a real agreement over a non-trivial window, this plan has
  proven nothing about the port. A compiling oracle library is not a verified port.
