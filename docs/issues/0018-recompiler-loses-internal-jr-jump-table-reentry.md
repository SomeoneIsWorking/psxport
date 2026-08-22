---
id: 18
title: Recompiler loses internal jr jump-table reentry labels
status: open
symptom: Toy Story 2 FUN_800100E4 reenters its internal 32-way jr table at 0x8001040C, loops, then misses 0x800104E4 because generated C emits sequential unlabeled gotos for 0x800103EC..0x800104E4.
tags: recompiler,jump-table,jr,reentry,toystory2
created: 2026-08-22
updated: 2026-08-22
---

Future generic emitter task. The consumer has an exact verifier and mutation controls for the 32-way table. The emitter must preserve every legitimate internal computed-jump target as a reentry label and route to it; no per-title manual generated-code edit.
