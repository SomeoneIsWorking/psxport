---
id: 146
title: Agent runs shared one memory card, so every run started from the previous run's writes and the same route reached gameplay at different fields
symptom: Spyro 3's title route reached gameplay at field 4740 in two runs and 4890 in a third, Spyro 2's at 3340 once and 3190 after, same binary and same pad edges
state_items: none (tooling and instrument; no runtime behaviour changed)
tags: determinism,memory-card,launch-environment,field-digest,agent-runs,root-cause
created: 2026-10-01
updated: 2026-10-01
---

## Answer

Not host time. `runtime/psx/memcard.cpp` opens `PSXPORT_CARD`, else the title's default, else
`scratch/saves/card.mcr` relative to the working directory. Agent launches set none, so every run of every title
read and then rewrote one shared file: the previous run's writes were the next run's boot state.

Measured on the Spyro 2/3 title routes (stock tooling, one card, alternating titles, 16 runs): Spyro 3 gave 4890 on
the card as the 2026-09-20 checkout left it and 4740 on each of the five runs after; Spyro 2 gave 3340 on the card Spyro 3 had left
and 3190 on every run after its own write. A `fielddigest` trace of the two Spyro 2 runs agrees for fields 0..244
and differs at 245, the first field at which the guest reads the card; every field before it is byte-identical.
Host load did not matter: with the host saturated (18 busy loops on 16 cores) five runs gave one digest, identical to
six unloaded runs. The wall-clock reads in the runtime (`cd_override.cpp` host stream pacing, `gpu_perf`, `gpu_vk`
fence budgets, SPU profiling) do not reach guest-visible state on the headless unpaced path: CDC deadlines, the
segment cap and the display clock all run on `EmulatedTime`.

## Fix

- `agent_environment(settings, card=...)` and `blank_card_environment()` (`tools/port/launch_environment.py`)
  delete the image and point `PSXPORT_CARD` at it, so the product creates a blank card as for a first run. The
  oracle's `compare.fresh_card` now calls the same function instead of its own copy.
- `PSXPORT_DEBUG=fielddigest` (`runtime/psx/field_digest.*`, called from `Timing::advanceDisplayFields`) prints one
  line per display field: emulated tick, a hash of main RAM, I_STAT, I_MASK, pad. Two runs on the same inputs must
  print identical lines; the first differing line is the divergence point.

## Consumers

Every agent tool that launches a port and expects a repeatable run should pass `card=`. Toy Story 2's
`tools/headless_run.py` builds its environment with `agent_environment` and does not yet.
