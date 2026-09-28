---
id: 0135
title: The decomp work order — which guest code to recover so the visible bugs become debuggable
status: open
symptom: six titles show a visible defect, and for most of them the responsible guest code has no
  recovered form, so the port can only observe the symptom. The operator's directive: "decompile as much
  as guest code needed so the bugs we see can be debugged, replace black box game code with native
  readable and debuggable overrides".
tags: re,decomp,overrides,frontier,widescreen,fps60
created: 2026-09-28
updated: 2026-09-28
---

## The criterion, because "as much as needed" is otherwise unbounded

`SCUS_942.28`'s text is 103,936 words. Decompiling all of it is not a plan; it is a second project
with no debugging value at the end of it. This issue fixes the filter:

> **Recover guest code that (a) sits on the path of a defect the player can SEE, and
> (b) has no native owner yet.**

Both halves are load-bearing. (a) is what keeps the work aimed at the actual deliverable. (b) is
what keeps it out of the JIT's way: guest code that already has a hand-written native override is
already readable and debuggable, so decompiling it buys nothing. Code that runs as an opaque
translated block is exactly what (b) selects.

**The boundary, stated once so it is not re-litigated.** The decompiled C is a **reading and porting
aid**, exactly as `decomp-port` documents it: it references absolute addresses and Ghidra intrinsics
and is not buildable. What ships is a **hand-written native override that owns the recovered
behaviour**, with the JIT still executing everything else. That is the architecture this workspace
already mandates — "hand-written native overrides own recovered behavior; all other guest code runs
through an on-demand JIT" — so the operator's two halves of the directive are not in tension:
decompile to understand, then own it natively. What is **not** on the table is an offline or
install-time translation of guest code into shipping objects, or a prebuilt translated corpus. The
previous static recomp was deleted for exactly that reason and must not return in another shape.

## The work order, per visible defect

Ordered by what the player sees, and by how much of the title is behind the defect.

| # | title | the visible defect | guest code on its path | has a native owner? |
|---|---|---|---|---|
| 1 | spyro | **pool water renders as per-block colour noise**; actors, hedges, towers, buildings clean; a blue rectangle outlines the pool | `func_800258F0` (`0x800258F0`, the S_World renderer) and the field-environment path that authors the per-block water data | **no** — `grep -i water` over `game/` and `titles/` hits only two files, and **neither is a water renderer** |
| 2 | tekken3 | the picture is an authored 4:3 title card; the band is **exactly 133x16 in both legs** | the stage/gameplay scene the port never reaches | no |
| 3 | spider1 | **0 of 25,920 pixels non-black** at either aspect | `0x8008C3E0` reads `0x1F801803`, dispatches through the table at `0x80096670`, calls `*[0x800B3B18]` | partially — `GuestCdStreamCallbackLayout::readyCallbackPointer` is the declared seam, now implemented in `cd_ready_delivery.*`; still black |
| 4 | megamanx4 | faults at presented frame ~14,757 | `0x80015FE0 sw $a1,0x1F68($at)` inside `decompress_player_gfx`, one of exactly 3 instructions in 1,177,600 bytes that write `0x8010C3F0` | no |
| 5 | crash | **no frame at all** | the Lightrec budget exit at `0x800159A8` | no |
| 6 | ctr | widening invisible: **0 of 73,695 prims is 3D** | the guest's 3D submission path | no — and `AGENTS.md` bans guest ordering-table state as a native source, so this one needs the *submission* path recovered, not the table read |

**Row 1 is the one to do first**, on two independent grounds. It is the priority title, and it is the
only row where the defect is a *rendering* fault in a title that otherwise plays — the others are
stuck before or at presentation.

## Row 1 is also the clearest case of why decompiling helps, rather than a guess

`AGENTS.md` requires native producers to consume **pre-GTE game state**, and forbids shipping
presentation that executes partial guest rendering. So the pool water is drawn by a **native
producer reading guest-authored per-block state whose layout is currently a black box.** A per-block
*colour* fault with clean actors and clean buildings is a layout or stride error in exactly that
read, not a shading bug.

That makes the decompilation directly load-bearing rather than decorative: **recovering the guest
function that authors the per-block water data yields the readable field layout, and the layout is
what the native override is misreading.** This is the "debuggable" half of the operator's ask in the
most literal sense — the defect is unfixable-by-inspection until the layout is named.

**The instrument for attributing it already exists and is already wired.** `core.rsub.census.report()`
in `game/core/runtime_run.cpp` answers "which producer owns the thing I am looking at", and
`report()` **refuses loudly** when it was never fed, so a zero cannot read as "no producer drew
anything". It was added for precisely this frame and has, as yet, been **observed to print nothing at
all** on a rebuilt run. **Establishing whether the census fires is step zero, and it is cheaper than
any decompilation.**

### Step zero, narrowed — and the first hypothesis was wrong, which is the useful part

