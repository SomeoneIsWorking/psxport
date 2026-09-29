---
id: 0138
title: The per-function override differential — the gate a new native override must pass
status: open
symptom: nothing compared a native override with the guest body it replaces. An override was trusted
  because the game kept running, which is exactly the evidence a wrong-but-harmless-looking override
  also produces, and there was no gate an automated override producer (an LLM swarm) could be held to.
tags: native-dispatch,overrides,differential,diagnostics,gate
created: 2026-09-29
updated: 2026-09-29
---

## What exists now

`runtime/cpu/override_differential.*` shadows sampled calls of selected overrides inside a real run
(`PSXPORT_OVERRIDE_DIFF=<name|0xaddr>,...`, knobs in `docs/config.md`). Per sampled call it snapshots
CPU/GTE/RAM/scratchpad, runs the original to its return live (native suppressed, resumed across budget
exhaustion), restores the snapshot with Lightrec invalidation for every changed range, runs the native
with the original's device traffic replayed (`runtime/cpu/side_effect_journal.*`), compares under the O32
contract (`override_differential_contract.*`), and continues from the original's state. It writes one log
line per verdict change, a summary with denominators, and a JSON report that
`tools/port/override_differential_gate.py` gates on.

## Decisions a reader might otherwise re-derive

- **Device traffic is RECORDED on the original and REPLAYED to the native**, not executed twice. Running
  both paths live would apply every GP0 write, CD command and FIFO read twice, and continuing from the
  original would then be a lie about the devices. Replay means the devices only ever see the original.
- **Guest time is held on the native path** (`accountGuestInstructions` and the executor's mid-segment
  device-clock commit consult the journal). The native path's state is discarded, so its instructions
  must not move the CDC/SIO/field clocks.
- **Unreplayable effects make the call INCOMPARABLE, never a match**: a BIOS/platform-HLE/pad-work-area
  service, a syscall, or pending interrupt/host-turn work on the original (the native is then not run for
  that call), or any of those on the native (withheld). A nested *title* override is replayable (its
  traffic uses the same funnel) and runs on both paths.
- **Dead-stack window** (deviation from "every RAM byte must match"): bytes below the entry `sp`, within
  `PSXPORT_OVERRIDE_DIFF_DEAD_STACK` (default 8 KiB), are callee-frame residue under O32. They are counted
  (`dead_stack_bytes_ignored`) but not judged; without this every original that builds a frame would
  "mismatch" a frameless native. Bytes outside the window, including a caller's stack arguments above
  `sp`, are judged.
- **All-incomparable is a failure** (strengthening of "zero samples fails"): a selector whose every
  sample was incomparable has compared nothing.

## Evidence

`tests/test_override_differential.cpp` (13 cases, through the product dispatch route: a guest caller
`jal`s the function, the executor's host-dispatch boundary resolves the override, `NativeDispatcher::invoke`
hands the call to the differential). Negative cases: wrong `v0`, a missing store, a clobbered `s0`, an
extra store, an extra device write (mismatch, and the write never reaches the device), a native BIOS call
(incomparable), an original syscall (incomparable, native not run). Positive controls: a correct override,
a correct override whose result depends on a replayed device read. Also: sampling arithmetic, address
selectors, a zero-sample selector reported as a failure, and restored code bytes invalidating a block the
native path had translated (patched at the block's first word: an interior-word write is not honoured by
Lightrec at all, recorded in issue 0050). `tools/port/test_override_differential_gate.py` covers the gate's failure
classes against a clean-report control.

## Open

- Device models a native override reaches DIRECTLY in C++ (not through `Core`'s memory API) are not
  observed; the report says so in `not_observed`.
- A native that polls a device register in a C++ loop after its replay diverged reads the last recorded
  value (or 0) forever; such a call would hang rather than report.

## First real use (2026-09-29, Spyro 1, SCUS_942.28)

One `tools/drive.py gameplay` run (Artisans, 300 held frames) of a scratch spyro build whose `dist2d`
(0x80017990) returned `v0 + 1`, with 17 overrides armed. Verdicts:

| override | calls seen | sampled | match | mismatch |
|---|---|---|---|---|
| 15 unmodified overrides (rand, fill, copyw, vadd, dist2d's peers, mvmva, …) | 58,277 | 1,044 | 1,044 | 0 |
| `dist2d`, deliberately wrong | 12,109 | 205 | 0 | 205 — first: `v0` original 0x2D00 native 0x2D01 |
| `angdist` | 0 | 0 | — | — reported as a failure (no evidence) |

`dllink` and `angdist` are never called on this route (the native frame driver owns the display
list), and the gate fails such a selector rather than passing it. Megamanx4 issue 0039 is the
second real use and found two register clobbers in a shipped owner; it also reports two framework
gaps still open here: the final report is not written on that title's fault exit path, and a
host-invoked override is sampled once rather than per call.
