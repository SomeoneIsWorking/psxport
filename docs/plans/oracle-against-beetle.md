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
- `mednafen/psx/cpu.c` **compiles clean** against the vendored include paths — 0 errors. The others need
  only `-Ivendor/beetle-psx/libretro-common/include` for `boolean.h`; psxport ALREADY builds `gte.c`,
  `mdec.c` and `spu.c` this way, so the toolchain question is settled.
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

## Order of work

1. **Spike:** build the mednafen core into a `psxport_oracle` static library, no game, no port. Prove it
   steps N instructions from an injected executable and can read `MainRAM`. Nothing else.
2. **Register-level differential** over a straight-line window from the game entry (option 2), driven by
   the existing PC hook. First real result: does our interpreter agree with Beetle instruction for
   instruction before any BIOS call?
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
