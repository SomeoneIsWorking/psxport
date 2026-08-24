---
id: 20
title: Jal-shaped overlay payload splits a resident branch from its delay slot
status: open
symptom: Toy Story 2 model package table reset writes only slot zero; entries 1..127 stay stale and later arena payload bytes are dereferenced as pointers
tags: recompiler,overlay,mixed-data,delay-slot,toystory2,model-table
created: 2026-08-24
updated: 2026-08-25
---

## Root cause

`overlay_funcs()` treats every overlay word decoding as `jal` to resident text as a call seed. Toy Story 2 FMV payload words at `0x800E7C70` and `0x800E9C70` equal `0x0C0107FF`, spuriously targeting resident `0x80041FFC`. That address is the delay slot of the retail `bnez` at `0x80041FF8`, not a function entry. Promoting it partitions `0x80041F38` immediately before the slot, so its emitted reset loop loses `addiu a1,a1,4` and writes zero to table base `0x800C7268` 128 times instead of clearing 128 consecutive entries. Slot 9 remains `0x8013B770`; disc/DMA later reuses and overwrites that arena, and the model consumer dereferences payload bytes `93 F8 A4 ED` as pointer `0xEDA4F893`.

## Candidate resolution in isolated worktree

A resident target whose preceding retail instruction is a branch, jump, or register jump is structurally impossible as a distinct function entry: that word is executed as the predecessor control instruction's delay slot. `overlay_funcs()` now rejects only that impossible target class, without requiring a prologue or title/address special case. Toy Story 2 loses exactly five false delay-slot roots and regenerates from 365 roots/889 functions to 360 roots/884 functions. `gen_func_80041F38` now retains the retail delay-slot increment and no `gen_func_80041FFC` exists.

The whole-pipeline regression places a jal-shaped data word in a synthetic overlay and targets a resident loop delay slot. Before the fix the loop increment runs once instead of three times; after the fix it runs 3/3. The direct emitter suite passes 48/48 with Clang.

## Runtime verification

One bounded direct headless writer watch observes slot 9 loaded with `0x8013B770`, cleared by a later
execution of reset `0x80041F38`, then loaded with distinct fresh package `0x8012ED8C`. The process
continues through field 10,303 and exits only at its 180-second bound with no fatal, recompilation
miss, or retired `0xEDA4F893` fault. This is the producer-side opposite answer the static correction
predicts; it does not claim indefinite stability or interactive gameplay.

The issue remains open until the shared framework patch is reviewed and landed. Falsifiers are a
legitimate resident function entry immediately preceded by a control instruction, any remaining
`gen_func_80041FFC` root, failure to execute the loop increment 128 times, or the same terminal pointer
fault after corrected emission.
