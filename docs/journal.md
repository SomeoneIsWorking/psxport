# Debug / progress journal

## 2026-06-14 (later 9) — DIRECTION CHANGE: native PC port (static recomp); decoder S0 done
**User: "new direction — port to PC, no PSX emulation, no PSX BIOS."** wide60/emulator path
paused; full plan in `docs/recomp_port_plan.md`. Approach = instruction-level static
recompiler (MIPS R3000A → C), HLE BIOS, peripherals (GTE/GPU/SPU/MDEC/CD) **lifted from the
GPL-2 Beetle fork**, diffed bit-exact against Beetle as oracle. Faithful-first, then wide60.
- **S0 decoder DONE + validated:** `tools/recomp/{psexe.py,decode.py,test_decode.py}` (8/8,
  anchored to the real Tomba2 entry words). Full R3000A + COP0 + COP2/GTE coverage. Verified
  **0% unknown over 28480 words** of real game code.
- **CRITICAL input finding:** the recompiler input is **NOT the boot EXE `SCUS_944.54`**.
  Boot-EXE text `[0x80010000,0x80038800)` differs from frame-1000 RAM in **98.8%** of words
  (EXE `0xFFFFFFFF` vs RAM `27BDFFD8` real prologue at `0x8001FC50`). The boot EXE is a
  **loader stub**; the real game = a **resident core + overlays loaded from the CD** over
  `0x80010000+` (spans past boot text — `jal 0x8011534C`). The 1886-fn Ghidra decomp matches
  the RESIDENT image (0% unknown), not the boot EXE. NEXT: recursive ISO9660 lister (extend
  `tools/discdump`) to find the on-disc main executable + overlay files = clean static inputs.

## 2026-06-14 (later 10) — recompiler input found: MAIN.EXE (validated 99.9% vs RAM)
Added `discdump list` (recursive ISO9660 tree) + `discdump get <NAME>`. Disc tree shows the
real game executable: **`MAIN.EXE`** (root, LBA 23, 716800 B) — entry `0x800896E0`, load
`0x80010000`, text `0xAE800`, SP `0x801FFFF0`. Extracted to `scratch/bin/tomba2/MAIN.EXE`.
- **Validated:** MAIN.EXE text vs resident RAM_f1000 = **99.9% identical** (262/178688 diffs =
  runtime data writes); **all 1596 in-range Ghidra fns decode 0% unknown** from the clean
  file. So MAIN.EXE IS the recompiler input; `SCUS_944.54` is just the boot stub that loads it.
- Overlays load above MAIN's text end `0x800BE800` (`jal 0x8011534C`, intro SM `0x80106xxx`)
  from `BIN/*.BIN` — later concern. FMVs are `MOVIE/{LOGO,OP,END}.STR`. Full disc map in
  `docs/recomp_port_plan.md`.
- NEXT (S1): emitter — recursive-descent decode from `0x800896E0` (+1596 fn-entry seeds) →
  C per function, dispatch table, modeled R3000 state + memory accessors.

## 2026-06-14 (later 11) — S1 emitter done: full core compiles, leaf semantics verified
`tools/recomp/emit.py` translates MAIN.EXE → C: **all 1597 functions** → `generated/
tomba2_rec.c` (6.6 MB), **compiles clean** (3.5 MB .o). Runtime: `runtime/recomp/{r3000.h,
mem.c,stubs.c}` (R3000 state, flat 2 MB RAM+scratchpad, lwl/lwr/swl/swr, R3000 div sem).
- Emitter handles delay slots, intra-fn goto/labels (only for emitted addrs; data-region
  branch targets route to rec_dispatch → no undefined labels — this was the one compile bug,
  caused by data blobs in inter-fn gaps), direct-call vs rec_dispatch, generated dispatch.
- **Verified** on 3 hand-checked leaf fns incl. delay-slot effects (`test_leaf.c`, all pass):
  `0x80089A30`→v0=0x800ABFD4 (lui+DS addiu), `0x800535D4`→mem8(a0+374)+1, `0x800269EC`→v0=1
  +store. Reproduce: `tools/recomp/build.sh`.
- Faithful-first simplifications to verify via harness: no load-delay; add==addu; computed
  `jr`→rec_dispatch (switch-table recovery later); data blobs emitted as dead fns.
- NEXT (S2): load MAIN.EXE into g_ram, entry trampoline `func_800896E0`, HLE syscalls +
  A0/B0/C0 vectors; stand up S4 diff harness vs Beetle in parallel.

