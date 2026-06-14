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
- **S0.5 — disc file tree + true static input. DONE.** Added `discdump list` (recursive
  ISO9660 tree) + `discdump get <NAME>` (extract any root file). **Recompiler input =
  `MAIN.EXE`** (root, LBA 23, 716800 B): entry `0x800896E0`, load `0x80010000`, text
  `0xAE800` (714752 B), SP `0x801FFFF0`, GP `0`. **99.9% identical** to resident RAM_f1000
  (262/178688 diffs = runtime data writes); **all 1596 Ghidra fns in range decode 0%
  unknown** from the clean file. `SCUS_944.54` is just the licensed boot stub that loads
  `MAIN.EXE`.
  - **Overlays** load *above* MAIN's text end `0x800BE800` (seen: `jal 0x8011534C`, intro SM
    `0x80106xxx`) from `BIN/*.BIN` — a later front-end concern; MAIN.EXE is the core.
  - **Disc map** (file → LBA, size): `MAIN.EXE` 23/716800; `SCUS_944.54` 152155/167936;
    `BIN/A00..A0L.BIN` (area data) + `BIN/{OPN,SOP,START,DEMO,GAME,CRD}.BIN` (overlays/seq);
    `CD/{TOMBA2.DAT,IMG,IDX,SND, SWDATA.BIN}` + `CD/{BGM,DEMO,VOICE}.XA` (stream data/audio);
    `MOVIE/{LOGO,OP,END}.STR` (FMVs — the logo-hold = `LOGO.STR`); `ZZZ.DAT`.
- **S1 — emitter. DONE/validated.** `tools/recomp/emit.py` emits all **1597** functions
  (Ghidra boundaries as units; entry seeded) → `generated/tomba2_rec.c` (6.6 MB), which
  **compiles cleanly** (3.5 MB .o). Runtime model `runtime/recomp/{r3000.h,mem.c}` (R3000
  state, flat 2 MB RAM + scratchpad, unaligned lwl/lwr/swl/swr, R3000 div semantics);
  `stubs.c` for GTE/COP0/syscall (S2/S3). Emitter handles **delay slots**, intra-fn
  `goto`/labels (only for emitted addresses; data-region targets route to `rec_dispatch`,
  so no undefined labels), direct calls vs `rec_dispatch`, generated dispatch table.
  **Semantics verified** on 3 hand-checked leaf functions incl. delay-slot effects
  (`runtime/recomp/test_leaf.c`, all pass). Reproduce: `tools/recomp/build.sh`.
  - Documented faithful-first simplifications (verify via harness): load-delay not modeled;
    add/addi treated as wrapping; computed `jr` (switch tables) → `rec_dispatch` (jump-table
    recovery is a later pass); inter-function data blobs are emitted as dead "functions"
    (correct but wasteful — exclude later).
