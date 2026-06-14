# Native PC port — static-recompiler plan (Tomba! 2)

**Direction set 2026-06-14:** a true native PC port — **no PSX CPU emulation, no PSX BIOS
ROM**. The wide60 emulator-runtime work (`runtime/wide60rt` on Beetle) is paused; the
enhancement tier rides on top of a faithful recompiled base *later* (see "Faithful first").

## What "no emulation / no BIOS" actually means here
- **CPU:** R3000A MIPS of `MAIN.EXE` is **statically recompiled to C** (translated once,
  compiled native) — no interpreter loop.
- **BIOS:** kernel/syscalls are **HLE'd** in native C (reuse the `runtime/` HLE work) — no
  BIOS ROM.
- **Peripherals are NOT optional.** The game's own code drives GTE (COP2), the GPU command
  FIFO, SPU, MDEC, CD/DMA, timers and pads directly. These get implemented as **native C
  modules**, pragmatically **lifted from our GPL-2 Beetle fork** (`vendor/beetle-psx`) so
  they are correct-by-construction and match the oracle. "No emulation" = no CPU interpreter,
  not "no hardware implementation."

## Locked decisions
- Instruction-level static recompiler, **generated code is sacrosanct** (never hand-edit;
  fix the emitter or add an override). Output gitignored + regenerable.
- Recompiler written in **Python** (offline tool; ~41.5K instructions — throughput is a
  non-issue at this scale; no premature C rewrite).
- Peripheral/HW layer **lifted from the Beetle GPL-2 fork**; Beetle is also the **diff
  oracle**.
- **Faithful first, then enhance** — prove bit-identical to the oracle before any widescreen
  / 60fps work.

## Target EXE (Tomba! 2, SCUS_944.54, 167936 B, disc LBA 152155)
- entry PC `0x80018B6C`, load `0x80010000`, text size `0x28800` (165888 B = 41472 insns),
  init SP `0x801FFFF0`, init GP `0x0`. Text at file offset `0x800`.

## Assets already in hand (big head start)
- Extracted boot EXE: `scratch/bin/tomba2/SCUS_944.54`.
- **Full Ghidra decomp of all 1886 functions** (`scratch/decomp/ram_f1000_all.c`) — a
  function-boundary map and a **reference for writing overrides**, NOT the port itself
  (not bit-faithful: `undefined4`, missing GTE semantics, overlay "bad instruction data").
- Deep RE: StrPlayer / cooperative task scheduler (`0x801fe000`) / intro sequencer
  (`0x801064f0`) / CD filesystem (`CdSearchFile` `0x8008b8f0`).
- HLE BIOS work in `runtime/`.
- Beetle GPL-2 fork = HW-module source + bit-exact oracle.

## Front-end input image — CRITICAL finding (2026-06-14)
The recompiler input is **NOT the boot EXE `SCUS_944.54`**. Measured:
- The boot EXE text `[0x80010000,0x80038800)` **differs from frame-1000 RAM in 98.8%** of
  words. At `0x8001FC50` the EXE has `0xFFFFFFFF` while RAM has `27BDFFD8` (`addiu sp,-0x28`,
  a real prologue). The boot EXE is a **bootstrap/loader stub**; at runtime the real game code
  is loaded from the CD over `0x80010000+`, overwriting it.
- Decoding the 264 Ghidra function entries (and 28480 body words) **from the frame-1000 RAM
  snapshot** gives **0% unknown** — the decoder is validated against real game code, and the
  real code is the resident image, not the boot EXE. Resident code spans far beyond the boot
  text (e.g. `jal 0x8011534C` → code at `0x8011xxxx`).
- The game is a **resident core + swapped overlays** (intro overlay `0x80106xxx`, etc.),
  loaded from on-disc files. **Next: recursively list the disc ISO9660 tree** (extend
  `tools/discdump`) to find the main executable + overlay files = the clean static inputs.
  (A RAM snapshot is a runtime artifact — BSS-written, one overlay resident — so it's a
  fallback image, not the ideal reproducible input.)

## Stages
- **S0 — recompiler decoder (test-first). DONE/validated.** `tools/recomp/{psexe,decode}.py`
  + `test_decode.py` (8/8). Decoder verified **0% unknown over 28480 words of real game code**
  (Ghidra fn bodies decoded from the RAM snapshot). Coverage = full R3000A + COP0 + COP2/GTE
  move/op/load/store.
- **S0.5 — disc file tree + true static input.** Recursive ISO9660 lister; identify the
  on-disc main executable + overlays; settle the recompiler's per-image front-end. ← *next*
- **S1 — emitter.** ops → C against a modeled `R3000` state (regs + memory accessors);
  one C function per Ghidra-mapped function, block labels + `goto` for intra-fn branches,
  **dispatch table** keyed by address for indirect calls/jumps (seed indirect-only targets).
- **S2 — runtime skeleton.** 2 MB RAM + scratchpad, register file, memory map, entry
  trampoline, HLE syscall stubs (A0/B0/C0 tables).
- **S3 — GTE (COP2)** native module (port from Beetle); recompiler emits COP2 ops as calls.
- **S4 — differential harness vs Beetle oracle.** Per-block CPU-state diff on the same EXE;
  stop at first divergence; drive the build→diff→root-cause→fix loop. Stand this up early.
- **S5 — peripherals:** GPU FIFO→renderer, SPU, MDEC, CD, DMA, timers, pads (lift from
  Beetle).
- **S6 — boot to first frame faithfully;** expand coverage via the harness loop.
- **S7 (later) — wide60 enhancements** (widescreen + interpolated 60fps) on the proven base.

## Hard rules (inherited)
- Generated code sacrosanct; systemic emitter/decoder fixes, never per-address hacks.
- No magic constants; document every non-obvious translation.
- Verify = bit-exact diff vs oracle on real execution, cited. Never "matches" on a vibe.
</content>