The natural explanation is that the output is filtered. **It is not.** In
`vendor/lucent/include/lucent/log.h`, **only `debug()` is channel-gated**; `info`, `warn` and `error`
call `log()` directly, and `log()` itself (`vendor/lucent/src/log.cpp:246`) has **no level filter at
all** — it writes to the sink or `stdout` unconditionally. An `info` line is not suppressible by
configuration, so "the level is wrong" cannot explain the silence and would have been the wrong thing
to go and change.

That leaves two causes worth separating, both of which are cheap to tell apart:

1. **The run never reached the report.** `RuntimeRun::shouldEnd()` (`game/core/runtime_run.h:20`) ends
   a run cleanly on `endRequested_ || fields_ >= fieldLimit_`, and `fieldLimit_` comes from
   `cfg_int("PSXPORT_NATIVE_FRAMES", 0)`. So **a capped run exits the loop and does report**, while a
   run ended by a **signal** — harness timeout, or a `kill` by the driving tool — never reaches
   `main.cpp:101` at all.
2. **The binary predates the call site**, which prints nothing identically.

**So the census is armed, wired, unfilterable, and silent on exactly the runs that get killed — which
is what a probe does.** Its deliberately loud "never fed" refusal is lost by the same kill. **An
instrument that only reports on a clean exit is an instrument that reports nothing during a probe**,
and that is a defect in its own right, independent of this bug: `report()` should also run on the
termination paths, or the driving tools should stop killing and start requesting an end.

The distinguishing observation needs no decompilation and no new tool: a run that reaches its own
`PSXPORT_NATIVE_FRAMES` cap must print a `[producers:*]` line, and a run killed at the same point must
not. A cap-reached run that is silent confirms cause 2.

## What exists, and what must be stood up

- **Present:** `psxport/tools/ghidra_decomp.py`, a Ghidra headless post-script, and Ghidra
  **12.0.4** at `~/dev/ghidra_12.0.4_PUBLIC` (`analyzeHeadless` on PATH).
- **Absent:** no analyzed project, no decomp output on disk, and **no decomp tooling in `spyro` at
  all**. The capability has never been stood up.
- **A gotcha already paid for, which must not be rediscovered:** Ghidra's non-returning-function
  analyzer mislabels ordinary leaf helpers on a PSX RAM dump, and every caller then decompiles to
  `/* WARNING: Subroutine does not return */` with the whole body after the call discarded and a
  fabricated `return 0` — **output that reads like a complete function and is not one.** The tool
  clears the flag (`PSXPORT_CLEAR_NORETURN=all`); any new pipeline must keep that and must assert
  against it, because this is the workspace's signature failure mode: **instruments producing
  confident wrong answers rather than absences.**
- **One image-specific trap, from this session:** `llvm-objdump --triple=mips` **silently
  misdecodes the MMX4 image**. Never Ghidra *or* objdump-with-that-triple for `SLUS_005.61`.

## Blocked on, right now: memory, not method

This machine has **16 cores and ~2.2 GB available**, with four agents building concurrently and load
average 16.2. A Ghidra auto-analysis of a 2 MB RAM dump costs 1–2 GB. Launching it now would
plausibly OOM-kill another agent's build, and a build killed by someone else's memory spike gets
reported as a **false red**. The order is therefore: stand the pipeline up when the build agents
drain, not before.

## Steps, in order, with the falsifier for each

1. **Prove the census fires, and make it fire on killed runs too.** A run that reaches its own
   `PSXPORT_NATIVE_FRAMES` cap must print a `[producers:*]` line. Then fix the signal case, because a
   diagnostic whose loudest refusal is unreachable during a probe is not a diagnostic.
   *Falsifier:* silence on a cap-reached run — that isolates cause 2 (a stale binary) instead, which is
   a different fix.
2. **Stand up the spyro decomp pipeline** as a repo tool: import the admitted `SCUS_942.28` at the
   right base, auto-analyze, emit a function inventory, and decompile a named target set to C into a
   reviewable location. Assert against the no-return truncation, and assert against a known-good
   function that the body is present.
3. **Recover the per-block water layout** from the field-environment authoring function, and name
   every field it reads. The control is the existing `tests/test_world_animation.cpp`, which already
   pins the four animation channels' field order and offsets.
4. **Fix the native producer's read** against the recovered layout — or, if the layout says the
   producer is right, that is a finding and the next probe is named instead.
5. **Then rows 2–6**, each with its own control, one at a time.

## Not claimed

That decompiling any of this will fix any of it. Decompilation makes a defect **debuggable**; it does
not make it **correct**. Rows 1–6 are six different frontiers — a data layout, an unreached scene, a
CD delivery path, an authored-array overflow, a budget exit, and a missing submission primitive — and
only the first is a strong candidate for "read the layout, fix the read". If step 3 shows the layout
matches what the producer reads, that is a real result and the next probe is named, not a failure.
