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
executable requires all three to be absent. The bounded inline-table positive must preserve its
`lui`/`ori` base across an ordinary branch and execute four distinct trampoline slots, while its
structural discriminator requires exactly those slots and rejects the adjacent jump-bearing shared
body. It failed against the prior shipping emitter by dispatching the first local slot externally,
then failed against the first combined candidate because the depth-attribution clobber predicate
erased the base at the branch. The whole-pipeline overlay-data control must run a resident loop's
delay-slot increment three times even when a mixed-data overlay word decodes as `jal` to that slot;
it ran once before impossible delay-slot roots were rejected. Two negative controls reject a partial
`lui` retained past a load clobber and a masked computed table with non-word-aligned targets; both
reproduced failures in the first candidate fix. The direct emitter suite passes 48/48 tests; the
decoder suite passes 9/9.

## Known failure modes

Synthetic tables do not prove a consumer's actual executable partition or generated substrate;
pair this instrument with an exact generated-target check and a bounded real-disc continuation gate.
