---
id: I023
kind: instrument
status: trusted
created: 2026-08-26
---

## Instrument

test_platform_hle_direct_runtime — shipping PlatformHlePlan registration/handler gate

## Validated by

RED before the fix because PlatformHlePlan lacked both typed fields; after the fix its positive case resolves and executes both framework handlers, while the opposite control places SetGeomScreen outside the declared half-open window and observes register_ REFUSE it. The handler assertions also distinguish the two leaves by their different register and ProjParams effects.

## Known failure modes

(none recorded yet)
