---
id: 141
title: A runaway shadowed native call grows the side-effect journal without bound until the host runs out of memory
status: fixed
symptom: a Spyro override gate ran 16 minutes on a 15-second route, reached 7.4 GB peak RSS, and took the machine's memory reaper with it
tags: override-differential, memory, diagnostics
created: 2026-09-30
updated: 2026-09-30
---

## What happens

A swarm worker's candidate native override for Spyro 1 `update_player_frame` (0x8003FE40) loops
forever inside one call. Under the override differential (`PSXPORT_OVERRIDE_DIFF`, every call
shadowed), each pass stores to a device address, and `SideEffectJournal::admitDeviceWrite` appends a
`SideEffect` to `effects_`, a `std::vector` with no bound. Host memory grows until the process dies.

Measured 2026-09-30 on the same 300-frame gameplay drive (`tools/drive.py gameplay --hold RIGHT
--hold-frames 300`):

| leg | peak RSS | wall |
|---|---|---|
| no differential | 184,340 KB | 15.4 s |
| differential on a well-behaved override (`camera_rotation_from_sphere`, 54 of 54 calls shadowed) | 186,376 KB | 15.2 s |
| differential on the runaway override, address space capped at 3 GB | grew ~6 MB/s after call 1's mismatch; `std::bad_alloc` in `SideEffectJournal::append` <- `admitDeviceWrite` <- `Core::writeGuestMemory<unsigned int>` <- the native body <- `OverrideDifferential::shadow` | killed at cap |

Uncapped, the same gate reached 7.4 GB peak (4.1 GB resident) after 16 minutes. Eight such gates in
a swarm batch explain every memory reap of 2026-09-29/30.

## Cause

The journal has no size limit, and nothing bounds how long one shadowed native call may run. A
diagnostic that exists to catch a wrong override therefore converts the most wrong kind of override,
one that never returns, into an unbounded host allocation instead of a verdict.

## Fix (acceptance)

- The journal refuses to grow past a stated bound (effects per call, a named constant sized well
  above any real call: measure the largest real call on the corpus and state it), and the call then
  fails fast with a named fault that identifies the override, its address and the bound. It must not
  wrap silently or drop entries: the verdict is a failure, not an incomparable skip.
- A falsifier test: a native function that stores to a device address in an endless loop, run under
  the differential, terminates with that fault, and the test's peak RSS (VmHWM from
  /proc/self/status) stays under a stated bound (for example 64 MB above its start).
- A negative: a real call with many effects under the bound still passes, with its journal intact.
- The product's peak RSS over the 300-frame gameplay drive with the differential armed stays within
  a few MB of the unarmed run (the table above is the baseline).

## What was done

`psx::cpu::kMaxSideEffectsPerCall = 1u << 20` (1,048,576) in `runtime/cpu/side_effect_journal.h`, and the
four acceptance items below.

### The bound, and the measurement it is sized from

MEASURED over the differential's own per-call log line (`"N side effect(s) replayed"`) as it survives in
the port repositories' run artifacts, across **five titles**: 6,574 completed differential reports
covering 166,098 sampled calls over 132 distinct real override names, of which the per-call effect
count was recorded for 1,011 real shadowed calls. `p50 = 0`, `p99 = 94`, **`max = 145`** (Spyro 1
`update_active_voices @0x8005637C`). The largest log LENGTH any real report names is 1; the largest
side-effect INDEX a real mismatch names is `#12`, a lower bound of 13. The framework's own hermetic
corpus adds 18 sampled calls, max 4,097 — `tests/test_override_differential.cpp`'s deliberate
many-effect case, a 4096-pass I_STAT-ack loop, and the largest effect count this repository produces
on purpose.

1,048,576 is therefore **256x the largest effect count measured in any real call in this workspace**,
7,223x the framework's own largest deliberate case, and **2x the largest movement one function can
physically make** — the machine's whole 2 MiB of VRAM in 4-byte transfers, 524,288 words. That last
figure is a ceiling rather than an estimate: a PSX function cannot journal more effects than it moves,
so no real override can pass it whatever the corpus happens to contain today. `SideEffect` is 16 bytes,
so one journal is capped at 16 MiB and one shadowed call — which holds the live journal and the shadow
journal at once — at 32 MiB.

**The claim is checkable rather than a comment nobody can test.** `OverrideDifferential`'s per-key
summary now prints the largest sampled call against the bound on every real run
(`summary <name> @0x…: largest sampled call journaled N effect(s) (call M of K sampled) against the
1048576-effect per-call bound`), and the JSON report carries `largest_sampled_call_effects`,
`largest_sampled_call_number`, `effects_bound`, `journal_bound_faults` and `first_journal_bound`. A key
that was never sampled says `NOT MEASURED, 0 call(s) were sampled` rather than reporting a largest
count of 0, because a key whose calls journaled nothing and a key that was never sampled both report
0 and only one of them is a measurement.

### Fail fast, naming the override, its address and the bound

`SideEffectJournal::refuseAtBound`, in three ordered rules, and the order is the design:

1. **REFUSE, never wrap and never store.** The refused effect is left out, so `effects_` holds exactly
   the first `kMaxSideEffectsPerCall` effects and the positions the replay compares by stay the ones the
   call really produced. Storing a clamped or wrapped entry instead would produce a log that compares
   EQUAL to a truncated one — the silent-skip failure this issue forbids.