## 2026-06-14 (later 8) — CORRECTION to "later 7" RE map (overlay sequencer decompiled)
Read the overlay decomp (`scratch/decomp/overlay.c` = `FUN_801064f0`) + the worker/scheduler
chain from the full decomp. Three labels in "later 7" are **WRONG** — fixing them so the next
session doesn't chase a non-existent activator:
- **`FUN_8008BF50` is the CD directory-cache reader for `CdSearchFile`, NOT a "stream
  activator."** Given a path-component dir-record index it calls `FUN_8008c1ec(1, descriptor,
  buf)` to read **one** directory sector and parse `0x2c`-stride dir records out of the table
  at **`0x80102d44`** (= the **directory-record** table, `0x2c`=dir-record size — NOT a
  stream-descriptor table). `FUN_8008c1ec` is a generic "read N blocks from LBA into buf"
  (1 block = a dir sector; many blocks = a stream). So "later 7"'s `0x8008BF50(a0=N) → plays
  stream N` and "descriptor table `0x80102d44`" are both wrong. Do not drive `0x8008BF50` to
  trigger FMV#2 — it just reads a directory.
- **`FUN_80051E60`** = cooperative task **dispatcher** over `0x801fe000` (stride `0x38`);
  `FUN_80080860/80/90/a0` are **context-switch coroutine primitives** (task create/start/resume
  — green threads), NOT libgpu draw wrappers as labeled.
- **`FUN_801064F0`** = intro/opening **sequencer**: resolves `\BIN\OPN.BIN` (×25 entries),
  `\BIN\START.BIN` (×3), `\CD\TOMBA2.IDX` (×5), `VOICE/DEMO/BGM.XA` via `CdSearchFile`
  (`FUN_8008b8f0`); stores descriptors at `DAT_800be118`/`DAT_800be1e0`/`DAT_800be0f0`; then
  bootstraps loader coroutines (`FUN_80044f58`, `FUN_8004514c` via registrar `FUN_80044bd4`)
  on a state byte at `obj+0x48` (`_DAT_1f800138`). The `obj+0x48` transitions are
  **unconditional** (load bootstrap), so this SM is NOT the consumer-paced logo gate.
**Corrected model:** the logo→FMV#2 hand-off is a **coroutine task playing the logo MDEC
stream**; when it yields end-of-stream the sequencer advances to the next playlist entry. The
real skip target remains (per "later 6"): the **logo stream's frame-count / length field** so
the segment consumes in ~1 frame and the game advances through its OWN code. Candidate: the
OPN.BIN descriptor for the logo segment (`FUN_8008a110` yields LBA; size = field+4). Not yet
pinned to a writable counter.

## 2026-06-14 (later 7) — FULL DECOMPILATION + StrPlayer playback architecture mapped
**Did what the user asked: "decompile everything with tools."** Built headless-Ghidra
decompilation tooling (committed): `tools/decomp.sh` + `tools/ghidra_decomp.py` (all 1886
MAIN.EXE functions → `scratch/decomp/ram_f1000_all.c`) and `tools/ghidra_overlay.py`
(force-disassemble a fn-ptr-only overlay range). Also **ripped out the turbo** (committed):
`g_module_turbo` + `Tomba2_LogoHoldTurbo` gone; `-play` fast-forwards only on manual Tab.

**StrPlayer playback architecture (from the decompiled C):**
- **Main loop `FUN_80050b08`** (resident, 0x80050b08): infinite per-frame loop. Per frame:
  graphics + `FUN_800506d0()` (timer-array tick) then dispatches on **state `DAT_1f80019c`**
  (scratchpad 0x1F80019C): 0 = display current stream frame (`FUN_8008179c` = PutDispEnv etc.,
  these 0x80080xxx/0x80081xxx are **libgpu wrappers**, not the demux); 2 = display one more +
  set state 1; **3 = stream end → outer loop restarts = advance to next playlist entry**.
  The dwell is the `do {} while (DAT_800e809c < DAT_1f800235)` vblank wait at ~0x80050ce4.
- **Cooperative task scheduler:** task array at **`0x801fe000`** (state byte at offset 0:
  1=timed,2=ready,3/4=running; stride 0x38/0x70). `FUN_800506d0` decrements timers (1→2 on
  expiry); `FUN_80051e60` dispatches (2→4 start `FUN_80080880`, 3→step). `obj = *(0x1F800138)`
  is the current task iterator (journal "obj+0x48" = overlay SM state).
- **Playing a stream = `FUN_8008c1ec(blocks, startLBA, buffer)`**: BCD-converts LBA →
  `Setloc`(cmd2) → `FUN_8008c960`(start stream) → which calls **`FUN_8008c5d8`** (read setup:
  sets `DAT_800ac2e4`=blocks, `DAT_800ac2f8`=remaining, registers read-cb `FUN_8008c294`).
- **Read SM `FUN_8008c294`** (per CD-read-complete): decrements `DAT_800ac2f8` (remaining); on
  0 → sets **`DAT_800ac308`=1 (done)** and fires completion cb `PTR_FUN_800abf24`=`FUN_800899bc`.
- **Activator `0x8008BF50`(a0=N)** reads a 44-byte descriptor from the table at **`0x80102d44`**
  (record N) and calls `FUN_8008c1ec` → plays stream N. The playlist overlay SM `0x80106xxx`
  calls the walker `0x8008B8F0` → activator on advance.

**Why the advance isn't yet forceable (measured):** the logo's CD *read* finishes by ~f1060
(`DAT_800ac2f8`=0, `DAT_800ac304` freezes at 0x772) — the ~250f to f1310 is the logo **MDEC
video task playing out at VBLANK rate**, a separate task in the **overlay `0x80106xxx`**. That
task's completion is what calls the activator for FMV#2. `FUN_8008c294`/`FUN_8008c1ec`/
`FUN_8008c960`/`FUN_800899bc` are all **advance-only** (coverage) = they are FMV#2's read, not
the logo's. Forcing state `DAT_1f80019c`=3 is **inert** (re-confirmed, hammered 80f). The
logo-task completion trigger lives in the overlay SM, which **does not cleanly decompile** in
Ghidra's default MIPS mode (GTE/cop2 "bad instruction data").

**NEXT (well-scoped):** decompile the overlay with a **GTE/cop2-aware** Ghidra processor (PSX
variant) from a *hold-phase* dump (logo overlay resident, e.g. `hle_hold_f950.bin`), find the
logo MDEC-video task's end-of-stream → it calls the walker/activator(FMV#2). Then the native
override is: on Start during the silent hold, drive that task's completion (or directly call
the activator for the FMV#2 descriptor index, found from the 0x80102d44 table) — the game then
plays FMV#2 through its OWN code. Verify FMV#2 plays clean AND advances to title afterward.

## 2026-06-14 (later 6) — REALITY CHECK on HLE: intro-skip + turbo do NOTHING; logo hold is VBLANK-paced ~370f; all low-level forces fail
**User report (ground truth): "skipping FMV#1 still waits 5 seconds till FMV#2." Plus
directive: HLE BIOS, RE, PC-native overrides — NO emulator turbo.** Investigated entirely
on the **HLE BIOS path** (`PSXPORT_HLE_BIOS=1`, instant CD, `-repl`). Findings, all measured:

- **The whole intro-skip (incl. the "later 5" turbo) was tuned to the OpenBIOS ROM path and
  does NOTHING under HLE.** HLE no-input timeline: FMV#1 (boot EXE) ReadS f413 → Pause f931;
  StrPlayer logo hold f944 → **FMV#2 ReadS f1318** (~370f ≈ 6s gap). Held-Start-from-boot
  gives the IDENTICAL f413/f1318 — the SCEA/Whoopee/Dwell/LogoHold hooks don't move anything
  under HLE, and **FMV#1 itself is not Start-skippable under HLE** (tap at f500 → still Pause
  f931). (`g_module_turbo` only acts in the `-play` loop; in `-repl` the dwell-hook still runs.)
- **CAVEAT that invalidated earlier "held-Start" repl tests:** REPL button names are
  **case-sensitive** (`press Start`, not `START`). My first held tests used `START` → no-op.
- **Dwell-escape (0x80050CE4) caps at ~−105f under HLE.** New knob forcing the 0x80050CE4
  pace-loop exit *unconditionally* (every reached frame): FMV#2 f1318 → **f1213 only**. So the
  display pace dwell is NOT the gate; the consumer-paced ring fill is. This is the hard ceiling
  of the entire dwell-escape family — it can never collapse the ~370f hold.
- **Forcing the prebuffer-wait gate DESYNCS (FMV#2 never comes).** The gate is
  `0x8008A784 bnez v1,0x8008A7B8` (advance when ring pos > target, else wait ≤60f). Hooking it
  to force v1=1 (always "buffer ready") → **no cmd 1B at all** through f1400. Same failure
  family as faking disc EOF / poking 0x80102748. Do not retry forcing this gate.
- **The StrPlayer state byte `*(0x1F80019C)` is 0 for the ENTIRE hold** (→1→2 only at f1318).
  Dispatcher `0x80050D00`: state 0 → calls per-frame driver `0x8008179C` EVERY frame; the
  advance decision is INSIDE 0x8008179C's consume logic (callee chain incl. status flag
  *(0x800ABE20) and the FMV#2 load 0x8008A6EC/0x8008B4B8). That is exactly why poking the
  state byte is inert — 0 is the *active* driver state, not a "waiting" state.
- **Mechanism (re-confirmed via PCCOV coverage-diff wait-frame vs advance-frame):** the
  advance frame uniquely runs the playlist walker `0x8008B8F0`, activator `0x8008BF50` (called
  from walker @0x8008BA60), and the FMV#2 overlay init/SM `0x80106xxx` (0x801064F0 parses the
  '\'-playlists; walker called from 6 sites 0x80106514..0x801066F0). The overlay SM is dormant
  during the hold and wakes only when the logo segment is consumed → it then drives the advance.
- **Savestates are UNRELIABLE under HLE** (retro_serialize captures Beetle state but NOT the
  runtime-side HLE BIOS thread/callback state) → on load the StrPlayer desyncs (0 CD cmds).
  Fast-iteration must run from boot (instant-CD makes f0→f1318 a few seconds).

**Bottom line:** the ~370f logo hold is ~265 logo frames displayed one-per-real-VBLANK
(consumer-paced) — NOT removable by escaping any spin (that needs more VBLANKs = turbo) and
NOT forceable at any low-level gate (all desync). The ONLY clean native skip is to **cut the
logo SEGMENT short** (drive the overlay-SM advance, or shorten the logo stream's
length/frame-count so it consumes in ~1 frame and the game advances through its OWN code).
That is the documented next step and remains uncracked — needs the consume counter / logo
stream length field RE'd (scratchpad-aware watchpoint; PSXPORT_WATCHW is main-RAM only).
**Tooling proven useful this session: `PSXPORT_PCCOV="s-e:path;s-e:path"` coverage-diff** (set
difference of executed PCs between a waiting window and the advance window — pinpointed the
advance-only functions). Experiments reverted (dead ends): force-dwell, force-prebuffer-gate.

## 2026-06-14 (later 5) — inter-FMV logo hold residual COLLAPSED via scoped fast-forward
The dwell-skip (later-4) got the hold f719->f598 (-121f) but left a ~3.3s residual =
the StrPlayer's per-VBLANK MDEC decode of the (invisible, skipped) logo clip's ~210
data frames (profile under dwell-skip: spread decode work 0x800834A0/0x8008B6D0 MDEC
poll/0x80044E5x RLE/0x8009A3F0 copy; one frame per VBLANK; no pokeable flag — the clean
"drive the advance" attempts all re-seek-loop). User-directed override: re-enable the
existing `g_module_turbo` (8x emulated frames/present, pacing bypassed) SCOPED to the
verified-silent hold: `g_tomba2 && Tomba2_LogoHoldTurbo() && !psxport_cd_strsnd_on()`.
Tomba2_LogoHoldTurbo() = the dwell-skip hook (signature-gated to the StrPlayer overlay,
never gameplay) fired within 45 emulated frames (bridges read/decode frames between
pace-dwells); STRSND-off is the hard cutoff (FMV#2 turns CD-XA audio on -> turbo ends
the same step). play-loop batch rewritten to re-check turbo per step & break on drop.
**Safe, not general turbo:** runs the SAME emulated frames unpaced, so FMV#2 state at
f598 is identical -> plays bit-for-bit the same, just sooner. Verified: turbo ON
continuously f388->598, off f599; no-Start path unchanged (FMV#2 f1181); -play boots
clean. **Wall-clock (-play): hold ~3.5s -> ~1.1s** (floor = emu speed ~190fps for the
210-frame consume). Combined intro gap (FMV#1-skip + dwell-skip + turbo): **5.6s -> ~1.1s**.

## 2026-06-14 (later 4) — inter-FMV logo skip IMPLEMENTED + verified (dwell-escape, STRSND-gated)
**Shipped:** `LogoHoldSkip` in runtime/games/tomba2.cpp — a Start-gated native override
that collapses the silent logo hold. **Verified result: FMV#2 ReadS f719 -> f598 (-121f)
when Start is held during the hold**, FMV#2 renders cleanly (Tomba jungle/character scene,
meanRGB (40.4,18.9,1.1)->(81.4,40.0,22.3), matches known-good baseline), plays to completion
(jungle scene still streaming at f1300, no hang), and **no regression** (Start NOT held =
natural f719). Default ON (gated with the other intro skips via PSXPORT_T2_NOINTROSKIP).
- **Hook:** PC `0x80050CE4`, sig `0x9482809C` (the per-frame StrPlayer pace-dwell body),
  redirect to loop-exit `0x80050CF8` — the SAME lever as the loading-screen FmvDwellSkip.
  Fires only when `s_skip_held && !psxport_cd_strsnd_on()`.
- **Phase gate = STRSND (CD-XA audio) OFF.** The silent logo hold runs STRSND-off (Setmode
  0x80/0xA0); every real FMV/cutscene streams audio (STRSND-on, 0xC0). New generic primitive
  `psxport_cd_strsnd_on()` (cdc.c) exposes it. Verified: holding Start through FMV#2 does NOT
  fast-forward it (same f598 start, full duration) — the gate protects real movies.
- **CORRECTS "(later 3)"'s claim that forcing the dwell saved 0 frames.** That was tested via
  PSXPORT_REA_FORCEDWELL on a different build/probe. Measured directly on the f719 skip path,
  escaping the 0x80050CE4 dwell DOES accelerate the consume: f719->f598. (fmv-skip.md's
  "held-Start saves ~120f" was right.) The residual f324->f598 is genuine consumer pacing
  (still-logo decoded 1 frame/VBLANK) — the safe floor; forcing past it underruns.
- **Approach 1 (read genuine EOF early) RULED OUT, newly tested:** redirecting the consume
  Setloc to the real EOF LBA 11492 makes the StrPlayer re-seek 11492 forever without advancing
  — its advance is gated on its OWN consumed-sector bookkeeping, not on physically reading EOF.
  Same failure family as forging an XA EOF submode. Do not retry disc-position fakery.
- **Approach 3 (drive the SM) unnecessary:** RE confirmed the inner SM 0x80106F80 is DORMANT
  during the hold (first ticks f709), states 1/2/3 are pass-through increments (jumptable
  @0x801062C4 all -> 0x80107034), state 0 = CD poll 0x80089bac(a0=0xE), state 4 @0x80107054 =
  advance. The SM only wakes once the consume completes, so the consume rate IS the gate; the
  dwell-escape addresses it directly. Tooling: added `[setloc f%u] lba= pc=` log (cdc_log).

## 2026-06-14 (later 3) — inter-FMV logo hold: DATA-bound, NOT audio/time; mechanism mapped
**Question answered:** is the ~317f logo hold (skip FMV#1@f380 -> FMV#2 ReadS@f719)
DATA-bound or TIME/AUDIO-bound? **Answer: DATA-bound (stream-position-driven), NOT
audio- or display-clock-bound.** Evidence (all `wide60rt_reA`, instant-CD default):
- **NO audio of any kind plays during the hold.** Setmode during FMV#1=0xC0 (STRSND on,
  XA processed every ~3f); during the hold Mode=0x80/0xA0 (**STRSND OFF**, zero `[xa]`
  sectors); FMV#2 (f719) Mode back to 0xC0. The CD-DA `Play`@f311 was Paused@f322. So
  the "logo jingle" hypothesis is FALSIFIED — the reads are plain data ReadN.
- **Display-clock collapse saves 0 frames.** Forced the per-frame pace dwell 0x80050CE4
  to always elapse (PSXPORT_REA_FORCEDWELL probe): FMV#2 ReadS STILL at f719, identical.
  So the hold is NOT gated by the StrPlayer's display counter (0x800E809C vs threshold
  0x1F800235=2). (Re-confirms fmv-skip.md's instant-CD/dwell findings on THIS skip path.)
- **Fixed 401 sectors** DMA'd during the StrPlayer hold phase (reproducible to the word
  across runs). Setloc LBAs creep slowly through TWO interleaved logo streams (~LBA
  1879-1908 and ~6565-6717, advancing 1904->1905->1906->1908 over f388-f522 = consumer-
  throttled), then JUMP to LBA 152238 (OPS.STR = FMV#2) at f719. The hold ends when the
  logo streams reach their descriptor end-LBA -> advance to next playlist entry.
- **Read pacing** during active stretches is ~2-3 sectors/frame (consumer-paced, real-
  VBLANK-clocked); reads are instant when issued (instant-CD), the long idle gaps (e.g.
  f455-f526) are the CPU spinning 62% in the dwell + ~11% in an RLE/decode loop at
  0x80044E14 — i.e. decoding/displaying the still logo, not waiting on disk.

**Per-frame decision chain (execution mapped, not a single pokeable gate):**
- The StrPlayer playlist is a `'\'`(0x5C)-delimited name list; the walker at **0x8008B8F0**
  tokenizes it and calls **0x8008BF50(a0=N)** to activate the Nth stream (a0=2 f387,
  a0=3 f402, **a0=4 @f717** -> activates FMV#2 -> ReadS f719). 0x8008BF50 stores N to
  0x800AC2D4 (the 3->4 the lead saw; poking it is INERT, re-confirmed).
- The actual advance is driven by the FMV#2-overlay state machine at **0x80106388**
  (outer state `[obj+0x48]`, obj=`*(0x1F800138)`, jumptable @0x8010622C) and inner SM
  **0x80106F80** (state `[obj+0x4a]`, jumptable @0x801062C4; state 4 @0x80107054 does the
  playlist-advance). This dispatcher is DORMANT during the hold — first runs f654 (one
  tick) then f709+ — because the StrPlayer stream scheduler only ticks the next segment's
  SM when the current (logo) segment's data is consumed. The hold proper (f386-f707) is
  the logo-stream consume; the SM spin-up (f709-f719) is the tail.
- Inner state 0 (0x80106FF0) spins `0x80089bac(a0=0xE)` until nonzero = a CD-command-
  complete poll (0x8008AC34 reads per-channel state 0x800ABC00+ch*4, issues via 0x8008A6EC).
  So the terminal gate is CD-command/stream-position state, consistent with data-bound.

**Forceability:** no clean single-flag lever (stream-count 0x800AC2D4 poke INERT; scratch
state 0x1F80019C is a downstream effect, written by 0x80050DA8 each frame, ->2 only at
f724 AFTER ReadS). The advance is genuinely data-position-driven. The forceable approach
remains the lead's option (b): a native override that DRIVES the segment advance (set the
logo streams' end-reached / invoke 0x8008BF50(a0=4) + the 0x80106xxx outer-state advance)
on Start. FMV#2 reached early is verified glitch-free (fmv-skip.md), so risk is only WHICH
state to drive. NOT YET implemented — needs the logo-stream descriptor end-LBA field RE'd
to set "logo consumed" cleanly, OR drive the 0x80106388 outer-SM transition directly.
**Tooling added (kept):** PSXPORT_PCTRACE_EXCL="lo-hi" excludes a hot spin sub-range from
the pctrace ring (so the dwell 0x80050CE4 can't flood it). Probes used then reverted:
cdc.c [setmode]/[setloc]/[xa] logs (gated on PSXPORT_CDC_LOG), tomba2 PSXPORT_REA_FORCEDWELL.

## 2026-06-14 (later 2) — inter-FMV logo skip: deep RE, new tooling, NOT yet cracked
**User's actual goal:** pressing Start during FMV#1 skips it, but then there's a
**~5.6s gap to FMV#2** they want gone. Reproduced: skip FMV#1@f380 → FMV#2 ReadS@f719
(=339 frames), the SAME fixed-duration hold as the no-input case (f842→f1181).
- **Structure:** FMV#1 is played/skipped by the **boot EXE** (0x8001xxxx, ReadS
  lastpc=0x80017E58). The logo + FMV#2 (OPS.STR) are the **MAIN.EXE StrPlayer**
  (0x8008xxxx). After FMV#1 the StrPlayer holds the **Whoopee logo as a static load
  mask** (screenshot: bright logo f880-1000, black f1120) while streaming the logo
  clip (jingle) for its fixed duration, then advances to FMV#2.
- **NEW TOOLING (committed):** scoped PC-trace ring (`PSXPORT_PCTRACE="lo-hi"` +
  REPL `pctrace`), CPU-scratchpad access (REPL `sr`/`sw8`, accessors in cpu.c) —
  games keep hot state in the scratchpad (0x1F800xxx), invisible to main-RAM tools;
  DMA3-arm + CD-cmd-write issuing-PC logs; StateProbe (PSXPORT_T2_STATEPROBE).
- **RE via pctrace (diff advancing frame vs waiting frame):** the advance runs a
  code path absent on waiting frames → command-complete handler **0x80085Exx**
  (clears flag *(0x800AAD1A)) → caller **0x80050D00** (StrPlayer state machine:
  reads scratchpad state `*(s5+0x19c)=0x1F80019C`; s5=0x1F800000) → dispatch
  **0x8008179C** → advance work (0x8008A6EC, 0x8008B4B8 load FMV#2 group + ReadS).
- **The trigger is the logo clip's STREAM END** (last CD command completing), not a
  pokeable flag. Every candidate is an EFFECT, verified by poking (no early FMV#2):
  scratchpad state byte 0x1F80019C (0→2, but changes AFTER the ReadS), 0x800AC2D4
  (active-stream count 3→4), 0x800AC299, 0x800BF8A7, main-RAM 1/frame counter
  0x800A5ADC. Forcing the counter high BREAKS progression.
- **Read acceleration = CONFIRMED DEAD END** (3 tests + journal): the reads are
  clamped to the StrPlayer consumer-ack (one batch/vblank); forcing faster delivery
  wedges the CD pipe (0 sectors), and the logo hold is real-time/audio-clocked, not
  read-bound (XAFAST = 0 frame change).
- **OPEN:** no clean override found. The advance is data-driven (stream end). NEXT
  candidates: (a) find the StrPlayer command-list position / stream frame-count that
  the command-complete handler checks for "last command", and force it; (b) make a
  native override INVOKE the advance dispatch (0x8008179C path) directly on Start
  during the logo. FMV#2 reached early is verified glitch-free (fmv-skip.md), so the
  risk is only WHICH state to drive, not early playback.

## 2026-06-14 (later) — HLE FMV#2 stall FIXED (new threads seeded IEc=0); prebuffer is VBLANK-paced
- **FIX (committed d3fd1a2):** `open_thread()` seeded a fresh thread's SR by copying
  the creator's saved TCB SR. Tomba2's StrPlayer OpenThreads the FMV#2 prebuffer thread
  from inside a critical section (IEc=0), so it inherited interrupts-disabled and spun
  forever (the IEc=0 deadlock from the entry below). Fix: force 0x404 (IEp+IM-IP2 = the
  LeaveCriticalSection enable mask) into the seeded SR. **Verified under pure HLE:** full
  FMV#2 prebuffer (12 SeekL + ReadN cycles, ReadS@f1181), ~2445 sectors DMA'd, FMV#2
  renders cleanly (Tomba jungle scene). Pinned via new PSXPORT_CS_LOG (Enter/Leave SR +
  ChangeThread resumed-IEc) — the last op before the hang was `ChangeThread -> new TCB
  resume=0x800499E8 newsr IEc0`.
- **OPEN — the ~5s gap from Whoopee-skip to FMV#2 is a VBLANK-real-time-paced prebuffer**,
  NOT CD-load-slow and NOT the CPU dwell. Measured: FMV#1 Pause@f842 -> FMV#2 ReadS@f1181
  (~339f ≈ 5.6s). Reads are instant (instant-CD) but spaced 70->10f apart (ring-drain /
  consumer-paced, the still-logo MDEC decode at VBLANK rate). The dispatch counter
  0x800ABDE0 advances exactly 1/frame. **Forcing the 0x80050CE4 pace dwell (76.5% of CPU
  during prebuffer) saved 0 frames** — so the gate is real-time VBLANK pacing, not CPU
  spin (contradicts fmv-skip.md's "held-Start saves 120f"; that path also fired other
  overrides). To make FMV#2 instant on PC needs a native lever on the game's per-frame
  prebuffer pump rate or its completion gate — fmv-skip.md showed the gate vars resist
  poking. NEXT: RE the StrPlayer script-processor command list (0x8008AE00, 2-byte
  records) for a "hold N frames" opcode, or drive the VBLANK prebuffer callback faster.
- **Prebuffer RE — gate FOUND, but it's consumer-rate-locked (no clean frame-counter
  lever).** Tooling: added issuing-PC logging at the CD command-register write
  (`[cmd-write]`, beetle cdc.c). Findings:
  - All CD commands (prebuffer ReadN + FMV#2 ReadS) issue from **0x8008ADE4** (the
    command-register store in the per-command processor 0x8008AC34); the opcode is the
    caller's arg. The caller walks a **script VM**: interpreter loop at **0x8008C034**
    (walks variable-length stream descriptors to 0x80104B68; per-stream handler
    0x8008A00C is just an LBA→MSF converter), command issue inline.
  - **The ReadS trigger is `0x800AC2D4` (active-stream count) going 3→4** — written by
    0x8008C174 (s6 = #active streams). It sits at 3 the ENTIRE prebuffer (f864→1179)
    then →4 at f1179 → ReadS@f1181. So the FMV#2-start gate = the 4th (FMV#2) stream
    descriptor becoming active.
  - **RULED OUT the "counter>target" wait (0x8008AE60) definitively**: measured live,
    `target − counter ≡ 960` every frame (target rewritten to counter+0x3C0) → that
    branch never fires; it's a self-resetting watchdog, not the pacer. fmv-skip.md's
    "held-Start saves 120f" did NOT reproduce (forcing the 0x80050CE4 dwell saved 0f).
  - **Root nature:** the StrPlayer streams everything at real-time MDEC/display rate
    (~1.8 sectors/frame — FMV#1 too: 922 sec / 511f). The inter-FMV prebuffer fills
    FMV#2's ring at that consumer rate while the (now-skipped) logo "plays", for its
    designed ~6s. No single pokeable dwell; the clean fix is to advance the StrPlayer
    script past the logo stream (force the 4th-stream activation / logo-stream-done),
    which needs the stream-descriptor activation field RE'd — the next focused step.
    Forcing FMV#2 early is verified SAFE (fmv-skip.md: clean frames at f1111), so the
    risk is only in WHICH state to flip, not in early playback.

## 2026-06-14 — HLE FMV#2 stall ROOT-CAUSED: interrupts stuck disabled (IEc=0), NOT a CD-ring bug
- **Falsified the prior "FMV ring never fills / lossy CD-IRQ / per-sector DMA" framing**
  (docs/tomba2-hle-irq.md banner added). Frame-stamped probes prove FMV#1 streams fine:
  **1969 DMA3 arms / ~922 sectors** into the ring to f835. The stall is the FMV#2
  *prebuffer*, after FMV#1: Setloc@f848 (acks fine) then **zero further CD commands**.
- **Root cause:** the StrPlayer spins in the prebuffer wait `0x8008AE54` (gate =
  `*(0x800ABDE0) > 0x3C3`) with **interrupts disabled**: new `irq` REPL cmd shows
  `I_STAT=0005 I_MASK=000D pending=0005 SR.IEc=0 EPC=800808A4` (LeaveCriticalSection),
  identical f880→f1280. VBLANK+CDROM IRQs are pending+unmasked but IEc=0 so beetle never
  vectors them. The gate counter `0x800ABDE0` is bumped only by VBLANK callback
  `0x800909C0`, dispatched by the game's I_STAT-polling dispatcher `0x80085D8C`
  (`I_STAT & I_MASK & 0x0D`, bit0=VBLANK). Dispatcher runs **578× on ROM** (every frame),
  **11× on HLE** then stops → counter frozen at 3 → infinite spin → no FMV#2.
- EnterCS/LeaveCS trace: HLE goes net **+1 Enter** at the f845-848 transition then silent;
  the outermost Leave that restores IEc=1 is never reached. ROM recovers. DEAD ENDS
  (re-confirmed): instant-CD on/off identical; ring/DMA path is healthy; mode-0x2000
  callback wiring is irrelevant (0x800909C0 is poll-dispatched, not a BIOS event callback).
- **NEXT (fix):** instrument IEc across each Enter/Leave syscall + return_from_exception
  RFE-pop (hle_irq.cpp) around f845-848 vs ROM. Tooling added: `irq`/`gpr` REPL cmds;
  `[dma3]`/`[cd-reqdata]`/`[hretrace]`/`[timerN-mode]` probes (PSXPORT_CDC_LOG). Full
  RE chain in docs/tomba2-hle-irq.md (2026-06-14 section).

## 2026-06-13 (later) — native intro skip IMPLEMENTED + verified
- **Found the real intro driver:** `0x800111B4` is the logos sequencer (straight-line
  blocking code, NO data-driven stage var). It calls `0x80010D54` (SCEA license:
  fade-in/hold(180)/fade-out/done state machine on `$s5`) then a poll loop running
  `0x8001138C` (Whoopee anim player; loop exits when `*(0x800253EC)==1`).
- **Falsified the falsifications** (kept notes honest): `0x800111B4` IS the sequencer
  (prior "stale stack-scan" note was wrong — it tested an unrelated fn); `0x8001E0CC`
  DOES run; `0x800253EC` is the Whoopee done-flag (not a "scene step"), but is NOT a
  usable lever (only re-checked at anim-pass boundaries — poking it mid-pass is inert,
  verified).
- **Native skip (default ON, Start-gated; runtime/games/tomba2.cpp):** `SceaSkip`
  @0x80010ED0 forces `$s5=3` → SCEA jumps to done/return (skips hold AND fade
  directly). `WhoopeeSkip` @0x80011414 sets the loop terminator + redirects to the
  epilogue. **Verified:** Start held → post-Whoopee FMV stage at ~f505 vs ~f1181
  baseline (~676f / ~11s earlier); end-to-end reaches the opening FMV cleanly.
- **Retired** the rejected `Tomba2_WantTurbo` fast-forward + falsified `kScenePhase`.
- **OPEN (user directive):** skipping the logos unmasks the loads they hid — the
  opening FMV streams in (white/black gap) at native XA speed; inter-stage black gaps
  are partly the game's frame-counted dwells. NEXT: RE the FMV load + paced dwells and
  make them PC-instant. Timing/IRQ primitives mapped but LEFT EMULATED (owning them =
  faking hardware). See docs/tomba2-intro.md.

## 2026-06-13 — intro RE restart: prior orchestrator FALSIFIED; logos NOT input-skippable
- **User reframed the goal:** the SCEA + Whoopee Camp logos must be **natively
  skippable via RE + native port**, NOT emulator fast-forward (the shipped
  `Tomba2_WantTurbo` Start-hold is rejected). Methodology the user fixed:
  **tooling → RE → native port → patch the native port**. Full RE in
  `docs/tomba2-intro.md`.
- **Tooling:** added REPL `shot <path>` (dumps current framebuffer to PPM on demand
  while driving `-repl`); VideoCb caches the last frame in g_last_fb. Made the
  skippability tests below possible.
- **Verified intro timeline** (frames.py contact sheet, instant CD, no input): SCEA
  license ~f200–600 → black → Whoopee Camp logo ~f850–1700 → opening **MDEC FMV**
  ~f1800+ → title screen.
- **The opening FMV IS natively Start-skippable** (REPL: press Start at f1900 →
  title by f1990 vs still-FMV at f2160 no-input). **SCEA + Whoopee are NOT** (hold
  Start from boot still shows them) — they're load masks with display-hold timers,
  pad not polled. Render heartbeat 0x8003CCA4 dormant the whole intro (expected).
- **FALSIFIED `0x800675CC`** (the scene-orchestrator doc's "scene phase"): never
  written during SCEA/Whoopee (watchpoint logs only the f25 BIOS clear). Also
  **FALSIFIED the `0x80011B4`/`0x80018C10` "intro driver"**: it came from a stale
  `bt` stack-scan; tracing `0x8001e0cc` (in that chain) over f0–1900 = ZERO hits —
  that code is post-intro title/menu, not the logo driver. Banner added to
  tomba2-scene-orchestrator.md. (Lesson: the heuristic stack-scan reports stale
  return addresses; confirm a "driver" by TRACING it before building on it.)
- **Ground truth (PCCOV intersection SCEA∩Whoopee):** intro driver code at
  0x80017E4C (VSync/elapsed-frame timing; frame counter 0x800267B4, markers
  0x80025684/88), 0x800181E8-0x80018484 (stage machine — disasm NEXT), + smaller
  regions. RAM-diff stage-var candidates: 0x80025454(5→0), 0x80025458(3→0),
  0x8002667C(1→0), 0x80026620/28(0→8). NEXT: watchpoint these to find the dispatch.
- KEY gotcha: the FMV stream overwrites 0x80011xxx/0x80018xxx — dump RAM **during
  the logo phase** (f400/f1000) to disassemble the intro driver, not during the FMV.

## 2026-06-13 — display enhancements: widescreen + 4x internal res + sharp scaling
- User: "not widescreen, doesn't look higher resolution, no bilinear." All three
  addressed via stock Beetle core options + presentation fixes (no GL context):
  - **Higher resolution = `beetle_psx_internal_resolution`.** The SOFTWARE
    renderer honors it: libretro.c:4287-4290 sets psx_gpu_upscale_shift =
    upscale_shift_hw when there is NO hw renderer, and the framebuffer is emitted
    at the upscaled size (libretro.c:3581). Verified 1x->350x240, 2x->700x480,
    4x->1400x960; 4x runs ~174 emu-fps headless (~5.8x realtime) so it's fine for
    live play with the software renderer. Default 4x in -play.
  - **Widescreen = `beetle_psx_widescreen_hack` + `_aspect_ratio`.** Reports the
    chosen aspect via av_info (16:9->1.778, 21:9->2.370, off->1.333). Default
    16:9 in -play.
  - **No bilinear:** SDL_HINT_RENDER_SCALE_QUALITY = nearest. Present now scales
    to the core-reported aspect (g_aspect from av_info + SET_GEOMETRY/
    SET_SYSTEM_AV_INFO), not a hardcoded 4:3, so widescreen isn't squished.
- **KEY INTERACTION: widescreen_hack AND internal_resolution change the
  coordinates the wide60 harness captures.** With both default-on, rtps-reproject
  fell to 2% and wide60-verify to 8% — the reproject math + GP0<->GTE join assume
  NATIVE screen coordinates, which the upscale (vertices at 4x) and the widescreen
  X-scale both break. So these are **play-time enhancements only**: default ON for
  -play, OFF (native 1x/4:3) for headless/RE runs. Env overrides either way
  (PSXPORT_INTERNAL_RES, PSXPORT_WIDE/PSXPORT_NOWIDE, PSXPORT_WS_ASPECT). Battery
  back to 100% at native. TODO: when the wide60 present stage is built, its
  capture must account for the upscale shift + widescreen scale to coexist.
- **Tomba2 widescreen caveats (expect, per-game tier):** the hack widens
  GTE-projected geometry (characters/models) but Tomba2's terrain is CPU-projected
  and 2D HUD isn't projected at all — those won't widen consistently, so expect
  misalignment/stretch. Also the wider FOV reintroduces edge pop-in (the
  cull-cone-widening override that masked it is disabled — it broke gameplay,
  see below). Proper widescreen for Tomba2 needs the per-game cull/terrain work.

## 2026-06-13 — user-reported fixes: blinking objects, real logo skip, dynamic res
- **Blinking / walk-through objects = the cull-cone-widening override.** User
  reported game objects blinking in/out *and being walk-through* (logic, not just
  visual). Root cause: `CullSlti` (runtime/games/tomba2.cpp) is the only Tomba2
  hook that REDIRECTS + writes regs; it forced the engine to draw objects it had
  culled, but their collision/logic was never set up -> visible yet walk-through,
  flickering at the widened boundary. The six slti sites are NOT all confirmed to
  be the same cull test. **Fix: gated OFF by default** (PSXPORT_T2_CULLWIDEN=1 to
  re-enable for RE). Correctness-first: a widescreen enhancement must not corrupt
  base gameplay. User confirmed blinking gone.
- **Intro logo: a TRUE in-game skip (user rejected fast-forward; "we're on PC").**
  First disproved the "make the disc read instant" instinct empirically: the data
  reads (ReadN) are ALREADY ~64x; the logo is paced by the **jingle audio stream**
  (ReadS/STRSND, irq1 every frame f680-1181), not a slow read. Accelerating audio
  sector delivery does NOT help — gated by consumer-ack it's ~10% faster; ungated
  it reaches the post-logo milestone at f2451 vs f1352 baseline (SLOWER: the
  intro's sequencing is tied to the audio playing in real time). DEAD END: no
  CD-timing change shortens the logo. Reverted (cdc.c back to committed).
- **FALSIFIED: "`0x800253EC` is the intro STEP".** The earlier RE used byte-level
  MEMWATCH (the tool logged single bytes, not words). The byte at 0x253EC is 0x10
  and looked flag-like; the actual 32-bit word is **0x26A60010 — a constant
  pointer**. The auto-skip (LogoSkip) wrote 1 into it whenever the logo audio
  clock ticked, which in -play **corrupts that pointer mid-intro** = the user's
  "Whoopee Camp doesn't play" bug. Headless never tripped it (no audio sink, clock
  frozen at 0). Removed entirely. Lesson: MEMWATCH now logs aligned 32-bit words
  in hex.
- **Real intro state machine RE'd + decompiled — see
  `docs/tomba2-scene-orchestrator.md`.** Found with the new write-watchpoint
  (`PSXPORT_WATCHW`): the scene-phase word is **`0x800675CC`** (0=SCEA license,
  1/2=transition+load, 3/4=Whoopee Camp logo + opening cutscene loop), driven by
  `scene_update` (0x8002C97C) dispatching through jump table 0x8004CDC0. Each scene
  holds until an advance event (`0x80076AC0`) fires; the event is paced by the
  kernel ready-signal `0x80047174` (VSync/CD) via the advance pump (0x8003AAA4).
  **Forcing the event races the loaders** (reproduces the white screen), so the
  clean skip is to fast-forward the ready-signal-paced dwells. Implemented:
  **Start-held fast-forward** of the intro (Tomba2_WantTurbo, scoped to phases via
  0x800675CC). Whoopee Camp now plays; holding Start skips SCEA + Whoopee (and the
  shared-phase opening cutscene). test-wide60 still 100%.
- **Dynamic resolution (play window).** Window now sizes to ~85% of desktop height
  at 4:3 (was fixed 960x720); framebuffer rescaled to the window every present with
  correct PSX 4:3 aspect (letterbox), adapting live to resize. F11 = fullscreen,
  linear filtering. (Visual confirmation pending — headless can't verify SDL.)
- wide60 matcher (first half of the prior NEXT): nearest-TR object-identity match
  across flip-segments + in-between synthesis (lerp transform, reproject joined
  verts, unjoined left at frame-A = no flicker). Verified at t=1 against the game's
  own next-frame projections: 79/79 xforms matched, 97% of next-frame verts within
  2px. Present/rasterize stage still pending (needs the rasterizer-path decision).

## 2026-06-13 — object-based 60fps: scope reframed + Tomba2 object system RE'd
- **User reframed the 60fps work (supersedes the DuckStation primitive matcher).**
  Not screen-space prim matching (its flaw: not object-based). Instead: RE the
  game's own entity system, an override that tracks objects (map IDs), the
  interpolator reads object transforms from there, and a *custom* renderer draws
  interpolated state (not bound to beetle's/duck's render path).
- **Cull-cone widening is now a native override** (was PSXPORT_POKE): six slti
  sites hooked in runtime/games/tomba2.cpp (CullSlti), RAM untouched, region-
  preserving redirect, overlay-signature-gated. Boot stays deterministic
  (RAMHASH). patches/tomba2/cull-widen.md updated.
- **Runtime gained savestates + REPL input driving** (main.cpp): -loadstate/
  -savestate, REPL save/load + press/release/tap (g_repl_buttons), F5/F9 in
  -play. Unblocks reaching live scenes headlessly. (Journal's "savestate TODO"
  done.)
- **Tomba2 object system mapped** (patches/tomba2/objects.md; tools/disasm.py =
  new capstone MIPS disassembler for RAM dumps). The cull/LOD dispatcher
  **0x8007712C** is the universal per-object chokepoint: a0 = object* for every
  live drawable, once per logic frame. Hooking it (ObjectCull, PSXPORT_T2_OBJLOG)
  enumerates the whole live set. Verified at frame ~7037+: 68-90 objects in a
  contiguous pool (base ~0x800EF478, **stride 0xC4**), positions at obj+0x2e/
  0x32/0x36 (s16 X/Y/Z), type at +0x0c, visible flag at +0x01. Camera world pos
  in scratchpad 0x1F8000D2/D6/DA. Pointer is stable per-entity across frames =
  the object ID; pool-slot reuse on scene change = snap, not lerp.
- **Renderer chosen: from-scratch reprojection** (user pick over re-running the
  game's renderer). GTE tap extended to forward OFX/OFY/H (CR24-26); new RTP
  vertex tap (psxport_set_rtp_hook, RTPS/RTPT in gte.c) reports
  (local V, transform) -> game screen SXY. PSXPORT_T2_RTPDUMP dumps tuples.
- **Projection core PROVEN faithful:** tools/reproject.py reimplements GTE RTPS
  (DivTable/CalcRecip, dist/Z divide, IR + screen saturation) and reproduces the
  game's SX/SY on **6,348,755 / 6,348,755 = 100%** of captured vertices. We can
  reproject the same geometry at an interpolated (R,TR) bit-faithfully.
- **Renderer capture layer built (runtime/wide60.{h,cpp} + GPU poly tap in
  gpu.c).** Captures per frame: GTE transforms, RTP projected verts (SXY->local
  +transform), GP0 polygons (verts/uv/color/clut/tpage). Joins poly verts to
  their transform by SXY. New psxport_set_gpu_poly_hook; PSXPORT_WIDE60=1 enables
  the renderer module (owns the GTE/RTP/GPU hooks), PSXPORT_WIDE60_LOG logs
  coverage.
- **KEY FINDING: the game projects one logic frame BEFORE it draws.** Same-frame
  poly<->projection join = ~0-2%; previous-frame join = 40-78%. So frame
  boundaries must be the GPU display flip (GP1 0x05), draws joined to the prior
  segment's projections. The <100% remainder is 2D geometry (UI/text/2D bg) with
  no RTPS origin — those snap, not lerp. (This also explains why the cur-frame
  join looked broken — it was a timing offset, not a coordinate-space bug.)
- Flip boundaries done (GP1 0x05 tap); beetle now a committed in-submodule fork
  (psxport branch), patch file retired (user call). GP0 vertex coords extracted
  as 11-bit signed (Coord11) to match the GPU/GTE.
- **CPU-projected terrain confirmed definitively.** Flip-segmented join = ~56% of
  drawn verts. Diagnosis (per-segment): projections are ABUNDANT (rtp ~7000-8000
  vs ~4700 drawn verts), yet 44% of drawn verts have NO projection even within
  ±4px (near-match 9-31%). So the unjoined 44% is genuinely transformed by a
  non-RTPS path (terrain/background = CPU-projected), not a coord nudge or timing
  miss. **User direction: RE + tap the CPU projection path too** (full fidelity,
  not a screen-space fallback).
- MVMVA terrain hypothesis FALSIFIED (those ops are lighting, not projection) —
  terrain is pure-CPU-projected, no GTE op to tap (do-not-retry).
- **User scoped the renderer:** interpolate only camera + GTE 3D models (RTPS-
  tappable), leave CPU-projected terrain + 2D UI at 30fps, NO FLICKER top
  priority. (memory: wide60-scope-decision.)
- **Core renderer op verified in C++:** wide60 retains per-joined-vertex local
  coords + the producing transform (s_xforms_prev); ReprojectRTPS (R*V+TR then
  the verified divide/screen-map) reproduces the game's captured object SXY on
  **1755/1755 = 100%** of joined verts. So object screen positions can be
  regenerated from (local, transform) in-runtime -> interpolation = lerp the
  transform + reproject the same local verts.
- NEXT: match each object's transform across flip-segments (nearest-TR), build
  the in-between display list (frame-A polys; GTE-object verts reprojected at the
  lerped transform; non-GTE polys unchanged = no flicker), rasterize + present
  A / in-between / B at 60fps.

## 2026-06-13 — read pacing root-caused via driven debugging; full fast boot chain works
- **The "stuck on Whoopee logo" class is solved.** Chain of findings, all via the REPL/
  trace/CDC-log tooling (no screenshot-guessing until final calibration):
  (1) the "frozen vsync counter" was a misread — the game main loop RESETS its counter
  each frame; identical bt = normal idle. (2) pad input verified delivered end-to-end
  (kernel pad buffer 0x8010246F shows pressed bits). (3) the logo persisting was real:
  frame-stamped CDC logs showed bit-8 fast read pacing made the game's chunked ReadN
  loader retry forever (577 cmds vs 284 healthy; sectors outran per-vsync chunk
  accounting). (4) FIX: fast pacing is consumer-conditional — next sector in 7000cy
  only if the previous data IRQ is ACKed, else native gap. Also: pacing applies to
  ReadN only (new psxport_read_is_readn; games stream audio manually over ReadS where
  sector pacing IS the audio clock — accelerating it wedges stream-end detection).
- Result, fastboot BIOS + full instant CD (default): EXEC f~50, logo jingle f~679,
  in-engine cutscene stream (Setmode+ReadS) at **f1778** (stock+native: f2833; original
  chain: ~4000+). RAM-hash deterministic. Remaining logo time is jingle-audio-bound
  (a per-game override could skip the jingle itself — future).
- CDC log lines now frame-stamped (psxport_frame). Reliable progress markers: cutscene
  start = "Setmode+ReadS" pair; load activity = ReadN/Pause cycles. The stream clock at
  0x8011824C is NOT a valid logo-end detector (byte-wraps, resets — misled two rounds).
- DEAD END (again, harder): consumer-pacing via pulling PSRCounter on ack — desyncs the
  pipe. The working form is forward-only conditional scheduling at delivery time.
- Driving pattern that worked: wide60rt -repl under a FIFO + holder process; run in
  chunks; bt/state/r/trace between; restart cheap (boot ~1s). Sessions are the new
  default workflow for game RE.

## 2026-06-13 — instant CD + fastboot OpenBIOS: game EXEC at frame 50; runtime is driveable
- **Boot chain now: ~50 frames to game EXEC** (retail-style ~4000, stock OpenBIOS ~700).
  Pieces: (1) fastboot OpenBIOS (FASTBOOT=1 upstream no-shell mode, built from a
  pcsx-redux sparse clone with mips64-linux-gnu cross gcc, FORMAT=elf32-tradlittlemips;
  scripts/build-openbios.sh; binary committed at bios/openbios-fast.bin);
  (2) instant-CD in the imported cdc.c, bitmask psxport_cd_instant (env
  PSXPORT_CD_INSTANT, default 0xF): 1=instant seeks (~2000cy incl. spin-up/pause),
  2=instant Reset (no random 0-3.25Mcy reset-seek - also a determinism hazard - and
  10kcy completion), 4=1ms disc startup delay, 8=fast data-read pacing 7000cy/sector
  (~64x; audio-paced CDDA/XA modes untouched); (3) beetle cd_access_method=precache
  (whole disc to RAM) - REQUIRED: beetle's threaded CD reader cond-wait wedges under
  instant request rates (host backtrace: CDIF_ReadRawSector/pthread_cond_wait).
- **DEAD ENDS (do not revisit):** ack-accelerated reads (pull PSRCounter on IRQ ack)
  desync the CDC sector pipe and wedge the BIOS bootstrap - twice. Holding the sector
  clock while IRQ pending degrades PS_CDC_Update to tiny chunks (livelock). 1000cy/
  sector pacing collides INT1s (consumer cannot ack between sectors). Beetle skip_bios
  hangs OpenBIOS (intercepts the retail shell); superseded by our fastboot OpenBIOS.
  The shell wait ("Data is acceptable", ~650 frames) was masking DiscStartupDelay.
- **Runtime is now driveable + self-debugging:** -repl mode (stdin/FIFO: run/r/w/cd/
  cdclog/trace/bt/state) so experiments need no rebuilds; TTY capture via PC hooks on
  the kernel A0/B0 putchar dispatchers (OpenBIOS narrates its whole boot); CDC cmd/IRQ
  log (PSXPORT_CDC_LOG); watchdog (SIGALRM, 5s no-frame-progress) dumps host backtrace
  + emulated GPRs + heuristic MIPS stack scan (jal-preceded return addresses) then
  kills. RAMHASH/GAMELOG/RAMDUMP/MEMWATCH/PCCOV env probes. Verified deterministic
  (RAM hashes identical across runs, full instant config).
- Tomba 2: engine running by frame ~1200 (GTE/card kernel patches logged), no hangs to
  4000+. Heartbeat hook (0x8003CCA4) verified plumbing-wise; fires only in real
  gameplay, which the X-mash script does not reach in this runtime (savestate support
  is the proper fix, TODO).
- User direction embedded in workflow: aggressive change + find-and-override on
  breakage; debug probes over screenshots; runtime must be interactive (REPL) and
  self-diagnosing (watchdog+stack traces). BIOS code is part of the project now.

## 2026-06-12 — runtime: hook layer + RE tooling; intro segments are LOAD MASKS
- Hook/override layer live in the runtime: per-instruction hook point in beetle's
  interpreter (patches/beetle-psx/0001, -DPSXPORT_HOOKS), registry in
  runtime/psxport_hooks.* (PC + expected-instruction signature -> native fn; REDIRECT
  return skips original code, resumes at chosen PC = native override). RE aids ported:
  PSXPORT_RAMDUMP (frame:path snapshots), PSXPORT_MEMWATCH (per-frame byte CSV),
  PSXPORT_PCCOV (executed-PC bitmaps over frame ranges). Per-game modules in
  runtime/games/ get RAM + per-frame tick + scoped input injection via main.cpp.
- **Tomba 2 intro: the license text and Whoopee Camp logo are NOT skippable — they are
  load masks.** Verified: scripted X presses change nothing (A/B identical timeline);
  PC-coverage diff shows the segment-end code is absent from RAM until the loader
  finishes (main overlay lands ~frame 1989 mid-logo; logo then runs out its jingle,
  stream clock at 0x8011824C freezes at 7776 at segment end ~2400). Poking the clock
  does nothing (it is an output, not the gate). DEAD END: do not look for an input
  check or a timer compare to patch.
- Implemented instead: scoped auto-turbo (8x, no presents/audio on skipped substeps)
  while main overlay absent (word @0x8005082C == 0) OR logo stream clock ticking,
  frame-capped. Ends exactly at the cutscene. No input injection (user correction:
  the X-mash "skip" was never wanted as automash).
- Beetle's skip_bios HANGS with OpenBIOS (intercepts the retail shell; game stalls at
  the logo) — -fastboot is opt-in, default off. 14x CD loading is safe and default.
- Tab = manual 8x fast-forward in play mode.

## 2026-06-12 — scope change #3: PC port via interpreter + overrides (Beetle/mednafen base)
- User direction: build the actual PC port — NOT static recomp; an interpreter+overrides
  design (native function overrides hooked by PC over an interpreted base), because the
  generic+matching tiers still flicker and the real fix is rendering new frames from
  interpolated state, which needs first-class control of the render path.
- Base: **Beetle PSX (mednafen) sources imported into our build** — vendor/beetle-psx
  submodule, runtime/Makefile reuses upstream Makefile.common source lists but compiles
  everything ourselves (no .so, no prebuilt core; user explicitly wants source import).
  Interpreter-only: HAVE_LIGHTREC=0. GPL-2: distributable with source, unlike the
  CC-BY-NC-ND DuckStation fork (which stays as lab/oracle).
- runtime/main.cpp: our host (libretro callbacks for now, to be replaced by native glue):
  headless, -frames/-dumpdir/-dumpinterval (PPM), -inputscript (same format as regtest),
  -bios dir. **VERIFIED: Tomba 2 boots and renders in-engine intro at frame ~4000 with
  OpenBIOS** (copied as scph5501.bin; SHA warning is benign). Built deps-free in ~1 min.
- PCSX-Redux was tried first and dropped: GPL-2 (fine) but heavy deps (luajit/luv/uv/
  ffmpeg) vs beetle's zero-dep build; user picked beetle.
- Next: port the hook layer (PC trace, pokes, GTE taps) into the imported cpu.cpp/gte.cpp;
  override dispatch table (PC -> native fn, signature-checked for overlays); then the
  first real override: Tomba 2 render entry at 60Hz with interpolated state.

## 2026-06-12 — per-game object-identity interpolation LIVE (both games)
- MatchFrames now has the per-game pass: objects matched across frames by mutual-nearest
  GTE translation (TRANSFORM_MATCH_RANGE 4096 L1), each matched object's prims paired in
  capture order with token equality, gate 160px (vs 48 generic), overriding the heuristic
  pairing. Identity-says-same but geometry-jumped pairs snap (match=-1), they don't fall
  back to the heuristic.
- Coverage: Crash Bash gameplay ~38% of draws tagged (the GTE-projected 3D models — the
  moving things that flicker), 57 objects, 100% TR continuity, ~95% of tagged matched.
  Tomba 2 gameplay ~10% tagged (NPC models; terrain is CPU-projected — confirmed: 152/500
  prim verts have NO recorded SXY within ±8px, so the engine projects terrain without
  per-vertex GTE) — but those 10% are exactly the characters. ~94% of tagged matched.
- **GTE tagging requires CPU = Interpreter** (swc2/mfc2 hooks are interpreter-only; the
  recompiler inlines them). Without it the generic tier still works, tagging is just 0.
  GUI users: Settings -> CPU -> Execution Mode -> Interpreter.
- Next for coverage: hook the game's CPU-side projection output path per game (Tomba 2
  terrain), rotation-matrix tracking for camera-motion-aware gates, per-game config files.

## 2026-06-12 — per-game tier started: GTE tagging + Tomba 2 cull-cone patch (user repro fixed)
- **Tomba 2 fisherman culling RE'd and patched.** Engine culls objects against a view
  cone narrower than the screen (six `slti $v0,$v1,0x350..0x370` threshold sites in the
  overlay enqueue function ~0x80077100-0x800776E0; v1 = cos-scaled dot/distance, below
  threshold = culled). Pokes in patches/tomba2/cull-widen.md scale thresholds by ~0.72:
  13 objects drawn vs 5 at the repro savestate, walk pop-ins 12 -> 5. Applied via new
  PSXPORT_POKE env (every-vblank RAM pokes, conditional "old:new" form for overlay
  safety) — the prototype of the per-game patch layer.
- RE toolchain built into the fork (all env-gated): -loadstate / -widescreen regtest
  flags, PSXPORT_TRACE_PC/-OUT (interpreter PC-hit logger: a0/a1/ra per hit + vblank),
  PSXPORT_WIDE60_RAMDUMP (2MB RAM snapshot for capstone disassembly offline),
  PSXPORT_WIDE60_TRDUMP (per-frame GTE transforms with writer pc/ra). Workflow that
  found the cull: trace draw-object entry (0x8003CCA4) per frame -> diff drawn-object
  sets across a scripted walk -> found live enqueue path (0x8007763C variant; NB the
  0x8007703C sibling never runs in this scene) -> disassemble backwards to the cone test.
- **GTE transform tagging tier (in progress):** TRX/TRY/TRZ ctc2 writes hooked (gte.cpp)
  = object world transforms: Tomba 2 has 110-125/frame, 100% frame-to-frame continuity
  (nearest-TR) — object identity is real. Linking transforms to prims: swc2/mfc2 SXY
  hooks keyed by VALUE (Tomba 2 memcpys prims from scratch buffers, so addresses don't
  survive; the packed coord word does). Status: only ~12% of draws tagged even with a
  +-2px probe (game nudges coords post-projection; demo tunnel mesh appears to be
  CPU-computed, not per-vertex GTE). Needs more work before it can drive matching.
- Matcher: added motion-coherence filter (matches whose displacement deviates >12px
  from the local median of +-3 arena-order neighbours snap instead of lerping) — kills
  ~8 wrong pairs/frame on Crash Bash, match rate stays 97%. Aimed at the user-reported
  vertex flicker; user verification pending.
- Tomba 2 in-engine (demo, real gameplay): generic matcher ~96% (1132/1172 typical).
  User-verified 60fps in the Qt GUI. Note: regtest headless to frame <12000 is all FMV
  for Tomba (draws=0) — gameplay tests need -loadstate or frames >20500.
- User direction: per-game quality (RE) is the priority, not generic-only.

## 2026-06-12 — REAL-TIME 60fps working in-emulator (patch 0002); user-verified on Tomba 2
- `gpu_wide60.{h,cpp}` in the fork: captures each logic frame's backend command list
  (tee at `GPUBackend::PushCommand`, draws carry DMA src addr + E5 offset from the GP0
  dispatch hook), segments by GP1(0x05) flips, matches adjacent frames (src-addr sort +
  difflib-style alignment on type/texcoord fingerprints, C++ port), and at the vblank
  after a flip replays a vertex-lerped copy of the frame's list (its own clear included),
  then the original one vblank later. Presented: A, lerp(A,B), B, lerp(B,C)... 60 fps,
  zero added latency, timing-invisible to the game (no FIFO/tick changes). Replays skip
  UpdateVRAM/CopyVRAM (stale uploads would clobber streamed textures) and re-apply live
  drawing-area/CLUT after. Enable: regtest `-wide60` or `PSXPORT_WIDE60=1` (Qt GUI too).
- **CRITICAL finding — E5 draw offsets DO alternate per double-buffer, bundled
  mid-packet.** All our offline tools (`interp_dump.py`, the E5 scans) only parse
  `words[0]` of each GP0 packet, so they never saw the E5s and compared raw
  (= buffer-relative) coords — the offline matcher worked BY ACCIDENT. At the backend,
  vertices are absolute (offset baked in) and alternate by e.g. y+256 per frame: gate in
  **buffer-relative space** (abs − that draw's E5 offset), rebase prev verts to the
  current offset before lerping. Before this fix: 0% matched (uniform disp ≈256);
  after: **97% matched** on Crash Bash gameplay (1637/1688 draws/frame), beats offline.
- Flip boundary: GP1 writes bypass the GP0 FIFO, so flip-at-GP1-time can cut the capture
  mid-frame; the boundary now fires once all words pushed before the GP1 are consumed
  (pushed-words counter). (In practice CB's FIFO was empty at GP1, but keep the guard.)
- Verified: consecutive presented frames all distinct in gameplay (vs identical pairs at
  30fps); interpolated frames visually clean; ~186 emu-fps headless SW renderer.
  **User-verified 60fps in Qt GUI on Tomba! 2** (built with `-DBUILD_QT_FRONTEND=ON`).
- Diagnostic tooling kept in the fork: `PSXPORT_WIDE60_DUMP=<csv>` dumps one frame-pair's
  sorted (addr, token, type, x, y) sequences for offline analysis; distance-2 arena-slot
  ground truth was used in-emulator to prove capture sanity (95% same-slot, disp ≤8).
- **Open issues:** (1) vertex flicker during play — generic-tier mismatches (wrong pairs
  within degenerate token runs lerping, borderline pairs alternating lerp/snap); the
  per-game GTE-transform-tagging tier is the designed fix, or matcher hysteresis.
  (2) Widescreen ineffective on Tomba 2 despite WidescreenHack=true + 16:9 (no gamedb
  override; likely the game bypasses standard GTE projection — needs the per-game tier).
  (3) Headless Tomba run showed draws=0 pre-frame-11900: that's the FMV region (demo
  starts ~20500), not a capture failure — GUI run interpolated fine.

## 2026-06-12 — object-identity matching via arena slots; 60fps PoC verified visually
- User direction: don't rely on screen-space nearest matching — use object identity.
  PSX-specific insight: matrices never reach the GPU (GTE is CPU-side), but the DMA
  linked list gives every GP0 packet's **RAM address**, and engines bump-allocate
  per-object prims from a **double-buffered arena** (measured: 0% address overlap
  between adjacent frames, 88.5% two frames apart → same slot = same prim at N↔N+2 =
  free ground truth).
- Fork change (patch 0001): GPU::DMAWrite records `SRCADDR <hex>` Comment packets in
  GPU dumps (the dump player ignores Comment packets, so dumps stay replayable).
- Matcher (in tools/interp_dump.py): sort draws by arena address (allocation order =
  entity iteration order), sequence-align (difflib) on position-independent mesh
  fingerprints = cmd byte + **texcoord low halves only**. KEY FINDING: clut/texpage
  high halves alternate with frame parity (the game double-buffers CLUTs) — including
  them drops adjacent-frame token overlap from 92.6% to 4%. Validated against the
  N↔N+2 address ground truth: **89.1% aligned, 99.2% correct** (Crash Bash gameplay).
  Naive rank pairing: 1%. Diff-align on (cmd,len) only: 18.7%. UV fingerprints with
  clut included: 3.2% coverage. (Dead ends — don't revisit.)
- tools/interp_dump.py converts a 30fps dump → 60fps dump (lerped in-between frames,
  abs-coordinate lerp across E5 offsets, 100px displacement gate snaps residual
  mismatches). Output replays cleanly through the real renderer (regtest replay mode);
  earlier screen-space-key version produced giant stretched-polygon artifacts, v2 has
  none visible. Comparison video: scratch/screenshots/crashbash-30v60.mp4.
- Tomba 2 with SRCADDR: arenas also double-buffered (d1 overlap 0%) but slots NOT
  stable at distance 2 (22.3% — allocator churns with streaming tunnel geometry), so
  slot-based ground truth is Crash-Bash-specific, not universal. The 51% "consistency"
  measured for Tomba 2 is against an invalid truth — ignore it. Fingerprint alignment
  covers 90.5%; visual check of interpolated frames is clean (no stretched polys).
  Videos: scratch/screenshots/{crashbash,tomba2}-30v60.mp4.
- Next: real-time implementation of this matcher in the fork (synthesize+present the
  interpolated frame at the vblank between game flips); widescreen tier; per-game
  validation tooling that doesn't depend on slot stability.

## 2026-06-12 — both games measured: 30 fps presented framerate (gameplay)
- Added `-inputscript` to regtest (in patch 0001): scripted pad-1 digital input,
  `<start> <end> <Button>` per line. Deterministic across runs — menu navigation
  scripted blind via screenshots worked reliably.
- **Detection method correction:** bucketing draw commands by vsync is misleading
  (submission spans vblanks → alternating big/small buckets). Ground truth is
  **GP1(0x05) display-start changes** (buffer flips) per vsync — added to
  gpudump_stats.py.
- **Crash Bash gameplay (Battle/Jungle Bash, 4 players active): 30 fps** — 64/64 flips
  at 2-vsync gaps. Menu also 30 fps. The "Crash Bash is 60fps" belief is false, at
  least for menu + this minigame. ~1200 draw cmds per logic frame in gameplay.
- **Tomba! 2 (attract DEMO, minecart, in-engine): 30 fps** — 73/75 flips at 2-vsync
  gaps (outliers = loading hiccup). ~1400 draw cmds max.
- Input scripts for reaching gameplay: `scratch/inputs-crashbash-gameplay.txt`
  (menu → Battle → 1P → Jungle Bash, gameplay from ~frame 11700);
  Tomba 2: title at ~3500, attract DEMO (in-engine minecart) ~frames 20500-24500 —
  no menu navigation needed for engine captures (`scratch/inputs-t2d.txt`).
- Tomba 2 cutscene-skip via Start did NOT work in scripted runs (FMV runs to ~18000
  then Loading → demo); reaching actual player-controlled gameplay still TODO.
- Dumps: `scratch/raw/crashbash-{menu,gameplay}.psxgpu`, `scratch/raw/tomba2-demo.psxgpu`.

## 2026-06-12 — oracle running; Crash Bash menu measured at 30 fps
- DuckStation regtest builds (prebuilt deps release-20260526 in `dep/prebuilt/`; system
  `extra-cmake-modules` required). Three fixes needed, kept as
  `patches/duckstation/0001-*.patch` (submodule gitlink stays clean upstream — apply
  patches after `submodule update`, see patches/duckstation/README.md).
- **No retail BIOS on this machine.** Using **OpenBIOS** (PCSX-Redux's open-source BIOS):
  downloaded from pcsx-redux GitHub Actions artifact "OpenBIOS" (gh api, needs auth),
  installed to `~/.local/share/duckstation/bios/openbios.bin`. Crash Bash boots fine with
  it (no fast-boot; menu reached ~frame 3500-4000). Keep a copy in `scratch/bios/`.
- GPU dump pipeline verified end-to-end: `duckstation-regtest -gpudump <path>
  -gpudumpstart N -gpudumpframes M -- <chd>` → `.psxgpu.zst` → `zstd -d` →
  `tools/gpudump_stats.py` (counts draw cmds/frame, hashes display lists, infers logic
  rate from identical-frame runs).
- **Measured: Crash Bash main menu renders every other vblank → 30 fps menu logic**,
  ~430 draw cmds per logic frame, all frames distinct (sequence: 0,429,0,430,...).
  Gameplay rate unknown — needs controller input to reach a minigame (regtest has no
  input mechanism yet; next fork feature: scripted input or memcard/savestate boot).
- Log level names are case-sensitive (`-log Info`, not `info`).
- regtest run perf: ~3000 emulated FPS headless software renderer — 4000 frames ≈ 1.4 s.

## 2026-06-12 — scope change #2: generic "wide60" layer (see CLAUDE.md)
- Refined goal: a *reusable* widescreen+60fps system for PSX games, logic untouched,
  per-game RE minimized. Architecture = generic tier (primitive-level display-list
  interpolation + DuckStation widescreen hack) + per-game tier (GTE transform tagging
  config + culling/HUD patches). Full rationale in CLAUDE.md.
- Framerates NOT verified for either game yet — the interpolator's logic-rate detector
  will measure them. Do not assume Crash Bash is 60 fps (earlier note retracted;
  user is unsure too).

## 2026-06-12 — DuckStation hook-point survey (code reading, pre-build)
- All primitives flow through a single backend command stream: `GPU::HandleRenderPolygonCommand`
  (`src/core/gpu.cpp:3063`) → `GPUBackend::NewDrawPolygonCommand` (`gpu.cpp:3229`) →
  video-thread queue of `GPUBackendDrawPolygonCommand` (+Line/Rectangle variants).
  This *is* the per-frame display list for the interpolator: capture per frame at this
  layer, match prims across frames, re-emit lerped copies on synthesized frames.
- Frame boundary: vblank handling in `gpu.cpp` CRTC state (~line 1637).
- `src/core/gpu_dump.cpp`: existing GP0 stream recorder → use it to dump real frames
  and prototype the primitive matcher OFFLINE before modifying the render loop.
- GTE register writes for the per-game tagging tier live in `src/core/gte.cpp`.

## 2026-06-12 — scope change #1: no recomp, patches + modified emulator
- DuckStation license is CC-BY-NC-ND-4.0 → modified fork must stay private; patches
  themselves are publishable.
- DuckStation regtest build: prebuilt deps downloaded (release-20260526, sha verified)
  into `dep/prebuilt/`. Configure blocked on missing system `extra-cmake-modules`
  (ECM, for Wayland) — needs `sudo dnf install extra-cmake-modules`.
- The recompiler/harness scaffolding below is superseded; discdump, disc provisioning,
  and the submodule remain in use.

## 2026-06-12 — project init
- Repo scaffolded per recomp-init: `recompiler/ runtime/ overrides/ harness/ tools/
  generated/(gitignored) scratch/(gitignored)`.
- DuckStation vendored as shallow submodule, pinned at `3a98566`.
- Disc: `Crash Bash (USA).chd` via `PSXPORT_DISC` in `.env` (gitignored).
- No chdman on this machine; CHD is read directly with DuckStation's vendored
  `dep/libchdr` — `tools/discdump` extracts SYSTEM.CNF + the boot executable to
  `scratch/bin/`.
- `tools/discdump` built and verified against the real disc:
  - Boot executable: `SCUS_945.70` (432128 bytes on disc, LBA 23).
  - PS-X EXE header: entry PC `0x8002E7B0`, load addr `0x80010000`, text size
    `0x69000` (430080 = file size − 2048 header), initial SP `0x801FFFF0`.
  - SYSTEM.CNF: `TCB = 4`, `EVENT = 16`, `STACK = 801FFF00`.
- Next: harness scaffolding — drive DuckStation headless as the oracle (savestate
  freeze/restore, fixed timebase, pinned input) BEFORE any recompilation. Then the
  R3000A recompiler skeleton targeting the extracted EXE.
- Phase-2 feature targets (recorded now, implemented only after faithful base):
  widescreen (projection-level) + interpolated 60 fps via per-object transform
  interpolation (n64recomp style).
