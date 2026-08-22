---
id: I008
kind: instrument
status: trusted
created: 2026-08-21
---

## Instrument

python3 tools/recomp/test_emit.py

## Validated by

Trusted for main_reentry and computed-control-flow emission: the synthetic main-reentry positive
requires wrapper, generated body, and dispatch case for an interior PC; the identical unseeded
executable requires all three to be absent. The bounded inline-table positive must execute four
distinct `lui`/`ori` trampoline slots, while its structural discriminator requires exactly those
slots and rejects the adjacent jump-bearing shared body. The latter failed against the prior
shipping emitter by dispatching the first local slot externally. The Vagrant regression controls
also reject a partial `lui` surviving a load clobber and any unaligned computed target. The emitter
has 46 pytest cases; the combined emitter/decoder suite passes 55 cases.

## Known failure modes

Synthetic tables do not prove a consumer's actual executable partition or generated substrate;
pair this instrument with an exact generated-target check and a bounded real-disc continuation gate.
