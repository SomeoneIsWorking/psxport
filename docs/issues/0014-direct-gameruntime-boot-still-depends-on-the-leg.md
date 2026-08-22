---
id: 14
title: Direct GameRuntime boot still depends on the legacy GameConfig bag
status: resolved
symptom: A consumer deriving GameRuntime directly has core.cfg null, so crt0 setup, MAIN routing, and resident-code backtraces cannot read their executable facts
tags: runtime,architecture,gameconfig,boot,overlay
created: 2026-08-22
updated: 2026-08-22
---

## Root cause

Three generic executable-image algorithms reached through the transitional `c->cfg` compatibility
view instead of a typed runtime-owned fact. A direct runtime correctly exposes no legacy config, so
crt0 planning, resident-MAIN routing, and the guest backtrace lacked an authority for their measured
image facts.

### Resolution (2026-08-22)

`GuestProgramImage` now owns crt0, resident-text, and backtrace-range facts. `Core` snapshots that
immutable view, and only `LegacyGameRuntimeAdapter` projects the old fields into it. The ownership
test mechanically rejects legacy-field reads in all five generic consumers.
