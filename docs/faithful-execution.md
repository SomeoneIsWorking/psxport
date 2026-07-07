# Faithful execution model — how pc_faithful native code achieves byte-exactness

**Standing ruling (USER 2026-07-07):** faithful strictness is non-negotiable — the strict SBS
compare is what makes recomp_path an oracle. No exemption classes, no dead-byte probing, no
compare masks under pc_skip=false. Every byte of guest RAM + scratchpad must match, including
task-stack scratch. This document derives the engineering consequences.

## The rule: faithful ports execute on guest machine state — INCLUDING the guest stack

A faithful native port of `FUN_xxxx` is not "a C++ function with the same effect on globals".
It is the same algorithm executing against the same machine state:

- **Locals live in the real guest frame.** The port descends `c->r[29]` by the RE'd frame size,
  keeps its buffers/locals at their frame offsets (filename copies, CdlFILE results, dir-parse
  scratch), and ascends on return. The frame layout comes from the Ghidra RE and is part of the
  function's contract, exactly like a struct layout.
- **ABI slots hold live values.** Where the substrate spills `ra`/`s0..s3`, the port writes the
  *actual* values of the machine it reproduces: the guest call-site return address (a named
  per-port constant derived from RE — the port's guest identity, not a tuning knob) and the
  actual register contents. Values are computed by the real control flow, never emitted as
  standalone byte-patches.
- **Loop counts emerge from control flow.** A wait loop iterates on the real `done_flag`; its
  per-iteration writes happen because the loop actually runs. No hardcoded iteration counts.

Anti-pattern (banned, being removed): `emit_fun_80044BD4_prologue`-style synthetic write blobs
at hardcoded `S0-496`-type offsets, decoupled from control flow. They can only chase the oracle;
they can never converge (c308fd6's own commit message concedes this).

## Cooperative tasks: native bodies on fibers

The substrate scheduler (RE: `FUN_80051e60` pass; task table at `0x801FE000`, stride 0x70) is
cooperative BIOS threads; the recomp side (core B) runs those bodies on host fibers
(`runtime/recomp/coro.cpp`), one resume per runnable task per frame, suspension at the yield
primitive with guest registers saved.

pc_faithful adopts the same shape with **native bodies**: a faithful task body (e.g. STAGE-0's
`startBinStageFaithful` arc) runs on a fiber owned by PcScheduler and suspends inside the ported
yield primitive. Per-frame slice cadence is then identical to B **by construction** — both sides
run one slice per runnable task per frame and park mid-body at the same semantic point with the
same guest sp.

This does NOT violate the "never route pc_skip=0 to the substrate" directive: the fiber is a
suspension mechanism (same one B uses); every instruction of the body is ported native C++. What
was banned is running the *substrate's recompiled body* and calling the result a native match.

## The scheduler primitives (ported once, wired globally)

Ported as PcScheduler methods, registered in `EngineOverrides` at their guest addresses so
substrate callers inside not-yet-ported bodies reach the SAME implementation (one implementation,
every caller — user 2026-07-07 global-dispatch directive):

- `FUN_80051F80(mode)` — yield: `task[+0x02]=mode; task[+0x00]=1;` switch to scheduler. Port:
  write the fields, spill ra at the real sp per the RE'd prologue, save guest regs, fiber-yield.
- `FUN_80051F14(slot, fn)` — spawn: entry ptr at `+0x0C`, fresh stack ptr at `+0x10`, state=2,
  `+0x6F`=0, thread create (native: arm the slot for the stanza/fiber).
- `FUN_80044BD4(fn, arg, mode, flag)` — spawn-and-wait: drain-busy yield loop; `FUN_80052010(2)`;
  clear `done_flag 0x1F80019B`; params at `0x801FE0DD/DE`; spawn slot 1; if `flag != 1`: RNG stamp
  to `task[+0x56]`, wait loop `{ if flag==2: frame counter++ + FUN_8007FD54(); yield(1) }` until
  `done_flag`. Port: same control flow on the guest frame (descent 40, spills at +16..+32).
- `FUN_80052010(slot)` — force-close; `FUN_80051FB4()` — self-close.

## What this closes (f0 anatomy, docs/findings/sbs.md 2026-07-07)

- The CdSearchFile ";1" filename locals at 0x801FE848..0x801FEA98: `LibcdNative` chain becomes
  guest-stack-resident (its locals move into the real frames its substrate counterpart used).
- The wait-loop ra spills at 0x801FE808..: produced by the ported yield running in the fiber at
  the real sp, once per real iteration.
- The A-only frame zeros (A×37 at 0x801FE818..834): disappear once A's dispatched leaves run at
  the same sp discipline inside the fiber instead of from the flat PcScheduler step.

## Verification

Strict gate unchanged: `PSXPORT_SBS_MODE=full PSXPORT_SBS_PCFAITHFUL=1` must hold zero-diff,
frame by frame, no masks. Every port lands with the SBS run that proves its bytes.