2. **RECORD.** `JournalBoundViolation` names the path (`original` / `native`, which are different
   findings for whoever reads the gate), the offending effect with its device address and width, the
   count journaled, the count refused after the bound, and whether the call was stopped.
3. **THEN STOP THE CALL, but only out of host code.** The shadow-path overrun inside a native body
   raises `JournalBoundExceeded`, which `OverrideDifferential::shadow` catches.

**The unwind guard is the part that is not obvious.** A `throw` from a device access is only legal while
every frame below is host code, and the journal cannot know that from inside itself: `Core::mem_w*` is
reached *directly* from Lightrec-generated code, so "am I in host code" is a fact about the caller. So
`SideEffectJournal::TranslatedExecutionScope` is opened by `LightrecExecutor::executeWithBoundary` —
the one place `execute`, `executeUntilExit` and `executeFunction` all funnel through — and
`refuseAtBound` refuses to raise while it is open. That is also why the LIVE path never unwinds at
all: the original's device traffic comes from translated guest code, so a throw there would cross JIT
frames on every overrun rather than only on the rare one. The live path's growth is already bounded by
the executor's cycle budget, so it terminates through the ordinary bounded-exit path and the recorded
violation is reported when it does. **The fault is recorded either way**, so a call that could not be
stopped is still reported.

**The verdict is a MISMATCH, and the header says why.** `Incomparable` is tolerated when other calls of
the same key compare, so a runaway reported that way would look like a call with nothing to compare —
exactly the wrong answer for the wrong override. A mismatch fails `failuresOf`, fails
`tools/port/override_differential_gate.py`, and is additionally reported in its own right with the full
violation sentence, so a reader is never told "mismatched" when the truth is "one path could not be
journaled at all".

The native's state is discarded rather than compared: it was stopped mid-loop, so judging it would be
comparing a partial body to a complete one. The original's state is restored and the run continues from
the original's result, which is the property every other outcome has.

### The falsifier, and that it was observed to fail

`a_native_that_loops_forever_storing_to_a_device_terminates_at_the_journal_bound`: an endless
device-storing native under the differential. It asserts the call terminates; the verdict is
`mismatch` and explicitly **not** `incomparable`; the violation is on the `Replay` path, is
`stoppedCall`, has `effects == kMaxSideEffectsPerCall`, `refused == 1`, and an offending
`DeviceWrite @0x1F801074`; the description names the path, the bound and "not a comparison"; the
override's name and address are in the verdict; the fault is in the JSON report as well as the log; the
run ends on the original's `v0` and RAM result with `i_mask` still 0.

**Peak RSS** is `VmHWM` from `/proc/self/status`, read before and after, with the ceiling **192 MiB of
growth** — stated against the bound rather than picked: 8x the 16 MiB a full shadow journal costs, plus
the snapshots. A machine without `/proc` returns nullopt and the case fails loudly, because a reader
that got 0 there would compare 0 against a ceiling and call the runaway bounded.

**Observed to fail correctly:** with the bound check removed, this exact case terminates in
`std::bad_alloc` from `SideEffectJournal::append` under a 3 GB address-space cap — the incident,
reproduced, in the same test.

**The negative:** `a_real_call_with_many_effects_under_the_bound_still_matches_with_its_journal_intact`
— a real bounded guest loop of 4,096 I_STAT acks, 4,097 effects, matching, journal intact, and
`journal_bound_faults == 0`. It asserts the effect COUNT, not just the verdict, because a `match` is
also what a pair of empty logs produces and a funnel that had stopped journaling would report this green.
**Its discriminator was also run:** with the bound lowered to 1,024 this case turns red, so it measures
the bound rather than passing whatever the constant happens to be.

Two defects the fixtures hit, both recorded in place, because both produced convincing wrong answers:
a MIPS branch displacement measured from the wrong address (the back-edge pointed at itself and the run
spent its whole budget in a loop that never exited), and a many-effect loop writing I_MASK, which sets
`PW_IRQ` and made the call come out `incomparable` with "original path serviced asynchronous pending
work" — a real answer about a real hazard, and no evidence at all about the bound.

### Not done here

The fourth acceptance item — the product's peak RSS over the 300-frame gameplay drive with the
differential armed — is measured in the port repository against a provisioned image, not in this
framework worktree, and it is not claimed. The framework-side evidence for it is the 32 MiB per-call
cap and the 16 MiB a single journal can reach, which is what the armed leg's growth is now bounded by.
`docs/codemap.md`'s override-differential row now names the journal as the owner of the bound, its
refusal record and its unwind guard.

## Verified in the port (2026-09-30)

The incident's own candidate (`update_player_frame` @0x8003FE40, swarm job pete-artisans-walk-2),
rebuilt against this fix and driven over the same 300-frame gameplay route with every call shadowed:
24.75 s wall, 216,324 KB peak RSS (was 7.4 GB and a 16-minute hang), 220 of 220 calls reported
`mismatch`, 3 of them at the bound, and the summary line names the largest call and the bound. The
+30 MB over the 186 MB armed baseline is the documented 32 MiB per-call cap.