- **S2 — runtime skeleton. STARTED (boot driver + recon).** `runtime/recomp/boot.c` loads
  `MAIN.EXE` into `g_ram`, sets SP/GP/ra-sentinel, enters `func_800896E0`. Emitter now
  **discovers direct-`jal` targets** (fixpoint, data-bounded) → seeds functions Ghidra missed
  (1597→1598; fixed the `0x80089860` miss). Dispatch misses route to runtime
  `rec_dispatch_miss`. **The core runs from boot.** Boot HLE surface, measured in order:
    - `A0:0x39` InitHeap, `B0:0x19`, `B0:0x5B`, `C0:0x0A` ChangeClearRCnt, `A0:0x72`, `B0:0x35`
    - one still-missing **indirect** function `0x8009A8E8` (reached via `jalr`, not direct
      `jal` — needs a fn-pointer/indirect seed pass or manual seed).
    - hardware regs touched: I_MASK/I_STAT (`0x1F8010 74/70`), DMA (`0x1F8010F0`), Timer1
      (`0x1F8011 10/14`), CDROM (`0x1F8018 00/03`), **GPUSTAT `0x1F801814` ready-poll**
      (spins because `mem.c` returns 0 — first thing the GPU/timer stub must satisfy).
  - **DONE:** `runtime/recomp/hle.c` — recomp-native HLE (heap A0:0x33-0x39, HookEntryInt,
    FileWrite→stderr, GetB0/C0Table, ChangeClearPAD, GPU_cw, C0 installers, `syscall`
    Enter/ExitCriticalSection via `$a0`), transcribed faithfully from the proven
    `hle_kernel.cpp`. `mem.c` reports GPUSTAT `0x1F801814` permanently ready (+toggling bit31)
    to clear the ready-poll. Seeds: `0x8009A8E8/ADC4/AA4C`.
  - **Boot now executes deep real game code** (verified): heap init → HookEntryInt → CD init
    (prints `CD_init`/`CD_cw`/`CD timeout` via FileWrite) → past GPU handshake → OpenEvent/
    EnableEvent/**WaitEvent** loop + CD-command retry loop.
  - **S5 BOUNDARY reached (honest):** further progress needs real **peripheral + IRQ/event
    delivery** — the CD retry loop and `WaitEvent` block on CD-complete / VBlank IRQs that
    nothing generates yet. Faking "event fired" / CD-done would be a bandaid (forbidden).
## DIRECTION REFINED (2026-06-14): no CD/hardware emulation — native file I/O via overrides
User: "no CD code, no emulation, pure PC native." So we **do NOT** emulate the CD controller
or deliver CD/VBlank IRQs. Instead we **override the game's CD/streaming functions with native
C** that reads disc data directly from a file and completes **synchronously** (no IRQ
handshake). The deep CD/StrPlayer RE from earlier this session names the override points:
- `FUN_8008c1ec(blocks, startLBA, buffer)` — core "read N blocks from LBA"; override → native
  read from the disc image by LBA, synchronous.
- `FUN_8008bf50` (CdSearchFile dir-cache reader) / `FUN_8008b8f0` (path→descriptor) — already
  RE'd; may override at the file-resolve level instead/also.
- read-complete SM `FUN_8008c294` → done flag `0x800AC308`; completion cb
  `FUN_800899bc` — the override sets the done state / invokes the completion path the game
  polls, so its CD-driver wait loops fall through immediately.
- low-level CD-command/init (`CD_cw`/`CD timeout` loop, fns `0x8009Axxx`) — override the
  init/command primitive to report success without hardware.
VBlank-driven `WaitEvent` similarly handled natively (a native frame/event source, not IRQ).

- **S2.5 — override infrastructure. DONE/validated.** Emitter now emits `gen_func_X` (recomp
  body) + `func_X` wrapper checking a runtime override slot; `rec_set_override(addr, fn)` +
  `rec_func_index`. Body stays alive (A/B toggle + diffable), overrides fire on direct AND
  indirect calls, **super-call** = call `gen_func_X`. Verified in `test_leaf.c` (replace,
  fire, super-call, toggle-off — all pass).
- **S3 — native CD/file backend. CD PORTION DONE (2026-06-14, "later 16").** Implemented:
  `runtime/recomp/disc.c` (libchdr by-LBA `disc_read_sector`, verified CD001@16 / PS-X@23) +
  `runtime/recomp/cd_override.c` overriding CdInit `0x8008B2D8`→0, CdCommand `0x8008AC34`→0,
  CdSync `0x8008A6EC`→2, read `0x8008C1EC`→native blocks×2048 read. CD timeout/Init-failed
  spin gone; boot advances to `VSync: timeout`. **Remaining S3: native VBlank/VSync + event
  table** (override libetc VSync `FUN_80085900`/counter `DAT_800abde0`; events in hle.c) so the
  game mounts the CD FS and the native read path is exercised end-to-end. Original pinned
  targets below (all recompiled fns):
  - `0x8008B2D8` **CdInit** (the boot blocker — emits `CD_init`, then `CD_cw`/`CD timeout`
    polling CD I/O regs `0x1F8018xx` with no IRQ → spins). Override → report drive ready.
  - `0x8008AC34` CD command-write (`CD_cw`), `0x8008A6EC` low-level command+wait (`CD timeout`
    chokepoint — every command waits here). Override → complete synchronously, success.
  - `FUN_8008c1ec(blocks, startLBA, buffer)` read-N-blocks → native by-LBA read from a disc
    image; `FUN_8008c294` read-complete SM / done flag `0x800AC308` → set synchronously;
    `CdSearchFile 0x8008b8f0` path→descriptor (already RE'd).
  - Backend: `discdump image` → flat 2048-B/LBA data image, or libchdr at runtime. Then native
    VBlank/event source for `WaitEvent`. Verify boot advances to title/FMV.
  - **Separate gap (not the boot blocker):** the printf/format-parser `0x8009A76C` (indirect,
    Ghidra-missed) has an internal computed-`jr` **jump table** that the recompiler routes to
    rec_dispatch (no in-function jump-table recovery yet) → debug-printf misbehaves. Fix via
    (a) emitter jump-table recovery, or (b) a **native printf override** (cleaner, PC-native).
    The earlier `0x8009A8E8/ADC4/AA4C` "seeds" were mid-switch labels (mistake) — corrected to
    seed the real entry.
- **S4 — diff harness vs Beetle** (parallel): verify recomp bodies bit-exact (overrides off).
- Emitter TODO: automatic indirect-pointer (`lui`+`addiu`) seed scan to end CD-helper
  hand-seeding.
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
