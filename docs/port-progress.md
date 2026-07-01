# Tomba2Engine — PORT PROGRESS TRACKER (the single source of truth, boot → gameplay)

**This file is the authoritative, ORDERED map of the engine port. Read it FIRST every session; UPDATE it in
the same commit whenever you own a function or learn a boundary fact. Goal: port the engine top-to-bottom in
EXECUTION ORDER — boot → menu → gameplay → per-frame — owning every ENGINE function as it runs, keeping only
game CONTENT as PSX.** Don't cherry-pick; advance the FRONTIER (§ "CURRENT FRONTIER" below).

Pointers (detail lives there, STATUS lives HERE): RE map `docs/engine_re.md`; findings/dead-ends
`docs/journal.md`; driving/automation `docs/driving-the-game.md`; render `docs/render-arch.md`; the boundary
& directives `CLAUDE.md`.

## How to own a function (the LOOP — same every time)
1. **Find it in EXECUTION ORDER** (this file's spine). Take the first node marked ☐ TODO at the frontier.
2. **RE it:** `tools/disas.py 0x<addr>` (function-scoped — it stops at `jr ra`; `--mem` = just loads/stores
   with resolved addr+WIDTH). Note args (a0=r[4], a1=r[5], …), every mem read/write (addr+width), the return
   (v0=r[2]), and any `jal` (callee = keep-as-reference or own-too). Record the RE in `engine_re.md`.
3. **Classify** (see § BOUNDARY): ENGINE → reimplement PC-native in `engine/`. CONTENT → leave PSX. PLATFORM
   → native platform service in `runtime/recomp/`.
4. **Reimplement** as `ov_<name>(Core* c)` (ABI `void(Core*)`). Read args from `c->r[]`, do the work via
   `c->mem_r*/w*`, set `c->r[2]`. Register with `rec_set_override(0x<addr>, ov_<name>)` (or
   `rec_set_interp_override_auto` for OVERLAY fns, flushed on unload). Add the .cpp to BOTH `tools/build_port.sh`
   and `run.sh` SRC lists.
5. **VERIFY by RUNNING the game (see § VERIFY).** There is NO oracle to diff against and NO override-OFF
   A/B build — env gating is banned (ONE PC-native behavior). Drive the live port through the REPL to the
   scene that exercises the function, then check the result:
   - **CONTENT-INTERFACE fns (the guest RAM the still-PSX content/cull/render reads):** inspect that RAM via
     the REPL (`r`/`rw`/`dumpram`) and reason about correctness against the recomp REFERENCE you read while
     porting. The output must be what the retained PSX content expects to read back; a wrong write corrupts
     the content (e.g. native terrain made Tomba fall through the ground, later-158).
   - **Pure ENGINE / RENDER result fns:** gate on the observable result — the world/menu/scene built and the
     picture drawn. The USER eyeballs a build (the agent builds, runs, sends a `shot`, and asks).
   - **ALWAYS confirm the override actually FIRES** (a `cfg_dbg` log on entry, enabled via REPL `debug`). A
     faithful override registered where it's never invoked looks fine while doing nothing (later-170 trap).
6. **Update THIS file** (status ☐→✅ + file/symbol) and `journal.md` (later-NNN), commit + push.

## VERIFY — drive the live PC game via the REPL (there is NO oracle to diff against)
The methodology changed (2026-06-20): the Beetle oracle (`wide60rt`), all diff/compare tooling (`gpu_differ`,
`drive.py`, `dualcore.py`, `ram_region_diff.py`, …), and every env A/B toggle are GONE. You verify by RUNNING
the ONE native port (`scratch/bin/tomba2_port`) and observing it — never by diffing two builds.

Drive it with the REPL (`PSXPORT_REPL=1`, commands piped on stdin; or the live TCP debug server
`PSXPORT_DEBUG_SERVER=1` + `tools/dbgclient.py` for a windowed run):
```
# headless, drive into the field, then inspect the state the function produces:
printf 'newgame\nskip 500\npress r\nrun 150\ndumpram scratch/bin/state.bin\nr 0x800bf4fa 4\nshot scratch/screenshots/now.ppm\nquit\n' \
  | PSXPORT_REPL=1 PSXPORT_VK_HEADLESS=1 scratch/bin/tomba2_port
```
Key REPL commands: `run N` (advance N frames), `newgame` (pulse to the GAME prologue), `skip N` (pulse Start
to push through the intro into the field), `press`/`release`/`tap <btn>` (input), `r`/`rw`/`w` (read/write
memory), `dumpram <path>` (+ a `.spad` scratchpad sidecar — so dumps cover scratchpad too), `shot <path>`
(VK-readback PPM, works headless), `debug <chans|all>` (enable diagnostic channels at runtime — replaces the
old `PSXPORT_DEBUG` env var), `stage`, `regs`, `seq`, `quit`.

Pick a scene that actually EXERCISES the output: the idle field exercises boot/steady systems, but a
MOTION scene (drive in then `press r` to free-roam walk) is needed for camera/anim accumulators — a stopped
scene leaves them unchanged and hides a bug. Confirm correctness by reading the relevant guest RAM / scene
state with `r`/`rw`/`dumpram` and comparing it to what the recomp REFERENCE says it should be; for the
picture, have the USER eyeball a build (`shot` + ask). Do NOT conclude from a single cherry-picked still —
verify on the running game. Tools that remain: `tools/disas.py` (RE a function), the REPL/debug-server
inspection commands above, and the F1 imgui overlay for live-tuning enhancements.

## SBS — the DIVERGENCE GATE (use this so you NEVER flounder on "no idea why") ★ READ THIS
The recurring failure mode — "something is wrong, no idea why", weeks lost — is now STRUCTURALLY SOLVED.
`PSXPORT_SBS=1` (runtime/recomp/sbs.cpp) runs TWO cores in ONE process, IN-PROCESS, and mechanically
pinpoints the FIRST place the native path diverges from the PSX path, with the exact corrupting-write
GUEST STACK TRACE. It is the self-contained oracle the old text said we lacked. **Gate EVERY subsystem
rebuild on it; never proceed on a vibe.**
- `PSXPORT_SBS_MODE=render` — A = native gameplay + NATIVE render, B = native gameplay + PSX render.
  Native render MUST leave guest RAM identical to PSX render → run it, drive `sbs diff` to ZERO. A nonzero
  diff = the native render is RUNNING CONTENT / corrupting guest state (it must read guest data, write only
  host memory). Each surfaced address is a leak to fix.
- `PSXPORT_SBS_MODE=gameplay` — A = native gameplay, B = PSX gameplay (psx_fallback), render identical.
  A native gameplay subsystem MUST match the PSX body BYTE-FOR-BYTE → `sbs diff` ZERO. First nonzero
  address + `sbs watch` + `sbs bt` = the exact instruction that drifted. This is the gate for owning any
  per-frame gameplay/AI/physics function.
- `PSXPORT_SBS_MODE=both` — full native vs full PSX.
- Both cores sync at the gameplay-start flag (barrier), then lockstep on identical input; the first
  divergence AUTO-PAUSES. Inspect over the debug server (`tools/dbgclient.py --port N`):
  `sbs status | diff | bt | watch [hex] | show a|b | resume | step [n]`. `sbs watch` arms a per-core
  write-watchpoint so the diverging WRITE re-pauses mid-frame with each core's exact guest backtrace —
  that backtrace IS the answer to "why". Run: `MAIN.EXE` as argv[1], disc via `PSXPORT_TOMBA2_DISC`
  (NOT the CHD as argv[1] — that loads the disc as a PS-EXE and derails).
- This works because the subsystems are now per-instance (no shared state between cores): Core (RAM/regs/
  scratchpad), and the Beetle singletons GTE/SPU/MDEC (GteRegs/SpuState/MdecState, bound per core
  frame-step). Keep it that way — any new global mutable machine state breaks the gate.

THE REBUILD LOOP (every subsystem, same every time): (1) reimplement it native; (2) pick the SBS mode
that isolates it (render-diff or gameplay-diff); (3) drive `sbs diff` to ZERO, fixing each surfaced
divergence at its `sbs bt` write-site — that is "knowing why"; (4) USER eyeballs the picture for render;
(5) commit. If you ever can't explain a behavior, you have not run the SBS yet — run it.

## THE BOUNDARY (one line; full text in CLAUDE.md)
PSX keeps ONLY game CONTENT: per-character/enemy **AI & behavior**, **physics/collision response**, **level
data**, **quest/event/progression rules**. EVERYTHING else is PC-owned (engine): boot, menus, loading, scene/
world/object model, placement/spawn, camera, projection, **render ordering & the renderer**, save/load, 60fps.
The recomp body is the behavioral ORACLE for engine fns (reimplement, don't transcribe) and the live RUNTIME
for content fns (call it). Do NOT mimic PSX hardware (GTE/GP0/OT) — remove Beetle by porting its CALLERS.

---

# THE EXECUTION SPINE — boot → gameplay  (✅ owned · ◐ partial/staged · ☐ todo · ⬛ keep-PSX content)

## A. Boot / init  (MAIN.EXE resident)
- ✅ `FUN_800896E0` crt0 (SP/gp/heap/BSS) — platform, via `native_boot.cpp`.
- ✅ `FUN_80050b08` main = `ov_game_main` — `native_boot.cpp`.
- Init prefix of ov_game_main (classified in engine_re.md table):
  - ✅ platform stubs: CdInit, ResetGraph, SetDispMask, VSync, SPU/sound, DMA/pad — native platform services.
  - ✅ `FUN_80050a0c` frame-state init = `eng_init_framestate` (engine_init.cpp).
  - ✅ `FUN_800509b4` display + GTE projection = `eng_init_display` (engine_init.cpp).
  - ✅ `FUN_80050a80` camera init = `eng_init_camera` (engine_init.cpp).
  - ✅ `FUN_80075130` **font / text system init** = `ov_font_init` (engine_font.cpp) — ORCHESTRATION + direct
    writes + the 3 engine-state callees OWNED PC-native; the 8 LIBGPU/sound callees KEPT as `rec_dispatch`
    in-context (later-182b nested-dispatch risk). Owned: `ov_font_bank_select` (FUN_800963a0, store
    0x80105cec), `ov_font_bank2_store` (FUN_80096370, store 0x80105d28), `ov_font_glyphclass_fill`
    (FUN_800752b4, 24-entry table @0x800be238+8), plus the direct writes (0x800bed78/bed80, 0x800be358 +
    14× sh loop @0x800be3d6.., the sp+16 FntOpen struct, 0x800be22a/b). Mirrors the sp-48 frame so the
    dispatched FntOpen (0x80098330/0x80098d30) reads its struct at sp+16. VERIFIED: called directly from
    native_boot (replaces rc0); boot dump (run 2) main-RAM + scratchpad 0-diff vs scratch/bin/initref.bin.
    Registered in game_tomba2.cpp (orchestrator + 3 callees) for any other dispatcher.
  - ✅ `FUN_80096590` **per-area BAV effect/animation CEL LOADER** = `ov_bav_load` (engine/engine_bav.cpp,
    later-207) — the asset-loading subsystem that loads an area's effect cels (e.g. the seaside walking-dust).
    ORCHESTRATION + descriptor parse + ALL cel-system global writes OWNED PC-native; the genuine VRAM
    allocator/upload **callback** (a2 = FUN_800964b4 → allocator FUN_800977c0) KEPT `rec_dispatch` in the
    recomp's exact order (VRAM layout + timing identical). Owns: slot alloc (16-slot state table 0x80105D18,
    refcount 0x80105D70), the 16-byte cel-record + UV-table parse, the per-frame tpage/clut writes (rec+12/14),
    and the slot-keyed globals 0x80105C10/C50/C98 (data/desc/UV-base, index = slot*4), 0x80105CDA (64/128
    clamp), 0x80105CF0, 0x80105D30 (size), 0x80105D78 (VRAM base), 0x800AC638 (lock, 1=free/0=busy). BAV
    descriptor + cel-record + global layout fully RE'd in engine_re.md "BAV cel loader". VERIFIED 0-diff via
    the `bavload` full RAM+scratchpad+v0 A/B gate (native→snapshot+rollback→rec_super_call→diff, excl.
    [sp-0x800,sp)): **0 mismatches over all 3 area-entry cel loads** in the reachable seaside field (a
    spawn-time loader, not per-frame). 3 GOTCHAs caught by the gate: lock inverted (delay-slot `sw zero`),
    kind-shift inverted (`kind<5`→`<<2`), and the C10/C98 index = slot*4 (sll-s2,16>>14 idiom, NOT field18>>14).
    Registered in game_tomba2.cpp.
  - ◐ `FUN_800520e0` **engine subsystem init** = `eng_init_subsystems` (engine_init.cpp) — ORCHESTRATION owned
    PC-native (later-183): the 6 direct engine-flag writes (*0x800bf4fa=0xffff, *0x800ecf4a/4c/4d/4e/4f=0) +
    the 4-callee sequence. Boot A/B (frame 50) main-RAM + scratchpad 0-diff. Callee descent:
    ✅ `FUN_8007b328` entity-pool init = `eng_init_entity_pool` (later-183b, boot A/B 0-diff: the 8-byte header
    @0x800fb160 + FUN_8007b2c0(0) scratchpad scale params, both inlined native).
    ✅ `FUN_80086620` mode ctrl = `eng_init_mode_ctrl` (later-184: full logic reimplemented native; verified a
    NO-OP at the init call — a0=1 with both mode flags 0x800abe88/8c + counters still BSS-zero → early-return;
    boot RAM+scratchpad IDENTICAL to the recomp-running reference dump).
    ✅ `FUN_80087a60`→`FUN_80086970` input init = `eng_init_input` (later-184: own the direct writes + the
    7-call orchestration native — FUN_80080890/80087400/800873f0/80085b10/800808a0 + the dispatch handler
    *0x800abe3c twice — rec_dispatched in-context; boot RAM+scratchpad IDENTICAL to reference).
    ✅ `FUN_80088b00` allocator/dispatch-table = `eng_init_alloc` (later-184: install the 6-entry mode
    dispatch table 0x800abe38.. + build the 480-byte heap @0x80102500 native, rec_dispatch the 3 callees
    FUN_80089160/8009a340/80086738 in-context; boot RAM+scratchpad IDENTICAL to reference). **FUN_800520e0
    is now FULLY OWNED — all callees native.** (Reference-cmp caught a lui+addiu sign-extension slip in the
    table addrs — 0x8009 lui + negative addiu = 0x8008xxxx — which is exactly the gate working.)
  - ◐ `FUN_80051e00` scheduler task-table init (rc0'd at boot, native_boot.cpp:371) + `FUN_80051f14` register
    task 0. The native frame loop replaces the per-frame stepper.
- ✅ Native cooperative frame loop `native_scheduler_step` (replaces `FUN_80051e60`) — `native_boot.cpp`.
  Thread/critical-section primitives owned: ov_open/close/change_thread, ov_switch (0x80080880).

## B. Stage sequencer / loader  (task 0)
- ✅ `FUN_800499e8` task-0 bootstrap = `native_task0_bootstrap` (native_boot.cpp) — resolves \BIN\START.BIN
  via the native ISO9660 resolver `disc_find_file` (disc.c, CdSearchFile replacement), records its {LBA,size}
  at 0x800be1e0, then switches task 0 to stage 0. **Owned PC-native top-down (override system removed);
  replaces the old override-based `ov_task0_boot`.** (later-211)
- ✅ `FUN_80052078` stage transition = `native_start_stage` (native_boot.cpp) — overlay load + display/BIOS reset.
- ✅ `FUN_800450bc` overlay LOADER = `native_load_overlay` (native_boot.cpp) — `cd_loadfile_native(0x80106228,
  LBA, size)` + sets task restart PC from the stage-entry table 0x800a3ecc[stage].
- ✅ CD loadfile path native (ov_cd_loadfile 0x8001DB8C synchronous; `cd_loadfile_native` direct-call wrapper) — `cd_override.cpp`.

## C. Stages (task 0 cycles START → DEMO → GAME)
- ✅ **START.BIN stage-0 `0x8010649c`** — boot bootstrap overlay: file-table builder + the 4-state area/asset
  PRELOAD SM, NOW OWNED PC-native + SYNCHRONOUS (native_boot.cpp `ov_start_bin_stage` → `native_stage0_sm`).
  File-table BUILDER native (later-211, `disc_find_file`, 0 not-found / 0 CD timeouts). The SM (overlay
  0x80106728) is reimplemented in C — no coro-redirect to PSX, no async: states run in order, then
  `native_start_stage(c,1)` → DEMO + `ov_switch` yield (keeps the slot's state=3 restart).
  - The async hang is GONE. Reuses the existing leaf natives the **code map** surfaced (don't re-derive):
    `cd_loadfile_native` (the sync replacement for the async CD reads `0x8001DB8C`/`0x8001DC40`), `ov_unpack_group`
    (FUN_80044E84, now calling `ov_upload_image` direct, not rec_dispatch). Orchestration reimplemented sync:
    `preload_texgroup` (=FUN_80044F58 header+archive+unpack+42-word meta), `preload_stage1` (=FUN_8004514c
    SWDATA+DAT+relocation), `preload_build_vram` (=FUN_800754f4), `preload_cel` (=FUN_800753d4 alloc+kick, async
    DMA-poll DROPPED — native upload is synchronous).
  - VERIFIED: reaches DEMO (`stage=DEMO(0x801062E4)`) with no hang; content-interface tables populated sane
    (meta 0x800FB170 file-shaped, 0x1F80022C=0x801886F4, 0x800ED014=0x8017C800). FOLLOW-UP: `FUN_80096480`
    returns -1 (no slot) for the 2nd cel — verify the cel lands in VRAM once DEMO renders (USER eyeball).
- ◐ **DEMO stage `0x801062E4`** = TITLE / front-end MENU state machine (8 substates) — **the big front-end
  engine system** (engine/engine_demo.cpp, AUTO-registered via demo_scan_overlay). **NOW REACHED from boot
  (stage-0 preload owned, above).**
  - ✅ **DEMO ENTRY `0x801062E4` + substate s0 OWNED PC-native + SYNCHRONOUS** (`ov_demo_stage_main`,
    engine_demo.cpp; wired into the native scheduler in native_boot.cpp like 0x8010649C/0x8010637C). The
    prologue (UI/ctx init 0x800810F0 + input reset 0x8005082C + the field inits, faithful to the 0x801062E4
    disasm) runs native, then s0 (0x801063C0, run-once menu-resource INIT) runs native+SYNC: loader 0x80045080
    via `cd_dc40_sync` (new, cd_override.cpp — the inline FUN_8001DC40 sync read) + area-load 0x80044BD4(cb=
    FUN_80044F58,2,0) via `preload_texgroup(c,2,0)` (reused from stage-0, now non-static) + the SYNC tail calls
    0x8007982C/0x80075240/0x8001CF00. Sets sm[0x48]=1, coro-redirects to the guest loop @0x80106388 (starts at
    s1). **VERIFIED interface-correct: the 42-word texgroup metadata @0x800FB170 (the table the content reads
    back) is a BYTE-EXACT match vs the reference steady-menu RAM dump.** WHY native+sync: run pure-PSX, s0's
    loaders descend into the IRQ-driven async reader (FUN_8001D940 via FUN_8001DC40 / the FUN_80044BD4 task-1
    spawn) which our no-IRQ runtime never completes → infinite spin in libcd's VSync-timed `CD_cw`.
  - ✅ **DEMO runs as a NATIVE PER-FRAME LOOP** (`ov_demo_frame`, engine_demo.cpp; scheduler integration in
    native_boot.cpp `native_scheduler_step` + `SchedulerState::demo_native[3]`). The guest root loop
    @0x80106388 (jr-v0 substate dispatch) can no longer be intercepted (override table gone), so the DEMO
    task is driven as a STATELESS native per-frame dispatcher: each frame the scheduler calls ov_demo_frame,
    which reads sm[0x48] and runs that substate's one-frame work natively, then bumps the frame counter
    0x1F800198. No coroutine/ucontext — substates are synchronous, so "yield" is just returning; the slot is
    re-armed to state 2 each frame. A `setjmp(yield_jmp)` guard contains any cooperative yield from a
    rec_dispatch'd substate (so a not-yet-sync CD load can't corrupt the scheduler via a stale longjmp).
    Leaving DEMO→GAME re-registers the slot with a non-DEMO entry, clearing demo_native.
  - ✅ **substate s1 (0x8010641C) + its inner menu machine (0x80106F80) states 0..3 native.** s1 runs the
    menu machine; on completion (v0!=0) advances sm[0x48] 1→2; on a pad edge sets the skip flag. The menu
    machine (a real jr-ra function) is owned by special-casing its libcd states and rec_dispatching the
    CD-free ones: state 0 (0x80106FF0, CdControlB Setmode 0x80) → native no-op (our disc reads by LBA need
    no drive mode) + advance; states 1..3 (0x80107034, pure sm[0x4a]++) → rec_dispatch. VERIFIED: boot
    reaches DEMO, runs the native loop, advances sm[0x4a] 0→1→2→3 with NO hang, `run N` completes cleanly.
  - ✅ **s1's menu machine FULLY owned incl. the OP.STR opening-movie sub-sequence; reaches s2.** demo_menu_
    machine states ≥4 (the movie load/stream/post/teardown) are owned natively: `native_fmv_play(c,
    "MOVIE/OP.STR")` (skipped under NO_FMV/headless) + a faithful replication of state-7 teardown (the sync
    libgs/SPU/cleanup callees + CdlPause→native no-op + clear 0x1F80019C/9D), returning nonzero so s1 advances
    to s2. **VERIFIED: no hang, sm[0x48] advances 1→2.**
  - ✅ **substate s2 (0x80106464) native** (`demo_frame_s2`, ov_demo_frame case 2): title/menu sub-machine
    0x8010696C (intro timer sm[0x5a]=450) + cursor two-phase transitions, ending with TAIL_REND (attract
    render 0x80075A80). **VERIFIED: stable 120+ frames in s2, no hang.**
  - ☐ **NEW FRONTIER (1) — the title/front-end renders BLACK.** s2 runs the attract render (0x80075A80) every
    frame but the headless `shot` is all-black (0% non-black). This is now a RENDERING issue, not a hang —
    needs the gfx-debug methodology (LIVE game + `debug scene/provat/vkvram` + USER eyeball; the agent can't
    self-verify a render). Suspects: (a) the attract render's display-list/OT isn't reaching the native VK
    present; (b) skipping the movie's frame-decode states (5/6) left the title without a background/camera
    setup it expected (interface-correctness of the FMV teardown shortcut); (c) the title genuinely needs the
    movie's last frame. Check the scene/submit state on the running port first. See docs/gfx-debug.md.
  - ☐ **FRONTIER (2) — substates s3..s7 still need native conversion** (ov_demo_frame default case = parked
    no-op). s3/s6 have logic in the old coro-redirect ov_demo_s3/s6 (convert like s2); s4/s5 were "stays
    guest" under the old model (re-RE for the native loop); s7 = attract (plays OP.STR again, reuse the FMV
    path). Each is a `case` in ov_demo_frame. Reach s7 by letting s2's intro timer (450) expire (~run 450).
  - (historical) menu-machine states ≥4 = the OPENING MOVIE + attract sequence (FMV subsystem):
    DISCOVERED: state 4 (0x80107054) passes descriptor @0x80106254 = the STRING **`\MOVIE\OP.STR;1`** (dest
    0x8018A000, size 0x7E5) to **`0x8008B8F0`** (libcd file-by-name queue op → 0x8008BBE8 → 0x8008C1EC; the
    "time out in strNext()" error string sits right after the filename @0x80106264). So the DEMO front-end's
    sm[0x4a]≥4 states stream/play the **opening .STR movie** (then attract). That is why the title is black
    under PSXPORT_NO_FMV (the title sequence IS the OP.STR movie + attract, not a static title screen). This
    is the FMV subsystem, NOT a menu-resource load — own it via the native .STR player (runtime/recomp/
    native_fmv.cpp) instead of cd_loadfile_native, and honor the NO_FMV skip (advance sm[0x4a] past the movie
    states). Currently PARKED cleanly at sm[0x4a]≥4 (no hang). NEXT: wire native_fmv into ov_demo_frame's s1
    menu machine (play OP.STR, or skip+advance under NO_FMV), then the attract states, then s2.. (s2 = the
    real front-end/title display, TAIL_REND attract render 0x80075A80). Each new substate is a `case` in
    ov_demo_frame. Tools: `debug demo`, `debug cdc` (dynamic), PSXPORT_WATCHDOG=<sec>, native_fmv's own path.
    - **DECISION (user, 2026-06-22): INTEGRATE the native FMV player** (not skip-only). Plan + the RE'd movie
      state map (menu machine 0x80106F80, sm[0x4a]):
      * state 4 (0x80107054): start load — descriptor 0x80106254 = `\MOVIE\OP.STR;1`, size word table
        @0x80107718[0]={0x80106254,0x8018A000,0x07E5}; CdControlB 0x8008B8F0; sets *0x1F80019C=1; sm[0x4a]→5.
      * state 5 (0x801070A4): stream a chunk via **strNext 0x8010755C**(a0=0x1f800000, a1=0x07E5 sectors);
        v0!=-1 → sm[0x4a]→6 (more); v0==-1 → print "time out in strNext()" + sm[0x4a]→7. (0x80107400/0x801074BC
        are the STR/MDEC decode setup.)
      * state 6 (0x80107144): post (reads scratchpad 0x1F800008/14; libgs 0x8009C784/800 draws the decoded frame).
      * state 7 (0x80107280): teardown — 0x8001CF00 (SPU mix), libgs 0x8009C820/8BC, 0x8008CD40, 0x8008CCE0
        (=ov_8008CCE0, native exists), **CdControlB 0x80089E1C (CD Pause) busy-loop**, then clears *0x1F80019C
        and *0x1F80019D, j 0x801072DC → returns v0!=0 (the Pause result) ⇒ s1 advances sm[0x48] 1→2 (to s2).
      * INTEGRATION: in demo_menu_machine, replace states 4..7 with: `native_fmv_play(c,"MOVIE/OP.STR")` (it
        already plays LOGO/OP at boot in native_boot.cpp ~L969; respect the NO_FMV/headless skip), then
        replicate state 7's OBSERVABLE teardown (clear 0x1F80019C/9D, run the SYNC libgs/SPU cleanup callees,
        CD-Pause = native no-op) and return nonzero so s1 advances to s2. **INTERFACE-CORRECTNESS RISK:** s2
        reads what states 6/7 set up (libgs draw env, scratchpad) — verify s2 renders correctly, don't blindly
        skip. native_fmv_play is SYNCHRONOUS (blocks the whole movie in one ov_demo_frame call — fine for an
        attract movie). Next after this: substate s2 (the front-end/title display — TAIL_REND 0x80075A80).
  - FOLLOW-UP (pre-existing, stage-0): the 2nd cel slot @0x800BED82 reads FFFF (no slot) vs reference 0001 —
    `preload_cel`/FUN_80096480 returns -1 for the 2nd cel. Verify the cel lands in VRAM once DEMO renders.
  Substate ownership:
  - ✅ **s0/s1/s2/s3/s6** (run-once INIT + the title/menu input substates; later-182/185, coro-redirect
    handshake on each substate body, A/B 0-diff at a steady menu frame).
  - ✅ **s7 0x80106668 attract-demo launch** = `ov_demo_s7_phase` on the phase machine 0x80106C24 (later-208).
    Owns phase SELECTION + phase2 teardown native, coro-redirects the deep-yielding phase0/phase1 into the
    guest body (prologue-frame replication). REACH: `tap 4008` (title s2->s3) then `run ~455` (s3 intro timer
    expires -> sm[0x48]->7 auto-launch). VERIFIED: steady-phase1 full-RAM+scratchpad dump vs guest baseline =
    scratchpad 0-diff, main-RAM diff only the 2-byte coro saved-ra slot (top-of-RAM stack, never game data);
    phase0/phase1 clean 2000+ frames; phase2 (poke sm[0x4a]=2) restarts sm[0x48]->0 with no crash.
  - ✅ **s4 0x80106580 — LOAD GAME, OWNED native (later-221, `demo_frame_s4`+`load_machine_s4`).** Was
    UNHANDLED in the native per-frame loop (`default` case) → silent spin = the USER-reported "Load Game
    freezes". Reimplements the load sub-machine 0x8007bf20 native+SYNC (case-0 disc load FUN_80045558(1) of
    the load-menu OVERLAY to 0x8018a000 done via cd_dc40_sync; the resident UI driver FUN_8007be18 → the
    overlay slot browser FUN_8018fa88/fbcc rec_dispatched — its memcard frame R/W is already sync+instant via
    the BIOS B0/A0 card HLE). Routes on sm[0x6b]: ==1/2 → s2 (title); ==7 → s5 (GAME), *0x1f800134=1. GOTCHA:
    root prologue loads s2=1,s1=2,s3=3 → `sh s1,0x48`=sm[0x48]=2 (TITLE), not 1 (would replay the OP movie).
    VERIFIED: renders the PS1 card screen, Return → title, no freeze/derail. (The old "STAY GUEST" note was a
    pre-override-removal artifact — s4 IS ownable in the native per-frame dispatcher.)
  - ⬛ **s5 0x801065DC — owned via `demo_frame_s5`** (`jal 0x80052078(2)` leave-demo → GAME). (later-185/208/212.)
- GAME stage `0x8010637C` (overlay GAME.BIN). **NATIVE OWNERSHIP ENDS HERE AT THE PROLOGUE** — full map
  `scratch/gameplay_start_flow_re.md` (read it for the gameplay-start flow + the next-step plan).
  - ✅ top-level prologue = `ov_game_stage_main` (LIVE — called directly by `native_scheduler_step`), then
    `rec_coro_redirect`s to the GUEST loop body 0x801063F4 ⇒ the per-frame loop + sm[0x48] dispatch + the
    SOP field-mode machine all run as RECOMP from here down.
  - ⚠ STALE-MARKER CORRECTION: `ov_game_s48_0/1/2` + `ov_game_s4c` are written natively BUT **UNWIRED /
    ORPHANED** since the override-table removal (2026-06-22): `stage_scan_overlay` is a no-op and the defs
    are `(void)`-cast (engine_stage.cpp:188). Correct ready-to-wire REFERENCE bodies, not live code; the
    live guest loop calls the GUEST handlers (0x801086e0/720/784), not these.
  - ✅ **(0) SOP area-DATA load OWNED native+sync** (`native_sop_area_load`, engine/sop.cpp; later-217b) —
    reimplements LAB_80109164 (4 sync CD reads via ov_cd_dc40 + unpack FUN_80044e84 + collision load
    FUN_80045258 + ecf58 reloc patch + 1f80019b=1), dropping the FUN_80051fb4 task-yield. Wired as a native
    slot-1 task-entry interception in native_scheduler_step (keyed on entry 0x80109164). VERIFIED: newgame →
    sm[0x50] 0→1→2 (gameplay) at the SAME frames as baseline (f61), 1f80019b=1, ecf58 patched 8 entries,
    stable 220 frames, zero derail. This is the sync-load fn the native SOP state-0 will call inline.
  - ✅ **GAME stage now driven by a NATIVE PER-FRAME LOOP** (later-217c) — steps (1)(2)(3) DONE. The
    `game_native` path in `native_scheduler_step` (mirrors `demo_native`) runs `ov_game_stage_prologue`
    (split out of ov_game_stage_main; the old coro-redirect-to-guest-loop path is retired) then
    `ov_game_frame` each frame. **NOW NATIVE & LIVE-WIRED:** the per-frame loop (`ov_game_frame`), the
    sm[0x48] dispatch (`ov_game_s48_0/1/2_frame` — the orphaned handlers are re-wired), the GAME→SOP
    bridge 0x8010882c (`ov_game_submode0`), and the **SOP field-mode machine 0x80109450**
    (`ov_sop_field_mode`, engine/sop.cpp — the full sm[0x50] LOAD→FADE→GAMEPLAY→RESET switch; state-0
    calls `native_sop_area_load` INLINE so it never yields). Heavy per-state callees (FUN_801092b4
    per-frame field update, BG init, the per-scene object handlers) stay rec_dispatched as content/leaves.
    VERIFIED: newgame → sm[0x50] 0→1→2 (gameplay) by frame 60 (a frame faster than the cooperative
    baseline — load is inline now), 1f80019b=1, stable 300 frames, zero derail/hang/caught-yield, render
    99.9% non-black (USER eyeball pending). Map: `scratch/gameplay_start_flow_re.md`.
  - ✅ **FIELD AREA MACHINE (sm[0x4a]==1) now NATIVE per-frame** (later-217g). The walkable field that
    runs AFTER the SOP intro is dispatched by GAME.BIN **0x801088d8** (handler[1], NOT 0x80106478 — the
    handoff's "area machine 0x80106478" was wrong; the live path is sm[0x4a]==1). Owned `ov_game_submode1`
    (engine_stage.cpp): a switch on sm[0x4c] (table @0x80106334, 7 states). State-0 calls
    **`native_transition_area_load`** (engine/sop.cpp) — a faithful native+SYNC transcription of the
    cooperative load body 0x800452c0 (the FUN_80044bd4(0x800452c0,…) spawn-and-wait), dropping the
    FUN_80051fb4 task-completes + the CD-settle/audio-busy yields (no-ops in our sync runtime) — then falls
    through to state-1's next-state select. States 2..6 rec_dispatch the field RUNNING sub-machine
    (0x80106b98/70b4/7230/766c/7790, sm[0x4e]) — a transitive jal-scan proved them YIELD-FREE.
    `ov_game_frame` now returns 1 for sm[0x4a]==1, so the **cooperative fallback is GONE** for the field.
    VERIFIED: `skip 400` → field reaches sm[0x4a]=1 sm[0x4c]=2 overlay 0x801138A4, sm[0x4e] cycles
    0/9/10/7/8/6/1 EXACTLY as the cooperative baseline, stable 200 frames, ZERO "[sched] cooperative" /
    caught-yield / derail; clean newgame unaffected. KEY RE GOTCHA: 0x800452c0's `sb v0` at 0x800453d8 is
    in the jal DELAY SLOT → it stores the OLD v0 (sm[0x6e]), NOT the FUN_80045080 return (a 168-byte load
    size that derailed the BGM jump-table when mis-stored).
  - ✅ **FIELD per-frame RENDER owned PC-native (later-225, engine/engine_render.cpp).** The native
    ov_field_frame / ov_field_frame_x now call `ov_render_frame` (0x8003f9a8) / `ov_render_frame_x`
    (0x8003fa44) DIRECTLY (was rec_dispatch). The orchestrator wires the 4 per-object render-queue WALKS
    (0x8003bf00/eec0/bb50/bcf4 = ov_rwalk_aux_bf00 / ov_rwalk_aux_eec0 / ov_render_walk_snapshot /
    ov_rwalk_aux_bcf4 — previously ORPHAN natives in engine_submit.cpp) back into the LIVE render; each
    attaches the object's PC-native WORLD-POSITION depth (engine-owned ordering, not the PSX OT). 0x8003b588
    (diagnostic-only) + the non-walk passes stay rec_dispatch. VERIFIED: seaside field renders correctly in
    WIDESCREEN, stable 320+ frames, zero derail (render_final.png; USER eyeball pending). CORRECTS later-224:
    the live render driver is 0x8003f9a8 (called by ov_field_frame), NOT the overlay SM 0x8010810c.
  - ✅ **per-object VERTEX PROJECTION now world-coord float (later-226).** The render walks dispatch the
    per-object render through native `submit_perobj_render` (0x8003cca4) → native `submit_perobj_flush`,
    which composes camera×object in FLOAT from real world coords (eproj) and projects every vertex via
    `eproj_vertex_active` — NO GTE for the picture. VERIFIED: field renders identically to GTE + correct
    under a moved camera; `debug eproj` confirms native compose fires (was 0× orphaned). The dormant
    later-224 foundation is LIVE.
  - ✅ **master render-list walk 0x8003c048 + resident terrain 0x8002AB5C owned native (later-227).** The
    orchestrator calls ov_render_walk; the terrain default-case routes to ov_terrain → terrain_render_pc
    (float, real depth, NO GTE). VERIFIED at seaside (static + motion). At seaside 0x8002AB5C is only 1 quad;
    the main GROUND is the overlay drawer 0x8013e9d8 (still PSX) — next.
  - ✅ **main seaside GROUND owned world-coord (later-228).** The overlay ground/BG node renderer 0x8013E9D8
    (ov_bg_render) is native + routed from the master walk; its 12 ground render-commands flow through native
    submit_perobj_flush (world-coord eproj). VERIFIED byte-identical to GTE + correct under motion. Per-object
    mesh + resident terrain + main ground are ALL world-coord float now (no GTE for those pictures).
  - ☐ NEXT (render): own 0x8003c8f4 (billboard-quad pickups, 0x8003c2d4/c464) projection via eproj to finish
    the per-object render GTE-free; audit remaining RTPS/RTPT callers at the field; 0x8003cdd8 secondary cases.
  - ☐ **FADE — own the cutscene/area screen-FADE PC-native (DESIGN COMPLETE, 2026-06-25; user-priority).**
    ROOT CAUSE of the "renderer struggling with cutscene fades": the fade is DELIVERED as a PSX full-screen
    semi-transparent OT RECT (`FUN_8007e9c8`: cmd 0x62, (0,0) 320x240, into OT @0x800ed8c8) and the PC
    renderer must HEURISTICALLY re-detect it (`gpu_native.cpp` `fade_full_2d`, ≥¾-screen test → force to top
    2D band) — fragile, and the prohibited "read the PSX OT" pattern. Two callers, BOTH now native: the SOP
    intro SM `ov_bg_scene_transition_sm` (fade_rect→FUN_8007e9c8) and the area-transition fade
    `engine_stage.cpp` case 10 (`d3(0x8007e9c8, (u<<16|u<<8|u), 0, 0)`).
    **BLEND SEMANTICS (RE'd from FUN_80083de0 — the E1 draw-mode word's ABR field, bits 5-6):**
    `FUN_8007e9c8(color, a1, a2)` sets a3=0x20 when a1!=0 else 0x40; FUN_80083de0 puts a3 into the E1 ABR →
    a3=0x20 ⇒ ABR=1 = **ADDITIVE (B+F)**, a3=0x40 ⇒ ABR=2 = **SUBTRACTIVE (B−F)**. So `a1==0` ⇒ subtractive =
    fade to/from BLACK; `a1!=0` ⇒ additive = fade to/from WHITE. The fade op is therefore a per-channel
    ADD or SUBTRACT of the rect's RGB, CLAMPed — NOT rgb*brightness. (Intro SM a1=P[3]: 0=black-fade,
    1=white-fade; area-transition a1=0 = black-fade. color grey u or 0xffffff.)
    **FIX:** own `FUN_8007e9c8` PC-native — instead of building the OT rect, store an engine fade global
    {r,g,b, mode(add/sub), active}; clear `active` at frame start; gpu_gpu applies `clamp(frame ± color)` to
    the finished image. CRITICAL (headless/windowed parity): apply into `s_tex` (read by BOTH the present
    blit AND the `shot` readback, gpu_gpu.cpp ~1946/2020) so a headless screenshot matches the window — NOT
    only in present.frag. Then delete the `fade_full_2d` OT heuristic (verify the pause-dim #21 path first).
    Owning FUN_8007e9c8 fixes BOTH fade callers at once. Verify = USER eyeball a headless fade-ramp shot
    (trigger via the area-transition force, or the intro). NB this will break the bgscenesmverify gate's
    rect-region compare (expected — the fade delivery changed by design).
  - ✅ SOP per-frame field update's FIRST leaf owned: `FUN_8002655c` = `ov_bg_scene_transition_sm`
    (engine/bg_scene_transition_sm.cpp, 2026-06-25) — the SOP intro BG scene-transition / screen-FADE state
    machine (states 0-4 over struct 0x80100400; driven by DAT_1f800236 + the 0x800bf80f direction byte;
    fade rect via the still-PSX FUN_8007e9c8 leaf). Wired in sop.cpp:192 (was rec_dispatch). Verified
    `bgscenesmverify` 0 mismatch: states 0/1/2 during the AUTO_SKIP intro, states 3/4 by forcing the struct
    via REPL `w8`. Gate-off boot clean to GAME free-roam (sm[0x4a]=1). NB the fade is still DELIVERED as a
    PSX OT rect (FUN_8007e9c8) — owning the fade PC-native (engine brightness scalar) is a separate render
    frontier (see the FADE note below).
  - ☐ NEXT (gameplay): descend the rest of the SOP per-frame field update `FUN_801092b4` (entity update FUN_8010a0e0,
    Tomba update 0x8007b008, BG draw, entity render FUN_80109fe0) — own its sub-systems native, re-wiring the
    orphaned cull/spawn/collision/object-walk natives as their callers become native. Also own the
    remaining sm[0x4a] running sub-modes (2..5) + the area-machine RUNNING sub-states (0x80106b98 etc.,
    currently rec_dispatched yield-free).

## D. Per-frame GAMEPLAY systems (inside the GAME stage loop)
- ✅ `FUN_800788ac` frame update = `ov_frame_update` (pad read + present + audio kick) — game_tomba2.cpp.
- ✅ **SOUND front-end (engine/sound.cpp, later-207)** — the game's SFX/BGM TRIGGER API (the functions the
  game logic calls to play a sound / start-stop BGM; wraps libsnd — the SPU is already native). Clean PC-game
  audio module: `sound_play_sfx`/`sound_play_bgm`/`sound_stop_bgm`, `sound_register()` (one line in
  game_tomba2.cpp). ✅ `FUN_80074590` SFX / song-id ROUTER = `ov_sound_play_sfx` — OWNED PC-native (id->song
  map + bounds; descriptor SFX sub-path + the 0x80075e04 submit leaf kept dispatched). `soundverify` full
  RAM+scratchpad A/B 0-diff over 800+ live calls (menu/cursor/action SFX). ◐ `FUN_80074BF8`/`FUN_80074E48`
  BGM start/stop = `ov_sound_play_bgm`/`ov_sound_stop_bgm` — engine-glue super-call WRAPPERS (own the
  instant-CD dialog-music cut hook + the clean API; the gen body runs as the live libsnd sequencer because
  its voicetab @0x800be238 state is co-evolved with the SsSeqPlay/SsSeqStop LEAVES — a native re-drive that
  dispatches the leaf can't reproduce the leaf's per-voice bookkeeping bit-for-bit, so they stay sequencer
  glue per THE BOUNDARY). Native BGM bodies + full RE retained in sound.cpp as the documented reference for a
  future pass (own the SsSeqPlay voice allocator too). Replaces native_boot ov_bgm_start/stop.
- ✅ `FUN_8007a904` object/entity WALK = `ov_objwalk` (engine_tomba2.cpp) — the per-frame object driver.
- ✅ **SPAWN subsystem — FULLY OWNED (engine/entity_spawn.cpp, later-208).** All 5 pool spawn primitives
  + both dispatchers PC-native: `FUN_80079C3C` (pool-208, cnt<3 guard), `FUN_80079DDC` (pool-2, delegates
  to 79F90 when empty), `FUN_80079F90`/`FUN_8007A12C`/`FUN_8007A2C8` (pool variants, empty→return 0) all
  share one `spawn_link_stamp`; `FUN_8007A980` per-type spawn dispatcher (table 0x80016E4C, ref=0/mode=3)
  + `FUN_8007AA38` spawn-relative-to-object dispatcher (table 0x80016E64, guards obj[+0x0a]==a3, passes
  caller mode/list). **Fixed a real latent bug in the shared link/stamp:** when the target active list is
  EMPTY the recomp inits BOTH end pointers (head-insert also writes *tail, and vice-versa); the native body
  set only one end → empty-list inserts diverged at the list head ptr. Gates: spawnverify, spawndispverify,
  spawnvarverify all 0-diff; pool2verify/replacedispverify not exercised at seaside (0 calls, RE-verified).
- ✅ **DESPAWN — `FUN_8007A624` `ov_despawn` (entity_spawn.cpp, later-208).** The inverse of spawn: unlink
  node from its active list, clear node[+0x28] high bit, push to the pool free-list (5 trivial free handlers
  0x8007a718..a7a8, pools = the spawn descriptors; class 4 also calls cleanup 0x8007ADDC), then the shared
  deactivate epilogue (zeros header words 0/4/8/c/10/14/18/38 + bytes 0x29/0x2a/0x2b/0x5e; preserves the
  free link +0x24). Gate `despawnverify` 0-diff over 100+ live despawns. **Object-pool lifecycle now COMPLETE
  (alloc + free).** GOTCHA: the deactivate epilogue clears far more of the node than node[0]/[4] (first try
  diverged at node+0x0a) — RE the full 0x8007a7d0 epilogue.
  - ✅ later-295 (2026-07-01): **wired despawn onto the LIVE path.** The oracle-verified native `ov_despawn`
    sat ORPHAN since the override-flip removal (2026-06-22) — every one of the ~34 per-object AI behavior
    handlers (`game/ai/beh_*.cpp`) that reach STATE 3 (despawn) still called `rec_dispatch(c, 0x8007A624u)`,
    running the PSX recomp body instead of the already-verified native code sitting right next to it. Added
    typed live-wiring entries `world_despawn(c, node)` / `world_spawn_and_init(c, a0, posSrc, a2)` (spawn.h)
    and replaced every `rec_dispatch`/`leaf1(...)` call site targeting `0x8007A624u` (34 sites) and the one
    targeting `0x8003116Cu` (spawn) with a direct native call — PC calls PC for what it owns, per the
    top-down-ownership rule. No behavior change (same oracle-verified bodies), pure substrate-dependency
    reduction; smoke-tested via headless `PSXPORT_AUTO_SKIP=1` (reaches free-roam, no regression) + the
    camera oracle test (0-diff, unaffected). ObjectWorld is the next port-progress candidate (formalize
    pool+spawn+despawn as `class ObjectWorld`, docs/pc-subsystem-rebuild.md) — this session activated the
    dormant code first since that was higher-value and lower-risk than a speculative class wrap.
- ✅ `FUN_8007712c` per-object CULL / LOD = `ov_object_cull` (game_tomba2.cpp). **BODY now PC-native
  (later-188)** — was a `rec_super_call` WRAP (recomp body ran hot, ~11.2% of sampled interp time); now
  `cull_native_body` reimplements the full decision (RE'd from the disasm: jump table 0x80016cc0, 5 state
  handlers + state-0 typed sub-dispatch). dist=eng_isqrt16(dx²+dy²+dz²); per-state {near,far,fov} cone test
  KEEP iff near≤dist<far AND (fwd·d)/(dist*4)≥fovthr (MIPS signed `div`, C `/` matches); writes the visible
  flag @obj+1 + pushes the obj ptr onto 3 type-keyed render queues (A/B/C @0x1f80013c/48/54, cap 24/40/28).
  VERIFIED 0-diff over 60000+ live calls via the `cullverify` gate (predict native pure, recomp body does
  the writes, compare obj+1/state/queue-delta/pushed-ptr) incl. directional motion. Field 27.06M→25.02M
  insns (−7.5%; cumulative −41.7% from baseline). Margin re-include (margin_collect) still fires.
- ✅ Gameplay-overlay pure LEAF `0x8013fae0` = `ov_tile_lookup` (engine_submit.cpp, later-188b) — 2D table
  lookup `tab[52*a1+a0] & (mask16<<4)` (tab=*0x8014c804, mask=*0x8014c800). Was the single hottest overlay
  piece (4.25%, 39.9k calls), mis-bucketed under FUN_8013F0DC until the prof_report overlay-resolution fix.
  Signature-registered in engine_scan_overlay; `tileverify` gate 0-diff 60000+ calls. Field 25.02M→24.46M.
- ✅ Gameplay-overlay TRIANGLE-SCAN SOLID-TILE GATHERER `0x8013f4dc` = `ov_tilescan` (engine_submit.cpp,
  later-191) — the per-object collision broad-phase: y-sorts 3 corner {x,y} (a1/a2/a3 ptrs), rasterizes
  the triangle scanline-by-scanline, per covered cell calls the owned `ov_tile_lookup`, and APPENDS the
  cell's tile id into obj[0x10 + 2*count] (count byte obj+6, cap 254) when the cell is non-empty AND solid.
  CLEAN INTEGER DATA (no GTE/GP0) → faithful exact-match (tile-lookup family). **CORRECTS the handoff/§F
  mislabel:** the profiler bucket `FUN_8013F0DC` (~11% with siblings) is NOT the anim/scale SM at 0x8013efa8
  (that fn is COLD — 3 calls/run); the hot code is THIS fn's loop body (0x8013f9xx), bucketed under
  FUN_8013F0DC only because that's the nearest call-target boundary. Subtle exact bits: s0 cell address =
  base+4 + 2*(s1*width+s2) row-major (the FIRST advance is 2*v0cmp from the `j 0x8013f788` delay slot, not
  2*width); empty cell skip = `v1 == -1` (the dedup beq's v0 = -1 from the branch delay slot 0x8013f798).
  Signature-registered (corner-ptr load triple); `tilescanverify` gate 0-diff 600+ calls over varied
  triangles (71–149 tiles). Field 25.99M→24.28M (−6.6%); the ~11% 0x8013f9xx cluster GONE from the profile.
- ✅ **later-192 batch — 6 resident leaves OWNED (field 24.28M→~23.0M).** All in game_tomba2.cpp /
  engine_submit.cpp, each absent from the profile after: (1) `FUN_8003F698` render-command DISPATCHER
  (~4%) — folded NATIVE into `ov_render_cmd` (`render_cmd_dispatch`): read cmd byte 0x800BF870,
  bounds-check <22, index jump table 0x80015268, decode the per-cmd thunk's `jal` target, rec_dispatch the
  handler (tail-call, exactly equivalent — render output unchanged, only dispatch glue native). (2)
  `FUN_8009A450` `ov_rand` — glibc LCG (state*0x41C64E6D+12345 @0x80105EE8, (state>>16)&0x7FFF), 100k
  calls/run, `randverify` 0-diff. (3) `FUN_80083E80/F50/EBC` `ov_trig_sin/cos/lut` — angle-table lookups
  (12-bit angle, tables @0x800a5af0/52f0/42f0/4af0), `trigverify` 0-diff 40k+ (GOTCHA: cos q3 a0≥3073
  returns UNNEGATED — its path `j 0x80083fe8` skips the negate, unlike q1/q2). (4) `FUN_80077ACC`
  `ov_cull_wrap_77acc` — 2nd cull-wrapper variant (flags 0x1F800080=1/0x1F800084=4, position in a1-a3,
  camera-relative → owned cull 0x8007712C), `cullwrap2` 0-diff. (5) `FUN_8004D7EC` `ov_bittest_4d7ec` —
  pure bitmap test `bitmap[(int16)a0/8] & (1<<((int16)a0%8)&31)`, base 0x800BFD34 / 0x800BFCB4 by a1&0xff,
  `bitverify` 0-diff. **LESSON re-confirmed:** the remaining big freq buckets (`FUN_8004CE14`,
  `FUN_80040558`, `FUN_8013C3F4/538`, `FUN_8012E2F4`) are per-object STATE MACHINES = game CONTENT
  (stay PSX); and the `ov_XXXX(>encl)` profiler labels are mid-fn boundaries — disas to the real prologue
  before porting.
- ✅ **later-193 — `FUN_80031780` `ov_list_scan_31780` (list-tail resolver/reset).** Walks the 8-byte-stride
  list rooted at a0[0x34], reading the tag at entry+4 until `tag & 0xC0000000`; if `tag & 0x40000000` set ->
  clear list (a0[0x34]=a0[0x38]=0), else set tail a0[0x38]=found+8. GOTCHA: the loop's `addiu v1,v1,8` is a
  DELAY SLOT — it runs even when the branch falls through, so the found entry is +8 past where the tag matched
  (first verify pass was off-by-8 until I modeled the delay slot). `listscan` A/B 0-diff (5000+ matches).
- ✅ **later-194 — `FUN_80049968` `ov_grid_setup_49968` (collision-grid ROW-POINTER setup).** a0=grid index
  (&0xff); table base @0x1F8001C8, `rec = base + table[a0]*2`; writes 5 scratchpad row ptrs:
  0x1F8001CC=rec+0x14, 0x1F8001D0/D4/D8/DC = rec + rec[12/14/16/18]*2. Pure scratchpad pointer arithmetic.
  `gridsetup` A/B 0-diff (5000+ matches). Pairs with the grid QUERY FUN_80047CBC (next).
- ✅ **later-195 — `FUN_80047CBC` `ov_grid_query_47cbc` (collision-grid CELL QUERY / neighbor-walk, ~158
  instrs, 3 returns; note 0x80048034+ is a SEPARATE fn).** Probe pos (sh[0x1BC],sh[0x1C0]) − origin
  (sh[0x1AA],sh[0x1AC]) >>6 → grid idx; bounds-check vs row table w[0x1CC]; cell record = w[0x1D0]+idx*8.
  Then walks following the cell TAG bits: 0x8000=keep walking, 0x4000=follow the cell's link/child list
  (inner sub-scan vs u16[0x1BE]−32), else step ONE cell ±X (sh[0x1C0]) / ±Z (sh[0x1BC]) per the low 3 tag
  bits (snap mask ~63), recompute, repeat. Returns 0 (off-grid/blocked) / 1 (resolved). Writes scratchpad
  ONLY. Verified with a FULL-scratchpad A/B gate `gridquery` ([0x1F800080,0x1F8001F0) byte-compare + return
  reg): 0-diff over 10000+ calls with movement in all 4 directions (exercises every step branch). GOTCHAs:
  delay-slot writes (sh[0x1A8]=a2 then conditionally =0); a1 reloaded to scratchpad base mid-fn (t5=SP);
  L_f9c uses (int16)t1*4 for the row index but FULL t0 for the column add.
- ✅ **later-200 — `FUN_800498C8` `ov_grid_resolve_498c8` (collision-grid RESOLVE LOOP — head of the grid
  family, drives the later-194/195 leaves).** a0 = probe object. Loop: `jal 0x8004798C(obj)` (per-step grid
  setup; non-trivial → kept DISPATCHED), `jal 0x80049968(u8@0x1F8001FE)` (owned row-ptr setup), `v0 = jal
  0x80047CBC()` (owned cell query). v0==0 → return 0. Else v1=w[0x1F8001E0] (record ptr the query latched);
  (h[v1]&0x4000)==0 → return 1 (terminal). Else obj[42]=b[v1] (record the resolved tag byte onto the probe
  object), reload v1'; (h[v1']&0x4000)!=0 → LOOP (descend) else return 1. Pure control flow over scratchpad
  + object memory; ONE object write (obj+42); NO GTE/render. Control flow + the obj+42 write owned native;
  all three callees stay PSX via rec_dispatch. Verified with the full RAM+scratchpad A/B gate `gridresolve`:
  **0-diff over 8000+ live field calls** (press right 250 + press left 250). GOTCHA (scriptvm/player family):
  the dispatched callee tree runs in BOTH passes and leaves transient stack residue below entry sp (no native
  frame there), so the gate excludes the top-of-RAM stack window [sp-0x800, sp) (sp ~0x1FE9xx, far above all
  game data) — a 32-byte window first exposed exactly this residue (diffs all at sp-0x26..sp-0x48), the wider
  window then went 0-diff. Registered in game_tomba2.cpp alongside the grid leaves.
- ✅ **later-201 — `FUN_8004798C` `ov_grid_step_4798c` (collision-grid PER-STEP ORIGIN/INDEX SETUP — the
  LAST dispatched callee inside the resolve loop; COMPLETES the grid family setup/query/resolve/step).** a0 =
  probe object. Pure scratchpad halfword arithmetic + two dispatched callees; NO GTE, NO render. (1) if
  obj[42] != byte[0x1FE] → jal 0x80048ecc(obj[42]) (grid reload, dispatched). (2) SELECT/RANGE TEST: if
  (h[0x1AE] u< h[0x1B0]) test the Z range ((h[0x1C0]−h[0x1AC])&0xffff vs h[0x1B0]) else the X range
  ((h[0x1BC]−h[0x1AA])&0xffff vs h[0x1AE]); if the probe is past it → jal 0x80048fc4(obj,1) (re-resolve,
  dispatched). (3) CLAMP+RECOMPUTE: on (h[0x1AE] u< h[0x1B0]) → Z branch (clamp 0x1C0 into [0x1AC,
  0x1AC+0x1B0], recompute 0x1BC) else X branch (clamp 0x1BC into [0x1AA, 0x1AA+0x1AE], recompute 0x1C0);
  recompute = cellbase + (((clamped − cellbase2)·pitch[0x1BA]) >> 14) — SIGNED mult, low word, arithmetic
  sra. Control flow + scratchpad ops owned native; the two callees stay PSX via rec_dispatch (and 0x8004798C
  is now routed through this native body from the owned resolve loop). Verified with the full RAM+scratchpad
  A/B gate `gridstep`: **0-diff over 8000+ live field calls** (press right 250 + press left 250). GOTCHA
  (same grid family): the dispatched callees run in BOTH passes and leave transient residue below entry sp
  (no native frame there) → gate excludes the top-of-RAM stack window [sp-0x800, sp). The whole grid family
  (49968 / 47CBC / 498C8 / 4798C) is now OWNED. Registered in game_tomba2.cpp alongside the grid leaves.
- ✅ **later-202 — `FUN_80040410` `ov_child_spawn_40410` (per-object CHILD-NODE SPAWN / sub-object builder —
  a callee of the per-object state machine FUN_80040558's state-0 handler).** a0=obj, a1=group index (low
  byte). NO GTE, NO render packets — pure control flow + object/child-node memory writes, 2 dispatched
  callees. Sets obj[8]=2 (child count) unconditionally; GATE: if (int16)*0x800ed098 < 2 → obj[4]=3, return 0.
  Else zero obj[9]=2/obj[13]/obj[11]/sh obj[84..88], then for i in [0,count): alloc a child node (jal
  0x8007aae8, dispatched), store its ptr at obj[0xC0+4i], node[6]=(i−1) s16, node[0/2/4] = u16 tblA[6i+0/2/4]
  (tblA=0x800a3b1c stride 6), node[8/A/C]=0, a2 = lh tblB[2*((a1&0xff)+i)] (tblB=0x800a3b28 stride 2),
  jal 0x80051b04(node,1,a2) (transform/geom setup, dispatched); return 1. Control flow + every memory write
  owned native; the allocator + setup stay PSX via rec_dispatch. GOTCHAs: child count is the value JUST
  stored (re-read obj[8]=2 from memory); node[6] uses the PRE-increment loop index via delay-slot ordering;
  the gate global is a signed 16-bit compare (`slti v0, lh, 2`). Verified with the full RAM+scratchpad A/B
  gate `child40410` (native run → snapshot+rollback → rec_super_call → diff): **0-diff over 28 live
  field-spawn calls** (press right 250 + press left 250 — this is a spawn-time INIT handler, called once per
  object-spawn not per-frame, so 28 is the natural exercise count; each call compares full RAM 0x200000 +
  full scratchpad 0x400 + v0). GOTCHA (same family as grid/scriptvm): the 2 dispatched callees run in BOTH
  passes and leave transient residue below entry sp (FUN_80040410's own 48-byte frame is also dead there) →
  gate excludes [sp-0x800, sp), far above all game data. Registered in game_tomba2.cpp.
- ✅ **later-203 — `FUN_80040558` `ov_sm40558` (per-object STATE-MACHINE HEAD — the dispatcher whose state-0
  handler calls the just-owned child-spawn FUN_80040410; owning the HEAD advances the whole behavior family).**
  a0=obj, void return. NO GTE, NO render packets — pure control flow + object byte/halfword writes + global/
  scratchpad reads, with EVERY `jal` (24 distinct sub-behaviors incl. overlay 0x80114xxx/0x80120xxx/0x8012xxxx)
  kept PSX via rec_dispatch. Dispatch on the state byte obj[4]: 0/1/2/3. **STATE 0** = spawn-init (calls owned
  FUN_80040410, sets up obj fields + an obj[5]/obj[94] sub-machine). **STATE 1** = the hot active-behavior path
  (obj[5] jt 0x80015300 + obj[94] jt 0x80015318 with the @7e0/@834/@888/@8c0 tails, gated on globals
  0x800bf870/816/817). **STATE 2** = a transition machine (obj[5] jt 0x80015338). **STATE 3** = jal 0x8007a624.
  Three inner jump tables dumped from MAIN.EXE; full RE in journal later-203. Control flow + every memory write
  owned native; all 24 callees stay PSX via rec_dispatch. Verified with the full RAM+scratchpad A/B gate
  `sm40558`: **0-diff over 11200+ live field calls** (press right 250 + press left 250). Coverage (via a
  transient -DSM40558_HIST histogram, since reverted): this seaside scene drives state 0 (28×, spawn-init) +
  state 1 (~11000×, hot path) — states 2/3 do NOT fire here (rarer transition/special paths) and are
  transcribed-from-disasm, verifying the moment a scene drives them (same posture as scriptvm/gridstep rare
  branches). GOTCHAs caught while RE'ing: state-0 obj[5]==1 entries 6/7 take the callee's v0 and early-return
  WITHOUT the obj[4]/[5]/[0]/[41] writes when v0==0 (entries 0-5 force v0=1); jt 0x80015318[7]→@7d8 is the
  same block as [0,1,3,4,6]→@7e0 only PREFIXED by jal 0x8012b118; the *0x800bf817==obj[106] test is vs the
  SIGN-EXTENDED s16 lh. Same-family gate exclusion [sp-0x800, sp) (dispatched callees + this fn's 24-byte frame
  dead below entry sp). Live (gate-off) run: Tomba walks normally (master X 0x0F640000→0x17704900 holding
  right), reaches stage 0x8010637C. Registered in game_tomba2.cpp.
- ✅ **later-204 — `FUN_8003FD10` `ov_osc_fd10` (per-object OSCILLATE / FRAME-TOGGLE sub-behavior — the FIRST
  descent into sm40558's STATE-1 hot active-behavior callees; JT1[0] of the obj[5] jump table @0x80015300).**
  a0=obj, void return. NO GTE, NO render packets — pure object/scratchpad/child-node memory ops + ONE
  dispatched callee (0x8009A450 = owned ov_rand). 3-way micro state-machine on the phase byte obj[6]: ==0
  arm (obj[43] gate → obj[6]=1, obj[64]=16); ==1 run (decrement counter obj[64], wrap obj[6]-- on cnt==-1,
  then set child node[2]/[0] (obj+0xC0) to oscillation offsets from scratchpad 0x1F80017C&1 and (ov_rand&3)-2,
  ×6); else no-op. Control flow + every memory write owned native; ov_rand stays PSX via rec_dispatch.
  GOTCHAs (all delay-slot, all caught by the A/B): node[2]'s `sh` is in the ov_rand jal delay slot (pre-call
  node/value); obj[6]-- only on the cnt==-1 branch (`addu v0,v1=-1`); offsets are v0*6=(v0*3)<<1. Verified
  with the full RAM+scratchpad A/B gate `fd10` (native → snapshot+rollback → rec_super_call → diff): **0-diff
  over 11000+ live field calls** (press right 250 + press left 250 — a hot state-1 sub-behavior). Same-family
  gate exclusion [sp-0x800, sp) (dispatched ov_rand runs in both passes + this fn's 24-byte frame dead below
  entry sp). Registered in game_tomba2.cpp. NEXT descent: JT1[1..5] (0x8003FED8/FFCC/4022C/40390) — 0x80040390
  is next-cleanest (gated obj[41], 2 dispatched callees, no GTE).
  **later-205 NOTE — the JT1[1..5] siblings are NOT autonomously verifiable in the seaside scene.** A JT1
  histogram on the live field (instrumented in sm40558's obj[5] dispatch, then reverted) showed obj[5] is
  **ALWAYS 0** here — only JT1[0] (fd10) ever fires; JT1[1..5] (0x8003FED8/FFCC/4022C/40390/80114934) get
  **zero calls** (a -DFD390_FIRE entry counter on 0x80040390 confirmed it never fires). So a 0-diff A/B gate
  on any of them can't be exercised autonomously; they are deferred until a scene drives obj[5]!=0. The
  autonomously-verifiable JT1 frontier is exhausted — pivoted to the §F resident hot-list instead (later-205).
- ✅ **later-205 — `FUN_80051128` `ov_xform51128` (per-object CHILD-NODE TRANSFORM loop, ~3.7% field hot —
  engine_submit.cpp).** A sibling of `FUN_80051464` ov_xform_propagate: for each child node it builds a
  per-child rotation from the child's stored rotation triple (child[56/58/60], sign-extended) + euler angles
  (child+8), multiplies it onto the parent's world matrix, MVMVA-transforms the child's local translation into
  child+0x2C, and accumulates the parent's world translation. EVERY callee is an already-owned native
  transform PRIMITIVE — ov_rotmat (0x80085480), ov_mat_mul (0x80084110), ov_apply_matlv (0x80084220) — so this
  is pure orchestration + scratchpad seeding + integer translation adds. NO GTE op in the body, NO render
  packets. We rec_dispatch the primitives in the recomp's EXACT jal order to preserve the matmul→MVMVA GTE-CR
  coupling (ov_mat_mul CTC2's R→CR0-4 so the following ov_apply_matlv reads the right matrix). Scratchpad work
  areas: 0x1F800000 SetVector work, 0x1F800020 RotMatrix out, 0x1F800040 composed matrix. GUARD: node[9]==0 ->
  return. Loop is a DUAL-BOUND idiom (enter on node[8], continue on node[9]) — same as xform_propagate. Parent
  select: child[6]==-1 → ROOT (parent = this node, node+152 matrix / node+0xAC translation); else SIBLING
  (parent = node[0xC0 + 4*child[6]]). GOTCHAs: (1) child[56/58/60] are sign-extended (lhu→sll16/sra16); (2) the
  sentinel child[6] is sll'd by 2 in the branch delay slot, so the sibling parent ptr is node[0xC0 +
  sentinel*4]. VERIFIED with the `xform51128` gate (same scheme as `xformverify`: snapshot each touched child
  sub-struct +0x18..+0x37 + the scratchpad work matrix + GTE data regs; both paths run the identical owned
  primitives, so it verifies the orchestration — loop bounds, branch select, address math, add order):
  **0-diff over 3000+ live field calls (~22000 child transforms), 0 mismatches**, exercising BOTH branches
  (root=4748 / sibling=17268 children) across both movement directions (press right 400 + press left 400).
  Live gate-off run: Tomba walks normally (master X 0x0F640000→0x17704900 holding right), reaches stage
  0x8010637C. Registered in game_tomba2.cpp alongside ov_xform_propagate. GTE-reg exclusion: FIFO regs 12-15
  + reg 31 (LZCR) — the comparator can't round-trip a FIFO restore, and neither path writes them (same as
  xformverify). NEXT clean §F targets: `FUN_80026C88` / `FUN_8003F024` (pure per-object dispatcher loops over
  the 40-entry 0x800ec188 table, no GTE — verify via full RAM+scratchpad A/B); `FUN_80051128`'s consumers in
  the transform cluster. AVOID `FUN_80027A4C` (16% but GTE/GP0 packet submitter, render-boundary).
- ✅ **later-206 — `FUN_80026C88` `ov_disp_26c88` (per-object DISPATCHER LOOP over the 40-entry, 64-byte-
  stride object table at 0x800ec188 — the §F-flagged primary target).** No args, void return. **NO GTE, NO
  render packets — pure control flow.** Loop i in [0,40): read obj[0] (active byte) — if 0 skip; else
  idx=obj[1], fn=*(0x800ad52c + idx*4) (handler fn-ptr table, stride 4), call fn(obj). obj += 64 each iter.
  The dispatcher itself writes NOTHING (recomp body only saves/restores s0/s1/s2/ra in its own 32-byte
  frame, never touched by the native body); ALL side effects live in the dispatched handlers, kept PSX via
  rec_dispatch (each honors its own owned override identically). Control flow + table/object address math
  owned native; handlers dispatched. Verified with the full RAM+scratchpad A/B gate `disp26c88` (native →
  snapshot+rollback → rec_super_call → diff): **0-diff over 800+ live field calls, 0 mismatches** (press
  right 250 + press left 250); FIRES per-frame in the reachable seaside GAME stage. Same-family gate
  exclusion [sp-0x800, sp) (dispatched handlers + this fn's 32-byte frame dead below entry sp, far above all
  game data). Registered in game_tomba2.cpp. NEXT: `FUN_8003F024` (flagged fallback, same dispatcher shape).
- ✅ **later-196 — `FUN_8004CE14` `ov_script_vm_4ce14` (per-object SCRIPT-VM tick — THE most-called field
  fn, ~14900 calls/run).** First CONTENT state machine owned after the boundary removal. Dispatch on state
  byte obj[4]: 2→no-op; 3→jal 0x8007A624; >3→no-op; 0→ if global 0x800BF873!=0 set obj[4]=3 & return, else
  INIT (obj[4]=1, obj[0]=1, load behavior fn-ptr from table 0x800A3F00[obj[3]] into cursor obj[108],
  obj[116]=0, jal 0x8004B354(obj,0)) then fall into state 1. State 1: a pause/mode gate (globals
  0x800BF870/871 + scratchpad 0x1F800207 + per-obj run-cond obj[3]) then a 16-byte-stride command loop at
  cursor obj[108]: opcode 0xFF terminates; flag byte s4[2] bit7 selects predicate 0x8004D7EC(clr)/0x8004D868
  (set), gated by per-slot mask obj[116]&(1<<idx); a passing slot runs 0x80111CCC(s4[12]) (when
  0x800BF870==1 && 0x800BF871>=15) or the cull/anim 0x80077ACC(obj,s4[4],s4[6],s4[8]); nonzero return ORs the
  slot bit into obj[112]. Terminator: obj[106]=slot count, obj[11]=31, obj[1]=1, jal 0x80077EFC. **CONTROL
  FLOW + memory ops owned native; every jal sub-behavior stays interpreted via rec_dispatch (each honors its
  own override identically in the super-call path).** Verified with a FULL RAM+scratchpad A/B gate `scriptvm`
  (each path runs once from one checkpoint; the native run is rolled back; FUN_8004CE14's OWN 56-byte stack
  frame [sp-56,sp) excluded — gen saves regs there, native never touches the guest stack): **0-diff over
  3000+ calls** with movement in all 4 directions (state-0 init + state-1 loop both predicates + cull-exec +
  terminator). Rarer paths (0x80111CCC, state 3, G0==6) transcribed from disasm, verify when a scene drives
  them. GOTCHA: the state-0 gate is INVERTED from the obvious read (`beq v0,zero` → gate==0 is the INIT path,
  gate!=0 sets obj[4]=3) — got this backwards first, caught by the A/B.
- ✅ **later-197 — `FUN_800931C0` `ov_input_dispatch_931c0` (per-frame INPUT/controller-state processor —
  the heaviest un-owned RESIDENT fn, ~12% of field TIME).** 5 phases over the global tables at
  0x80105xxx/0x801054xx: P0 advance the 16-slot ring index 0x80105BAC, clear new slot; P1 per object
  [0,(int8)0x80105CEC) jal 0x8009A1D0(s0,&rec[s0]) (rec base 0x801054CE, stride 56) + set "present" bit in
  the ring slot when rec.h0==0; P2 if (int8)0x80105D28==0, AND the ring slots 0..14 (a 15-frame coherence
  window) and for matching objects with rec-byte==2 jal 0x80097E10; P3 mask 0x801054B8/BA by ~0x80105BF0/BF2
  then per object [0,24) call the two indirect fn-ptr globals (*0x80105BA8)/(*0x80105A20) when the record
  halfwords are set; P4 per object [0,24) marshal a struct on the stack from the flag byte 0x80105A08[s0] +
  the halfword fields at 0x80105A28+s0*16 and jal 0x80099970(struct) when nonzero; P5 four channel flushes
  (0x80098F90 ×2 / 0x80098DB0 / 0x80097E10) then zero the channel globals. **Control flow + memory ops owned
  native; every jal (incl. the P3 indirect fn-ptrs) stays dispatched.** We mirror the gen 120-byte stack
  frame (sp-=120) so the P4 struct lands where 0x80099970 expects it AND every sub-call's frame aligns with
  the gen body. Verified with a full RAM+scratchpad A/B gate `pad931c0`, **0-diff over 4000+ calls** (all 4
  directions). GOTCHAs: (1) this fn runs from BOOT, so the verify gate must re-check cfg_dbg EACH call, not a
  one-shot static (a first-call latch pins it OFF before the REPL `debug` is processed). (2) A/B excludes
  scratch stack below the entry sp: an A/B that double-runs the sub-calls legitimately leaves different
  scratch there — a deep callee of 0x80099970 reads a transient mid-fn value reconciled before return (the
  struct passed in is byte-identical; ALL persistent state + v0 match). The stack lives at the top of RAM, far
  above all game data, so the exclusion cannot hide a behavioral bug (that would alter persistent state).
- ✅ **later-198 — `FUN_80056B48` `ov_player_move` (PLAYER velocity-integrate handler — engine/engine_player.cpp).**
  First MOVEMENT/physics content fn owned after the boundary removal. Integrates speed×dir into the MASTER
  position (16.16-fixed X/Y/Z at 0x800E7EAC/B0/B4, struct base G=0x800E7E80): posX += dirX·speed (a0+0x48 ·
  a0+0x44), posZ += dirZ·speed (a0+0x4C·a0+0x44), and — only when arg a1==0 — posY += dirY·speed (a0+0x4A·
  a0+0x44). Tail: if (flag363 @a0+0x16B == 0 && flag97 @a0+0x61 == 0) jal 0x80054650(a0,0) (a settle/stop
  helper, kept DISPATCHED — runs in guest RAM); else flag95 @a0+0x5F &= ~0x04. v0 = the dispatched call's
  result (jal path) or the masked flag95 byte (else path). The s16×s16 products fit in 32 bits exactly so
  `(int16)dir * (int16)speed` matches the MIPS signed `mult`/`mflo` low word. Registered in game_tomba2.cpp.
  Verified with the full RAM+scratchpad A/B gate `playerverify` (run native → snapshot → roll back → run
  rec_super_call → diff): **0-diff over the full motion run** (press right 300 + press left 300 frames),
  500+ matches, 0 mismatches. GOTCHA (same as scriptvm/pad931c0): the dispatched callee 0x80054650 runs in
  BOTH passes and leaves different transient values in its own stack frame below entry sp, so the gate
  excludes [sp-0x800, sp) — pure stack scratch at the top of RAM (~0x1FE8F0), far above all game data; every
  persistent word + scratchpad + v0 match. Live (non-verify) path confirmed Tomba walks normally (X hi16
  3940→6000 holding right). Added to run.sh + tools/build_port.sh SRC lists.
- ✅ **later-198 — `FUN_80076D68` `ov_anim_vm_76d68` (per-object ANIMATION-SEQUENCE VM stepper, ~3.6% of
  field interp time, no GTE).** a0=anim object; field s0+0x0E = frame countdown/duration; s0+0x38 = cursor
  (word ptr) into an 8-byte-stride keyframe stream (tag/payload halfword @+6, jump pointer @+8). ctrl =
  s0+0x0E read once; bit 0x1000 = "do NOT advance the cursor" (freeze). low12(ctrl)>1 → DELAY: cursor SIGN
  BIT is a flag — cur<0 freezes (h=(low12−1)|(ctrl&0x1000), ret 0), else apply frame (0x80075f0c, a1=low12−1)
  + h=(low12−1)|(post-call s0[14]&0x1000), ret 2. low12==1 → STEP on tag [cur+6]&0xc000: 0x0000 advance
  cur+=8; 0x4000/0xc000 FOLLOW cur=[cur+8]; 0x8000 hold; then reload duration via 0x80076904 + if loaded
  frame has the 0x2000 exec flag run executor 0x80075ff8. **Control flow + memory ops owned native; the 3
  callees stay PSX via rec_dispatch.** `animvm` full-RAM+scratchpad A/B vs rec_super_call: **0-diff 4000+
  calls** across all 4 movement directions. GOTCHAs caught by the gate (all delay-slot / arg-register slips):
  (1) the cur<0 path writes `(low12−1)+(ctrl&0x1000)` — the +0x1000 is the bltz delay slot `andi v0,a0,0x1000`,
  NOT a literal +2; (2) 0x80075f0c needs **a1=low12−1** (it sets the cursor's KSEG0 flag bit when a1==1) —
  passing only a0 left the cursor's high byte clear; (3) T4000/TC000 FOLLOW [cur+8] when NOT frozen (the
  transient cur+8 store is dead, overwritten) — had it inverted with the freeze case first; (4) the apply
  path RE-READS s0+14 after 0x80075f0c (the applier may modify it) for the freeze-bit OR.
- ✅ `FUN_80051C8C` per-object TRANSFORM build = `ov_build_xform`.
- **Camera update — now `class CutsceneCamera` (game/camera/cutscene_camera.{h,cpp}), restructured from the old orphaned
  the old `engine_camera.cpp` register-convention statics into PC-game structure (methods over named state, no
  `c->r[4]=cam`).** ✅ RESOLVED (2026-07-01, later-293, docs/findings/camera.md): this resident camera IS the
  free-roam FIELD camera — `snapFollow`(0x8006E3B0)→`lookAt`(0x8006D02C), driven by the resident dispatcher
  0x8006ec4c from the field frame via DIRECT intra-MAIN calls (`func_8006E3B0(c)`) — which are invisible to
  recdep/camtrace (they hook only rec_dispatch; that blindness caused the earlier "never runs in free-roam"
  false trail, now corrected). The handoff's "0x8006E3B0 is the live free-roam camera" was CORRECT.
  ✅ WIRED + VERIFIED:
  `CutsceneCamera::snapFollow` (FUN_8006E3B0) is called native from `game/scene/sop.cpp` via `cam_snap_follow`
  (replacing `d2(c,0x8006e3b0)`); `PSXPORT_DEBUG=camverify` = **0 mismatch over 51+ live SOP calls** (cam
  struct + full scratchpad vs recomp oracle) — exercises trackXZ/trackY snap + the full `lookAt` matrix
  builder. Remaining orchestrators/sub-ops are restructured + compile-clean but only latent (not SOP-live).
  - ✅ position X/Z `FUN_8006d960` = `CutsceneCamera::trackXZ`; ✅ position Y `FUN_8006da54` = `CutsceneCamera::trackY` (later-174).
  - ✅ rotation / LOOK-AT builder `FUN_8006e464` = `ov_cam_rotbuild` (engine_camera.cpp, later-175) — 2 jump
    tables + common look-at tail; owns control flow + arithmetic native, CALLS libgte rsin/rcos/ratan2/isqrt
    via rec_dispatch. Output 0x1f8000d0/d8 is SCRATCHPAD (main-RAM diff is blind) → gated by per-call
    comparator `PSXPORT_DEBUG=camverify` (native vs recomp oracle, 0-diff, d0 accumulating on motion scene).
  - ✅ DISTANCE / ZOOM solver `FUN_8006d2ac` = `ov_cam_dist_solve` (engine_camera.cpp, later-176) — first
    sub-fn of orchestrator FUN_8006e0f0. Picks a target (X,Z) (13-entry jump table on G[+0x164] + cam[+0x72]&2
    path), derives the camera→look-point planar distance (isqrt) + angular error (ratan2), smooths cam[+0x14]
    toward ±0x280000 (±65536/frame), accumulates into cam[+0x58], places camera planar pos cam[+0x08]/[+0x10].
    Outputs are MAIN-RAM cam fields → gated by per-call comparator `PSXPORT_DEBUG=camverify` (0-diff over 1800+
    calls on the motion scene). GOTCHA: the far/near branch's DELAY SLOT (slti angd,2048) reloads v0, so the
    NEAR-path final test NEGATES s0d when angd<2048 — a missed delay-slot effect (caught by the trig-spy diff).
  - ✅ angle-accumulator step `FUN_8006e010` = `ov_cam_angle_step` (engine_camera.cpp) — ±8 rate-limit of
    cam[+0x34] toward a mode-selected target; main-RAM output, camverify 0-diff (1000+ calls, motion scene).
  - ✅ remaining sub-fns: 0x8006d654 = `CutsceneCamera::pitch`, 0x8006c80c = `CutsceneCamera::yFloor`, 0x8006dcf4 =
    `CutsceneCamera::heading`, 0x8006d02c = `CutsceneCamera::lookAt` (orient/look-at matrix builder, uses GTE leaves —
    the one LIVE-verified via snapFollow). All restructured into methods; lookAt proven 0-diff live.
  - ✅ per-MODE orchestrators = `CutsceneCamera::mainFollow` (FUN_8006e0f0) / `CutsceneCamera::trackFollow` (FUN_8006e228) /
    `CutsceneCamera::simpleFollow` (FUN_8006e3f4) / `CutsceneCamera::snapFollow` (FUN_8006e3b0, the live SOP one). Latent
    except snapFollow until their scenes are driven (or the free-roam camera driver is pinned → own it next).
  - ✅ **UNIT TEST** (all 13 methods, not just the SOP-live ones): `PSXPORT_SELFTEST=camera ./scratch/bin/tomba2_port
    scratch/bin/tomba2/MAIN.EXE` — seeds thousands of synthetic states sweeping every mode selector, runs each
    native method vs the recomp oracle (`rec_interp`), diffs cam struct + scratchpad + globals. **0 mismatches over
    39000 runs** (390 oracle-skipped = states the recompiler can't evaluate, e.g. yFloor render-mode 1 — see
    docs/findings/camera.md). Deterministic, render-free; `PSXPORT_SELFTEST_ITERS=N` / `_VERBOSE=1`.
  - ✅ later-294..294e (2026-07-01): owned the whole camera TREE natively — the driver `update()`/`dispatchMode`
    (0x8006EC44), `init()` + its deps `initPlace`/`initSeedGrp`, all follow orchestrators (mainFollow/
    simpleFollow/trackFollow/snapFollowA/pitchFollow/snapFollowB), all sub-ops, the scripted look-angle
    builders, and the post-mode TAIL `shakeTail()` (0x8006C988, a screen-shake state machine on cam[0x76]).
    **CAMERA TREE FULLY NATIVE** (oracle 0-diff on every method) except the true field OVERLAYS reached by
    driver modes 0/1/9/10/17 (loaded `\BIN\*.BIN` content — a separate porting track). See docs/findings/
    camera.md. Next subsystem: ObjectWorld (entity pool + spawn + node/animation walk).
- ✅ Math helper `FUN_80077FB0` = `ov_isqrt` (engine_math.cpp, later-186) — 16-bit ROUNDING integer sqrt
  (libgte-style leaf). Was 8.41% of all interpreter instructions; owning it native dropped the field run
  42.93M→38.99M insns/300fr (−9.2%). Bit-exact: 65000+ live calls 0-diff vs recomp (`mathverify` gate).
- ✅ Math helper `FUN_80084080` = `ov_gte_norm` (engine_math.cpp, later-186) — table-based fixed-point sqrt
  (LZCR leading-bit count → normalize → LUT @0x800a6310 → exponent shift). GTE used ONLY as a CLZ → pure
  fn of a0, ported native with `__builtin_clz` (the GTE caller no longer needs Beetle). Bit-exact 0-diff
  15000+ live calls. (Was mis-attributed as 9% by 256B buckets; real cost 0.5% — the profiler bucket fix.)
- ✅ GTE 3x3 MATRIX MULTIPLY `FUN_80084110` = `ov_mat_mul` (engine_math.cpp, later-186) — P=R×M via MVMVA
  (sf=1, rot matrix, lm=0). Was 16% of hot interp time + top freq leader (55k calls). Ported PC-NATIVE per
  USER 2026-06-21 (GTE is PSX hardware → make the math native C, no gte_op/Beetle): plain-C MVMVA (44-bit
  accum, >>12, signed clamp16;
  last element via SWC2 = sign-extended). GTE-EXACT verified: a2 output + MVMVA GTE regs (IR/MAC/input) 0-diff
  over 115000+ live calls (FIFO/LZCR regs excluded — comparator can't round-trip FIFO writes; neither path
  touches them). Field run 38.77M→35.21M insns (−9.2%; cumulative −18% from baseline). First of the cluster.
- ✅ GTE RotMatrix `FUN_80085480` = `ov_rotmat` (engine_math.cpp, later-187) — Euler angles (SVECTOR a0)
  → 3x3 rotation matrix (MATRIX a1, also returned). Was ~17% of hot interp time (the hottest after the
  matmul leaf). Ported PC-NATIVE: SIN/COS LUT @0x800a6490 (sin low / cos high, sign re-applied to sin)
  + the GTE GPF op (cmd 0x3d, sf=1: MAC_i=(IR0·IR_i)>>12, IR_i=clamp16) reimplemented as a clamped
  scalar×vector multiply, interleaved with 2 native >>12 mults (cy·cx, cy·sx, truncated not clamped).
  GTE-EXACT verified: 5 output words + ALL GTE data regs (incl. the RGB FIFO the 4 GPFs push) 0-diff
  over 55000+ live calls (FIFO/LZCR excluded — same as matmul). Field run 35.21M→31.90M insns (−9.4%;
  cumulative −25.7% from the 42.93M baseline). Second of the cluster.
- ✅ GTE-transform cluster REMAINDER `FUN_80085050`/`FUN_80084EB0`/`FUN_80084D10` = `ov_rot_z`/`ov_rot_y`/
  `ov_rot_x` + `FUN_80084220` = `ov_apply_matlv` (engine_math.cpp, later-187). The three rot_* are ONE
  generic kernel (`rotpair`): compose an axis rotation onto a 3x3 matrix — rowA'=(cos·rowA−sin·rowB)>>12,
  rowB'=(sin·rowA+cos·rowB)>>12, differing only in (rowA,rowB) offsets + the Y-variant's inverted sin
  sign (80085050 rows +0/+6 posSin+1; 80084EB0 +0/+12 posSin−1; 80084D10 +6/+12 posSin+1). PURE (no GTE,
  SIN/COS LUT @0x800a6490 + native mults; multu→MFLO = signed product for 16-bit operands). ov_apply_matlv
  = MVMVA (sf=1, ROT matrix from GTE CR0-4, vec from a0) → IR1-3 sign-extended to a1, GTE-exact. VERIFIED
  0-diff: rotX 55k+, ov_apply_matlv 75k+ (output + all GTE data regs), rotZ 35k+ live calls; rotY only 1
  live call (Y-axis rotation barely fires in 2.5D seaside) but is the same kernel verified at scale on its
  siblings + offsets/sign read directly from the disas. Field run 31.90M→27.06M insns (−15.2%; **cumulative
  −37% from baseline**). The whole GTE transform cluster is now OWNED + gone from the hot-list.
- ✅ GTE RotMatrix VARIANT `FUN_80084A80` = `ov_rot84A80` (engine_math.cpp, later-189). Build a 3x3 matrix
  from 3 Euler angles (a0 SVECTOR vx/vy/vz, a1 MATRIX out + v0). PURE — SIN/COS LUT @0x800a6490 + native
  16-bit mults (multu/MFLO), NO GTE ops, so no GTE side-effects to mirror. Different element layout/rotation
  order than 80085480 (m20=−s1; m00=c1c2; etc — full layout in the fn comment). Intermediates kept 32-bit
  (asm's `sra ,12` stays register-width before re-multiplying); only the final `sh` stores truncate to 16.
  VERIFIED 0-diff 5000+ live field calls (forced-verify build: resident-math overrides latch `v` at boot,
  so the REPL `debug mathverify` line arrives too late — verify by registering `*_verify` unconditionally
  for the one-time gate, then revert to the gated form). Was 4.4% of field hot time, now gone from hot-list.
- ✅ Render submit: geom GT3/GT4/gt4_bp, per-object render `0x8003CCA4`, render walk `0x8003C048` — engine_submit.
- ✅ MORE per-frame transform/cull leaves OWNED (later-189):
  - `FUN_8007778C` = `ov_cull_wrapper` (game_tomba2.cpp): camera-relative delta (obj−cam, wrapping s16) +
    zero the two cull scratchpad flags (0x1F800080/84) → `rec_dispatch` the owned cull body 0x8007712c
    (routes through ov_object_cull for current-object tracking + margin). Verified 0-diff 40000+ via `cullwrap`.
  - `FUN_80051464` = `ov_xform_propagate` (engine_submit.cpp): child-node transform propagation. Orchestrates
    the owned rot_x/y/z + matmul(80084110) + MVMVA(80084220) in the recomp's exact jal order (preserving the
    matmul→MVMVA GTE-CR coupling), seeds the scratchpad identity work matrix, accumulates parent translation.
    Parent = node itself (sentinel c[6]==-1) or sibling node[0xC0+4*c[6]]. Verified 0-diff 6000+ via `xformverify`.
  - `FUN_800517BC` = `ov_settrans` (engine_math.cpp): SetVector 0x20-byte block (a1/a2/a3 s16 at +0/+8/+16,
    rest 0). Pure leaf. Verified 0-diff 30000+.
  - `FUN_800851F0` = `ov_rot851F0` (engine_math.cpp): CPU RotMatrix twin (non-GTE companion to 80085480);
    pure LUT trig, distinct layout, two elements negate-before-shift (nsh12). Verified 0-diff live (rare path).
    A dependency of FUN_800597AC. NOTE: resident-math overrides latch their verify-gate `v` at boot (the REPL
    `debug mathverify` lands too late), so verify them with a one-time FORCED-verify build, not the channel.
- ✅ WORLD-TRANSFORM orchestrator OWNED (later-190):
  - `FUN_80084360` = `ov_compmatlv` (engine_math.cpp): libgte CompMatrixLV — compose M ← R × M IN PLACE.
    Identical product/clamp/leftover to `ov_mat_mul` (P=R·M), only in-place into a1 + v0=a1. GTE-exact,
    0-diff via forced-verify (thin coverage in seaside, raised by its consumer FUN_800597AC). ALSO: made
    `ov_mat_mul` + `ov_compmatlv` faithfully CTC2 R→CR0-4 (the real bodies do; was relying on a prior
    80084470's CR write) so a following `ov_apply_matlv` (reads CR0-4) gets the right matrix robustly.
  - `FUN_800597AC` = `ov_orch597AC` (engine_submit.cpp): per-object world-transform orchestrator (3.8% field
    hot). Builds node+0x98 render matrix (SetVector + RotMatrix + CPU-rot + matmul + CompMatrixLV + 80084470),
    optional SECONDARY transform (node[0x145]/0x146 gated → 0x1F800060 + trans 0x1F800074..7C), then child
    propagation over node[0xC0+4i] with parent select (child[6]<0 → this node, via s6/s7 picking node vs the
    secondary matrix; child[6]>=0 → sibling node[0xC0+4*child[6]]). node[8] temp-forced to node[9] for the
    loop, restored on exit. rec_dispatch'd primitives in exact jal order (preserves CR coupling). Verified
    0-diff 2800+ live calls via `orchverify` (lazy first-call gate → REPL `debug orchverify` works).
  - `FUN_8002B278` = `ov_cone_cull_2b278` (game_tomba2.cpp): standalone view-CONE cull (3.9% field hot).
    Multiply-form of FUN_8007712C's cone test with fixed {near=512,far=7169,thr≡856}: dist=isqrt16(dx²+dy²+
    dz²) (dx=node->h[0x2C/2E/30]−cam@0x1F8000D2/D6/DA); reject if dist<512 or ≥7169; keep iff fwd·d ≥
    dist*3424 (fwd@0x1F8000E8/EA/EC; 3424=4*856, no-divide form). On keep sets visible flag node[1]=1,
    returns 1. Pure leaf (only owned isqrt). Verified 0-diff 20000+ via `conecull`.
- ✅ AUXILIARY render walks `0x8003BCF4` / `0x8003BF00` / `0x8003EEC0` = `ov_rwalk_aux_*` (engine_submit.cpp,
  issue #4): faithful per-node lift of each recomp body + per-node `gpu_obj_depth_add(world-pos depth)` so
  flame/rope/effect billboards occlude by real world depth (was: flat 2D band → drew over foliage). Field
  verified safe (renders intact, ndepth obj_depth spans 3→5 = the 2 aux queues now contribute). The FLAME
  occlusion itself awaits the flame-hut scene + USER eyeball (not headless-reachable yet).
- ✅ Render ORDERING: engine RenderQueue owns VISIBILITY (3D = real D32 depth buffer; 2D = enumeration order).
  Prims enumerated in OT LINK order (later-178) — NOT to honor PSX visibility (engine owns that) but to apply
  GP0 STATE (E1 texpage/E2 texwindow) in DRAW order so each 2D sprite binds the right texpage. later-172's
  linear packet-pool walk was REVERTED: memory order ≠ draw order broke 2D (title bg sprites went black).
  DrawOTag `0x80081560` = `ov_draw_otag`. Native projection `proj_native_vertex` (float matrices, real depth).

## E. PLATFORM services (native; the "remove Beetle by porting callers" axis — NOT game logic)
- ✅ CD/disc (cdc_native, cd_override, disc), ✅ XA stream (xa_stream), ✅ libsnd BGM (ov_bgm_*) + SPU voice.
- ⬛ Still-linked Beetle: `gte.c` (GTE — used by the still-PSX cull NCLIP + content collision; goes when those
  callers are ported), `mdec.c` (FMV), `spu.c`. These vanish only by porting their CALLERS, never by re-emulating.

## G. Save / Load FLOW  (engine/save.cpp)
- ✅ `FUN_80036DFC` save/load-flow HEAD dispatcher = `ov_save_dispatch` (engine/save.cpp, `save_register()`).
  The save system is a 6-state machine whose body is the active save-menu task's handler; this is the FLOW
  HEAD reached from the title "Load Game" / pause-menu "Load data". RE: reads SUBSTATE = task[1] (a0=task),
  bounds `<6`, dispatches via the handler table `0x80010668` → `jr table[substate]` (a TAIL-CALL; the page
  handler unwinds the dispatcher's 0x30 frame itself). s1=task, s0=`0x800D1E68` save-context struct. Handlers
  (table 0x80010668): [0] 0x80036E48 LOAD-SELECT, [1] 0x80036E58 LOAD-RUN (slot nav), [2] 0x800371D4
  SAVE-CONFIRM, [3] 0x80037360 SAVE-EXECUTE (sets flags 0x800BF809/0A/0B/3A + kicks writers), [4] 0x800375E0
  FORMAT, [5] 0x80037638 DELETE; substate≥6 = no-op return (epilogue 0x800376D4). The dispatcher does NO work
  beyond the bounds-check + table resolution + tail-call → owned native; the page handlers + the libmcrd card
  I/O LEAF stay PSX (the card frame R/W B0:0x4E/0x4F + SwCARD completion are PLATFORM, already native in
  runtime/recomp/memcard.cpp). Same shape as ov_render_cmd / ov_disp_26c88.
  - **No discrete game-side SERIALIZE/DESERIALIZE fn exists in this title:** the save data IS the live game
    progress block; persistence is done by libmcrd writing/reading card frames directly through the BIOS file
    API (open/read/write via 0xB0/0xA0 BIOS-call trampolines) — that whole path is library/PLATFORM, reached
    only through trampolines (no direct game-code jal into it), already owned by memcard.cpp's HLE. So the
    ENGINE-ownable save/load LOGIC here is precisely this FLOW state machine. (Evidence: file-API stubs
    0x800808B8/C8/D8/E8/F8 have ZERO direct jal callers; libmcrd internals FUN_8009Bxxx/8009Cxxx are reached
    via 0xB0 trampolines; the SAVE-EXECUTE handler 0x80037360 contains only flag writes + sub-calls, no bulk
    game-RAM→buffer copy.)
  - **VERIFY:** `saveverify` REPL channel = a NON-DESTRUCTIVE dispatch-decision gate (the page handlers can
    YIELD across frames — SAVE-EXECUTE commits the card write over multiple frames — so a double-running
    snapshot/rollback/super-call A/B is UNSAFE; the gate instead confirms the native body resolves the same
    handler the recomp `jr table[substate]` reads, by construction identical, and that the override FIRES at
    sane substates; the handler then runs exactly ONCE). **EXERCISE COVERAGE: the dispatcher is NOT reliably
    reachable headless** — its only trigger is interactive title/pause menu navigation into the file flow,
    which docs/driving-the-game.md §5 flags as only partly solved (cursor must land on "Load data" + confirm +
    spawn the file machine; the auto pause-menu nav attempts didn't enter it). The dispatcher is RESIDENT
    MAIN.EXE (0x80036xxx), so the override IS registered and verified inert during boot/field (field stage
    0x8010637C reached cleanly, no regression). The dispatch body is transcribed faithfully from the disasm
    above; the `saveverify` gate fires + verifies the moment the menu IS navigated. (later-207.)

## ⬛ CONTENT — keep as recompiled PSX (do NOT reimplement)
Per-enemy AI / per-character behavior; physics & collision response (Tomba move/jump/land); level data payloads;
quest / event / progression / game-rule logic.

## F. PERF PROFILER (the #1-priority lever: hot interpreted fn → own it native → faster + more PC-native)
The port is interpreter-only, so every un-owned engine fn + all CONTENT runs through `interp_flat`. The
in-port profiler (later-186, `interp.cpp`) gives the TIME + FREQUENCY histograms to pick the next fn to own.
- **Drive:** REPL `prof start` / `prof stop` / `prof dump <path>`. Cost: one predictable branch when OFF.
  Buckets are 16 BYTES (aligned to function starts — 256B straddled adjacent small fns and mis-attributed,
  e.g. lumped the hot FUN_80084110 under FUN_80084080; fixed later-186).
- **Report:** `tools/prof_report.py <dump> --top N` aggregates PC buckets by enclosing function (resident list
  `tools/recomp/tomba2_funcs.txt`, which runs to 0x8018FBCC; addrs past MAIN.EXE file end are overlay code).
  Top-200 16B buckets cover only the hot tail (TIME % is of the sampled buckets, not all insns) — the RANKING
  is what matters; the FREQUENCY list (call counts) is exact.
- **ACCURATE HOT-LIST (field, newgame+skip 600, 300 frames — later-186; don't re-profile to re-confirm):**
  - **GTE matrix/vector TRANSFORM cluster ≈ 52% of hot time — THE dominant lever + the "remove Beetle GTE"
    axis. NEXT BIG ARC.** All resident libgte ApplyMatrix/RotTrans-family helpers that CTC2-load a matrix
    + run MVMVA/RTPS, write to a2: ~~`FUN_80084110` 16.2%~~ ✅ OWNED native (ov_mat_mul, GTE-exact 0-diff
    115k calls — §D; the PROVEN RECIPE for the rest: decode GTE cmds, reimplement MVMVA from gte.c, verify
    a2 + GTE output regs via the comparator excluding FIFO/LZCR). **THE WHOLE CLUSTER IS NOW ✅ OWNED +
    GONE FROM THE HOT-LIST (later-187):** ~~80085480 16.9%~~ ov_rotmat, ~~80085050 9.3%~~ ov_rot_z,
    ~~80084EB0 8.25%~~ ov_rot_y, ~~80084D10 7.5%~~ ov_rot_x, ~~80084220 2.3%~~ ov_apply_matlv (+ the earlier
    80084110 ov_mat_mul). Field run cumulative **42.93M→27.06M insns (−37%)**. (Historical note — the orig.
    plan said porting native needs: (1) a native fixed-point GTE-math layer (MVMVA with
    sf/lm/IR-saturation/MAC-overflow matching mednafen gte.c), (2) GTE-REGISTER inspection tooling (the RAM
    dump is BLIND to GTE regs — must verify GTE-state LEAKAGE that downstream RTPS reads), (3) per-fn a2 +
    GTE-reg comparator. Big but the single highest-value perf + 100%-PC-native move.
    **USER DIRECTIVE 2026-06-21 (the GTE is PSX HARDWARE; this is a PC game → that math must be PC-NATIVE,
    not a mimicked/emulated GTE):** order is **(1) PC-NATIVE the GTE MATH FIRST** — reimplement each cluster
    fn as plain C arithmetic (NO `gte_op`, NO Beetle). This is NOT a "tradeoff vs the boundary" and NOT the
    same as the later-171 NCLIP/AVSZ revert: later-171 was a RENDER op (the engine already projects PC-native
    with real depth, so replicating PSX NCLIP was pointless mimicry → bypass, don't replicate). These cluster
    fns instead feed retained PSX CONTENT (object position), so the C math must produce values in the game's
    fixed-point format — i.e. it must match the GTE's NUMERIC result so the content reads correct data (the
    content-interface gate, CLAUDE.md). That is correctness, not GTE-hardware emulation. Verify a2 output +
    the GTE data regs the fn leaves that a still-PSX gte_op reader would consume (the comparator excludes the
    FIFO/LZCR regs neither path writes — can't round-trip a FIFO restore). **(2) CALLER ANALYSIS after**
    (FUN_80084110 ← 80084470 ← 80051980 = object rotated-displacement → world-position integration); **(3) a
    VULKAN/render PERF CHECK** (the profiler only measures interp insns, not GPU/present — needs a frame-time
    probe). End state: once the content consumers are ported too, even the fixed-point interface format goes
    away. The transitional `gte_write_data` of leftover regs is just to keep the remaining gte_op readers correct.)
- **PROFILER OVERLAY RESOLUTION (later-188b):** prof_report.py now merges the FREQUENCY call-targets into
  the TIME boundary set, so merged overlay buckets split at real function starts (`ov_<addr>(>enclosing)` =
  call-target-resolved, VERIFY with disas before porting — may occasionally be a jump-table re-entry, not a
  fn start). This corrected "FUN_8013F0DC 13.9%" → 4 distinct fns. **Run `prof_report.py <dump> --top N`.**
- **HOT-LIST after the GTE cluster + cull + tile-leaf (field, later-188b; cumulative total now 24.46M
  insns):** the SECOND ARC is the OVERLAY render. % is of sampled buckets:
  - `FUN_80115598` **39.4%** — **THE lever.** OVERLAY 2D scrolling-tilemap RENDERER (reads tile dims +16/+17,
    screen pos +40/+42 centered 160/120; builds GP0 sprite packets + OT). RENDER-boundary → reimplement as a
    PC-native 2D layer feeding the engine RenderQueue (NOT a packet/OT transcription). Register via the
    SIGNATURE scan in engine_scan_overlay (engine_submit.cpp) — the gameplay overlay loads at 0x80108F9C+
    0x459A8. Can't headless-verify render → USER eyeballs a build. Biggest single fn.
  - ~~`FUN_8013F0DC` **4.2%** + `ov_8013F988` 4.0% + `ov_8013FA80` 1.7%~~ ✅ OWNED (later-191, ov_tilescan).
    **The bucket label was WRONG:** these are NOT the anim/scale SM at 0x8013efa8 (the 21-way jump-table fn
    the handoff named is COLD — 3 calls/run); the hot code is the per-object TRIANGLE-SCAN SOLID-TILE
    GATHERER `FUN_8013F4DC` (0x8013f4dc..0x8013fadc), whose loop body lives at 0x8013f9xx and bucketed under
    FUN_8013F0DC only because that's the nearest call-target boundary below it. Clean integer data,
    `tilescanverify` 0-diff 600+ — see §D. (LESSON: when a profiler bucket spans a big range, disas to find
    the REAL fn entry + verify which entry actually fires before porting — the boundary label can be a
    non-entry.)
  - ~~`FUN_8007712C` 11.2% per-object CULL~~ ✅ OWNED (later-188, cull_native_body, §D). ~~`ov_8013FAE0`
    4.25% tile-lookup leaf~~ ✅ OWNED (later-188b, ov_tile_lookup, engine_submit.cpp; pure `tab[52*a1+a0]
    & (mask<<4)`, signature-registered, tileverify 0-diff 60000+ calls).
  - resident leaves still visible: `FUN_800931C0` 6.5% (font/text → render), `FUN_801401B8` 4.1% (overlay),
    `FUN_8003F698` 3.0% (26k×2 calls). ~~`FUN_80051464` 3.8%~~ ✅ OWNED native
    (later-189, ov_xform_propagate: child-node transform propagation orchestrating the owned rot/matmul/MVMVA
    primitives; verified 0-diff 6000+ via `xformverify`). ~~`FUN_800597AC` 3.6/3.8%~~ ✅ OWNED native
    (later-190, ov_orch597AC: per-object world-transform orchestrator; needed new leaves 0x80084360
    CompMatrixLV — ALSO owned (ov_compmatlv) — 0x800517bc + 0x800851f0 already owned later-189; verified
    0-diff 2800+ via `orchverify`). With the orchestrator + its deps owned, the resident transform/cull
    cluster is essentially exhausted — the remaining BIG levers (FUN_80115598 ~49% tilemap, FUN_800931C0
    ~9% font) are RENDER-boundary (REIMPLEMENT as PC-native 2D layers, USER eyeball — see §B targets below).
    ~~`FUN_80084A80` 4.4%~~ ✅ OWNED native (later-189, ov_rot84A80 RotMatrix variant; verified 0-diff 5000+).
    ~~`FUN_8007778C` 3.4%~~ ✅ OWNED native (later-189, ov_cull_wrapper: camera-relative delta + flag reset →
    rec_dispatch the owned cull body; verified 0-diff 40000+ via the `cullwrap` channel).
  - ~~`FUN_80084080` 9% (mis-attributed; real 0.5%)~~ ✅ OWNED native (later-186, ov_gte_norm) — table sqrt via
    LZCR→LUT, GTE used only as CLZ; verified 0-diff 15000+ live calls. ~~`FUN_80077FB0` isqrt~~ ✅ OWNED. See §D.
  - ~~`FUN_80076D68` 3.6% (no GTE) = animation-sequence VM stepper~~ ✅ OWNED native (later-198,
    ov_anim_vm_76d68, game_tomba2.cpp) — control flow + memory ops owned, the 3 callees (80075f0c applier /
    80076904 frame loader / 80075ff8 frame executor) stay PSX via rec_dispatch. `animvm` full-RAM+scratchpad
    A/B gate 0-diff 4000+ calls across all 4 movement directions (advance / follow-jump / freeze / executor).

---

# CURRENT FRONTIER (work these, in this order)
**SESSION 2026-07-01 (later-286) — free-roam recomp-MISS FIXED; NEW frontier = MINIMIZE RECOMP DEPENDENCY
(top-down descent, `recdep`-prioritized).** The free-roam abort (later-284c/285's frontier) was a recompiler
bug — `emit.py` dropped a fall-through edge, leaking 0x28 of guest sp per field frame until the render pass
overflowed task0's stack (findings/render.md). Fixed; `newgame; run 6000` holds free-roam steady.
**USER DIRECTION (2026-07-01): reduce recomp dependency to as little as possible.** The `recdep` channel
(`PSXPORT_DEBUG=recdep`, docs/config.md) measures it: 410 unique substrate fns run per free-roam frame.
The lever is TOP-DOWN ownership — own `ov_field_frame`'s direct substrate children, and wire their leaf
calls (rand 0x8009A450 @86/frame, matrix 0x80051794, libgpu 0x80082xxx/0x80083xxx) to the native subsystem
impls, so the hot leaves get captured as their parents become native. Landed this session (0-diff RAM+spad
A/B verified): `ov_list_walk_69b28` (FUN_80069b28) + `ov_arr8_dispatch_26368` (FUN_80026368) — two more of
ov_field_frame's 9 substrate children owned native (was 2 native/9, now 4 native/9).
- ✅ **DONE (later-289) — the two PURE engine children owned native** (0-diff RAM+spad A/B @ f1500, 0
  recomp-MISS @ f4000): `ov_scene_25588` (FUN_80025588, the field EVENT/COMMAND-FIFO state machine, struct
  @0x800ed058) + `ov_scene_4fe84` (FUN_8004fe84, the 2-phase scene/render-list builder driver, struct
  @0x800bf548), both in game/scene/engine_stage.cpp above ov_field_frame; their 10 leaf callees stay
  substrate. ov_field_frame is now 6 native/9 direct children. (Also fixed a codemap heuristic gap: an
  orphan doc-comment block glued to the next def, hiding the new addresses — separated with a blank line.)
- ☐ **NEXT — the content-heavy dispatchers** (engine_stage.cpp ~L285, still `d0(...)` in ov_field_frame):
  0x80050de4 (42-overlay-calls, scene-table @0x800f2418), 0x8001cac0 (22-way state dispatch @0x800bf870),
  0x8006ec44 (scene-event @0x800e8008) — they descend into per-area A00 object behaviors (the 0x8013xxxx
  handlers, ~5/frame each in recdep). Method: reimplement the DISPATCHER faithfully, route methods via
  dispatch_native_behavior|rec_dispatch, A/B RAM+spad 0-diff (build native vs `git stash` substrate, dumpram
  at f1500, cmp). Re-run `recdep` after each to re-rank.
- ✅ **DONE (later-286) — free-roam aborts ~f1184 at `jal 0x80109450`.** Root cause was NOT residency or the
  A00 field-frame flow — it was the recompiler fall-through/sp-leak (findings/render.md). Fixed in emit.py +
  seeded 0x8003D5CC/0x8003D8AC.

**SESSION 2026-07-01 (later-284b) — intro-cutscene FREEZE + red corruption FIXED; NEW frontier = free-roam
`jal 0x80109450` recomp-MISS.** The later-284 root-cause (PSX-render-underneath recursing deep on task0's
~2KB guest stack → sm[0x48]=17 clobber → freeze + game_coop r29 SP-leak → red corruption) is FIXED by
DELETING the redundant underhood re-render in ov_field_frame (engine_stage.cpp) — `dv_restore_pre` alone
keeps guest state PSX-correct (nothing consumes the PSX-built OT/packets; the native display re-derives from
node data). VERIFIED: oraclediff convergent native-vs-oracle through free-roam onset; the opening now PLAYS
(narration→cliff→walkable field) and renders clean (fx_700/1000/1145). Overturned the handoff's assumption
that dv_snapshot/restore had to go too — the rewind is a valid decoupling mechanism; the render bug only
needed the redundant re-render gone. (Making ov_render_frame write ZERO guest memory → drop the rewind for
perf = a valid FOLLOW-UP, not required.)
- ☐ **NEXT FRONTIER — free-roam aborts ~f1184 at `jal 0x80109450` with A00 resident in the MODE slot.** The
  GAME-stage dispatcher 0x8010882c does a hardcoded `jal 0x80109450` when sm[0x4c]==0 && sm[0x4e]==1. In SOP
  0x80109450 is a fn; in A00 (correctly resident at free-roam) it is a jump-table (data). Both native & oracle
  reach it with A00 resident → NOT a residency divergence. Decode the A00 field-frame flow (is this branch even
  meant to run under A00? is A00's 0x80109450 reached via the pointer table = indexed jalr, not this direct
  jal?), then own the A00 field-frame handler native or seed the right entry — no empty stub. Full writeup:
  docs/findings/render.md "Free-roam recomp-MISS: jal 0x80109450".

**SESSION 2026-07-01 (later-283) — INTERACTIVE-PLAY oracle scan added; the render-leaf frontier below was
STALE (corrected).** Two corrections + one new capability this session:
- **CORRECTION: `ov_terrain` is NOT orphaned — it is wired and FIRING in the live free-roam field.** The
  later-282 block below (and the handoff) claimed the PC-native terrain was orphaned with no caller and that
  the field terrain "renders via the PSX substrate". FALSE, verified live: `ov_render_walk` (called from
  `ov_sop_field_update`, sop.cpp:218) routes the default-case terrain fn 0x8002AB5C → `ov_terrain` →
  `terrain_render_pc`, which fires every field frame (node 0x800ED8D8/0x800ED960, `debug terrgte`/`terrpc`
  confirm — draws its geomblk quads PC-native, float transform + real depth). The field GROUND is ALSO native
  (`ov_field_entity_render`, 0x80109fe0, table 0x800F2418). So "wire the orphaned ov_terrain" = ALREADY DONE.
  At the free-roam opening the ONLY still-PSX render dispatches are GTE/scratchpad *setup* leaves (0x8003C2D4
  matrix-compose, 0x8013DD34 cull/bound) — intentionally left PSX (transcribing GTE compose is banned by the
  RENDER directive), and the per-object effect cases idx1/2/3/8 do NOT fire at the opening (`ccase` silent).
  ⇒ **The render-leaf frontier is DONE-or-BLOCKED at the verifiable opening**: what remains is either
  correctly-PSX setup, or effect leaves that only fire in LATER AREAS (unreachable/un-oracle-gated at the
  opening). Advancing them needs a scene that exercises them — i.e. extend the oracle envelope first.
- **NEW (harness): `oraclediff` interactive-play SCAN scaffold (selftest.cpp run_oraclediff).** From the
  aligned free-roam onset it drives BOTH cores with IDENTICAL pad input (hold D-pad Right) for 90 frames, then
  dumps both framebuffers. **LIMITATION (later-283, verified — corrects the first commit's overclaim):** at
  this checkpoint Tomba is STILL in the scripted "caught on the fishing line" pose and does NOT respond to
  movement input — holding Right (or mashing buttons) for 1400+ frames leaves him in the EXACT same position in
  the native core. So the scan currently only RE-CONFIRMS the still-frame convergence already proven at the
  onset (later-282); it is NOT yet an interactive-MOVEMENT test (there is no movement to diff here). The
  framebuffer match is real but is the same still-convergence, not proof of interactive-gameplay convergence.
  (The RAM diff during the walk is render-path NOISE — gameplay+render share node structs; native-VK vs PSX-
  soft-GPU populate node render-caches / OT 0x800ED000..0x800F1000 / render-queue lists 0x800F24xx differently.)
- ☐ **NEXT (real frontier — reach actual player control, THEN validate interactive convergence + advance):**
  the opening free-roam onset is a scripted caught pose, not player control. Find how Tomba becomes
  controllable (progress/skip the caught opening — button-mash and long walk-holds do NOT free him; needs RE of
  the opening sequence / the fisherman-pull event). Then (a) run the interactive scan THERE to actually verify
  movement/physics convergence, and (b) drive to the SECOND area (area transition + loader), which exercises the
  effect cases idx1/2/3/8 + per-area submitters — add an oracle checkpoint, own those leaves gated. The opening
  STILL-STATE + render is fully verified; the untested frontier is actual interactive control and beyond.

**SESSION 2026-07-01 (later-282) — the ORACLE now verifies the field; free-roam opening is CONVERGENT.**
The in-process interpreter+softGPU oracle (docs/oracle.md) has been extended to FREE-ROAM: it drives past the
intro narration into the walkable field and stays alive, and `PSXPORT_SELFTEST=oraclediff` now state-syncs the
native core vs the PSX oracle at a free-roam checkpoint. RESULT: the native free-roam opening is CONVERGENT —
RAM (only PRNG/ring/stdio drift) AND render (native VK vs PSX soft-GPU dumps are a near-perfect content match,
scratch/screenshots/oraclediff_freeroam_{native,oracle}.ppm). So the opening gameplay has NO divergence to fix.
- **⇒ THE ORACLE IS NOW A RENDER GATE for the frontier work below.** The old "Verify = USER eyeball (no render
  A/B gate)" caveat is SUPERSEDED for anything reachable in the field: after wiring/owning a render leaf, run
  oraclediff and compare the state-aligned native-vs-oracle framebuffers (and the engine-band RAM diff) BEFORE
  asking the user. Own render leaves top-down, then gate on the oracle. (See docs/oracle.md later-282.)
- ~~☐ NEXT: wire the orphaned PC-native `ov_terrain` into the field terrain path~~ **← CORRECTED by later-283
  (see block above): `ov_terrain` was NOT orphaned; it is wired+firing. The opening render leaves are all
  native-or-intentionally-PSX. This item is DONE-or-BLOCKED — the real next step is extending the oracle
  envelope past the opening area (later-283 NEXT).**

**SESSION 2026-06-24 (later-221/222) — FRONT-END bugs fixed + render-walk RE-WIRED into the C spine.**
- ✅ LOAD GAME owned (DEMO s4 native, `demo_frame_s4`/`load_machine_s4`, engine_demo.cpp) — fixed the freeze
  (s4 was the unhandled `default` in ov_demo_frame) + the cancel-replays-OP-movie bug (root prologue
  s2=1/s1=2/s3=3 → sm[0x48]=2 title, not 1). Renders the real PS1 memory-card slot browser. Memcard I/O is
  already sync+instant (BIOS B0/A0 card HLE, memcard.cpp). (later-221)
- ✅ Front-end menu music stops on exit (cd_override.cpp `ov_voice_stop` clears `cd.pending_music` when
  !dialog) — the dialog-coord was resurrecting the stopped Load/Options menu clip. (later-221)
- ✅ **Object RENDER-WALK 0x8003c048 RE-WIRED into the field spine** (sop.cpp ov_sop_field_update now calls
  the native `ov_render_walk`, not rec_dispatch). It was ORPHANED by the override-table removal — so the
  PC-native per-object world-depth (gpu_obj_depth_add) + the 60fps billboard reprojection (fps60_bb_node)
  were DEAD. Verified `[rwalk] NATIVE walk active` at the field. **THIS is the pattern the user wants** —
  the ires/wide/60fps "we used to have with overrides" are orphaned natives; re-wire them top-down. (later-222)
- ☐ **NEXT (render PC-native, top-down) — the spine's render LEAVES are still PSX:**
  - `ov_terrain` (engine_submit.cpp → terrain_render_pc, PC-native float terrain) is ORPHANED — NO caller.
    Wire it where the field renders terrain (confirm path: 0x80109fe0 tile render uses GTE control words +
    per-tile FUN_801099b4/80109c80; vs the geomblk terrain gen_func_8002AB5C that ov_terrain rebuilds — RE
    which one the live field uses, scratch/decomp/field/{80109fe0,8010a0e0}.c).
  - per-object TRANSFORM 0x8003CCA4 — ✅ ALREADY PC-NATIVE (note was STALE, corrected 2026-06-25). The
    transform+projection is `submit_perobj_flush` (engine_submit.cpp:793) → `eproj_compose_object`
    (engine_project.cpp:32, float Rcam·Robj/4096 + Rcam·Tobj/4096+Tcam) → `native_gt3gt4` (float-projected
    GT3/GT4, real depth) — NO gte_op, NO CR0-7, NO PSX packet. `submit_perobj_render` (engine_submit.cpp:950)
    only `rec_super_call`s the SECONDARY-EFFECT cases idx 1/2/3/8 (FUN_8003f4c4/f3f4/d584/f594/f344), which
    are GP0-PACKET post-passes (semi-transparency/blend/brightness), NOT transforms — and they DON'T fire at
    seaside (only idx0 flush-only). REMAINING per-object-render work: (1) own idx 1/2/3/8 as RQ-submit
    effects (semi/blend/brightness flags threaded into the render-queue item, not packet-byte patches) — only
    verifiable in an area that uses them (find via `debug ccase`); (2) the widescreen-margin path
    (margin_render.cpp:62-64 still rec_dispatches T2_BUILD_XFORM+T2_PEROBJ_RENDER → route through
    submit_perobj_render so wide edges project via the float path); (3) 0x8013DD34 in ov_bg_render =
    scratchpad temps only, leave PSX. Verify = USER eyeball (no render A/B gate).
  - then ov_render_cmd (0x8003F698) + the per-type render handlers (cases 1-8,0x10-0x14 in 0x8003c048).
  - TOOLING established this session: Ghidra-headless decompile pipeline for overlay code — import a RAM dump
    (base 0x80000000) → analyze → DecompDump.py per-fn clean C (scratch/decomp/). Field dump =
    scratch/bin/field_game_ram.bin (Ghidra proj tomba2_field). DEMO dump = scratch/bin/demo_title_ram.bin.


**SESSION 2026-06-23 (later-215a..d) — NEWGAME NOW BOOTS INTO GAMEPLAY (the override-removal regressions fixed).**
The GAME-stage derail/hang was a stack of THREE override-removal casualties, all now fixed (see journal
later-215a..d for full RE; scratch/{overlay_seq,sop_mode,level_layout}_re.md for the maps):
- **215b** SOP field-MODE overlay loaded to the wrong addr (0x80118f9c vs 0x80108f9c) in native DEMO s0 →
  GAME `jal 0x80109450` hit zeroed RAM → derail. (Two overlay classes share slot 0x80108f9c: MODE
  overlays SOP/OPN/CRD idx 0/1/2 have the per-frame field-mode fn at 0x80109450; AREA overlays A00.. idx 3+
  have a data table there. New-game needs SOP.) Fixed engine_demo.cpp.
- **215c** the cooperative YIELD was disconnected: ChangeThread FUN_80080880 (which FUN_80051f80 yield +
  FUN_80051fb4 task-end funnel through) lost its ov_switch wiring → every GAME per-frame yield spun. Re-wired
  via platform-HLE (sync_overrides.cpp, window widened to 0x80080000-0x8009E000). Restores the whole coop scheduler.
- **215d** the CD-subsystem native HLEs were orphaned (only FUN_8001DC40 migrated) → the XA reader spun in
  libcd CdSync. Re-registered the original set in cd_overrides_init (skip 0x8001D940 core + 0x8008B2D8).
- **RESULT (VERIFIED, headless + USER eyeball):** newgame → DEMO → GAME → SOP mode machine (sm[0x50] LOAD→
  FADE→GAMEPLAY), 200+ frames ZERO derail/hang, the OPENING CUTSCENE renders (intro narration). Also:
  REPL `newgame` freezes at GAME entry for clean inspection; watchdog default-on 3s + SIGINT killability.
- ☐ NEXT: drive PAST the intro into the walkable field (skip/input), exercise movement+camera; own more of
  the chain native (SOP mode machine, area asset loader FUN_800754f4 — level_layout_re.md); then the
  later-214 OT-walk-enumeration retire + 3D-position ordering. Live windowed render fidelity = USER eyeball.


**SESSION 2026-06-22 (later-214) — MAIN MENU / TITLE FIXED (was frontier #1 "title renders BLACK").**
Two independent root causes, both override-removal casualties:
- **Title texture never loaded:** native DEMO s0 called `preload_texgroup(c, 2, 0)` but that fn is
  `(mode, set)` while the original `FUN_80044BD4(set=2, mode=0)` ⇒ args were swapped, loading the wrong
  group. The title page (set 2) was never uploaded to VRAM (640,256)/(768,256)/CLUT(640,511), so the two
  full-screen title sprites sampled empty VRAM. Fixed → `preload_texgroup(c, 0, 2)`; title page now in VRAM.
- **Render queue never drained (the big one):** `rq_active()` is always on, so EVERY prim that walks the OT
  is QUEUED (the OT draw-ORDER is discarded; the engine owns ordering). The queue drains via `rq_flush`,
  which lived ONLY in `ov_draw_otag` (the native DrawOTag) — ORPHANED by the override-table removal. The
  native loop interpreted the PSX DrawOTag (`rc1 0x80081560`) which did the walk+queue but NO flush, so the
  queue filled every frame and never reached the VK renderer (`vk tex=0`). The ENTIRE 2D front-end was black.
  Fixed: native_step_frame now calls `ov_draw_otag(c)` DIRECTLY (PC-driven), which walks+queues+`rq_flush`es.
- **VERIFIED:** headless title shot now matches the offline oracle (logo/brick/characters/copyright + the
  New Game / Load Game menu text). Boot → DEMO s2 clean. NB the engine OWNS render order (OT order discarded);
  this does NOT re-introduce OT-order rendering — the OT is read only to enumerate leftover guest 2D prims.
- ☐ NEXT (per user directive "all game render PC-native, no OT"): retire the OT-walk enumeration entirely —
  own the 2D submit (title/menu/HUD/font) at the source so prims are queued PC-native without reading the OT;
  and make in-game world prims order by real 3D position (not OT) — the sea-background-on-top class of bug.

**GAME-STAGE DERAIL — ROOT CAUSE FOUND (later-214, was "NEW FRONTIER" in later-212).**
- Repro: `newgame` reaches GAME prologue (0x8010637C) ~f26, then `[DERAIL] bad opcode 0x6E656874 ("then")
  at pc=0x80118FF8 ra=0x801088B0`. Diagnostic: `PSXPORT_DERAIL_DUMP=<path>` dumps 2MB guest RAM on the
  bad-opcode abort (interp.cpp) → disas.py --ram.
- The GAME area-machine handler (0x801088A8 `jal 0x80109450`) calls into 0x80109450, which is **all zeros**.
  RAM map at derail: GAME.BIN code = 0x80106000–0x80108FFF; **0x80109000–0x801187FF = ZERO (the AREA
  OVERLAY slot, never loaded)**; data at 0x80118800+ (the idx-2 file at 0x80118F9C). Execution nop-slides
  from 0x80109450 through the zero hole and hits the first data word at 0x80118FF8 → bad opcode.
- WHY the slot is empty: **task1 (the cooperative AREA-LOAD task) never spawns** — at the derail
  task0={state1,entry GAME,sm48=2 running}, **task1={state0}, task2={state0}**; area id 0x800bf870 = 18.
  So the GAME stage reached its RUNNING substate (sm[0x48]=2) WITHOUT ever loading area 18's overlay. The
  newgame hand-off (s5 → demo_start_stage(c,2) → GAME) does not trigger the first area-load.
- FIX DIRECTION: on GAME entry, drive the first area-load (area 18) like the REPL `warp` does
  (seed 0x800bf870 + restart the area-load task slot 1 at FUN_80051f14(1,0x800452c0), or run the GAME
  stage's own load substate) so task1 pulls the overlay into 0x80109000 over the following frames BEFORE
  the area machine calls 0x80109450. PITFALL (later-212): do NOT force-sync the shared async core
  FUN_8001D940 — it corrupts the overlay; let the cooperative task run. Verify: newgame → run, no derail,
  0x80109000 region becomes CODE.

**SESSION 2026-06-22 (later-213) — PC-NATIVE GAME LOOP: present + pace + per-vblank audio re-OWNED top-down.**
- **Root cause fixed:** the override-table removal (later-212) ORPHANED `ov_frame_update` — `gpu_present`,
  `gpu_pace_frame`, and the per-vblank audio (sequencer tick + `spu_audio_frame`×quota) + fps60 commit were
  reached only via the dead override table, so the live loop ran UNCAPPED (window to ~385k frames) and the
  presented frame could be stale (`gpu_present` was not called anywhere in the live loop — only native_fmv /
  SCEA splash / gpu_clear_display).
- **Fix (top-down PC-driven):** `native_step_frame` (native_boot.cpp) now calls `ov_frame_update(c)` DIRECTLY
  (plain C call) in place of the bare `rc0(c,0x800788ac)`. `ov_frame_update` (game_tomba2.cpp) is no longer
  static / no longer an "override": it runs the still-PSX per-frame leaf via `rec_dispatch` (was
  `rec_super_call`), then OWNS per-vblank audio + `fps60_frame_commit` + (`!fps60`) `gpu_present` +
  `gpu_pace_frame`. Present stays BEFORE the OT submit (exact override-era ordering: the VK batch shown is
  the one DrawOTag built last frame). Removed the later-212 stopgap pace in the for-loop (5369b42) — would
  double-pace now that ov_frame_update owns it.
- **VERIFY:** headless boot → DEMO s2 CLEAN (no trap/derail/bad-opcode/timeout); `gpu_present_ex` diagnostics
  (`projprim(vtx) records=`) now print every frame ⇒ present reached. Headless stays uncapped/fast (pacer
  no-ops with no window). **LIVE windowed pacing/audio/render = USER eyeball (`git pull && ./run.sh`).**

**SESSION 2026-06-22 (later-212) — FAIL-FAST timing/IO + front-end reaches the GAME stage.**
- **FAIL-FAST is now a hard rule (user; see CLAUDE.md "Hard rules" + memory).** The PC port does ALL I/O
  and timing synchronously+natively; any PSX async/wait primitive is made sync-native OR traps+aborts.
- **Platform-HLE table** (`runtime/recomp/sync_overrides.cpp` — `platform_hle_register`/`platform_hle_lookup`,
  checked in interp `coro_native_call` + `rec_dispatch_miss`). Restricted to PSX library / I/O addresses
  (CD-IO glue 0x8001Cxxx, SCEI libs 0x80082xxx-0x8009Cxxx); NEVER game/engine `FUN_xxxx`. Entries:
  - **VSync 0x80085900 → TRAP+abort** (every mode/caller). The native loop (`native_boot` for-loop +
    `gpu_pace_frame`) owns ALL pacing. Removed the instant-VSync bandaid + the boot VSync(3)/(1) calls.
  - libgpu GPU-DMA timeout `FUN_800834a0/d4`, libcd `CdReadSync`/`CdDataSync`/`CdInit`-handshake,
    libmdec `DecDCTin/outSync` → native no-ops (GPU/CD/MDEC are synchronous; no busy-wait, no timeout).
  - async CD read: inline loader `FUN_8001DC40 → ov_cd_dc40` (sync sector read). NB do NOT intercept the
    shared core `FUN_8001D940` — it's also driven by the cooperative area-load task and force-syncing it
    corrupts the overlay. CdInit fully native (`cd_hle_init`, replaces the recomp libcd CdInit 5-retry loop).
- **Bad opcode → abort** (`interp.cpp`) with derail PC + guest-stack backtrace. No more spew-and-limp.
- **Boot is CLEAN**: no CD/VSync/MDEC/GPU busy-waits, no `CD timeout`/`CdInit failed`. Reaches DEMO s2.
- **Intro FMV de-duped**: boot plays LOGO only; the DEMO menu machine owns OP.STR (was playing twice).
- **DEMO substates s3/s5/s6 OWNED in the native per-frame loop** (engine_demo.cpp `demo_frame_s3/s5/s6`):
  s3 main-menu sub-machine 0x80106AC4 (→s5/s6/s7/s2), s6 page sub-machine 0x8007B45C (→s3), and
  **s5 = LEAVE DEMO → GAME** (`demo_start_stage(c,2)` = FUN_80052078; the scheduler detects the entry
  change and hands off, native_boot.cpp). **VERIFIED: New Game (s2→s3→s5) reaches the GAME stage
  (sm[0x48]=5 → GAME 0x8010637C).** Attract (s7) + menu (s3) paths clean, no trap/derail.
- ☐ **NEW FRONTIER — the GAME stage derails.** Once in GAME (newgame), a GAME.BIN handler at ra=0x801088B0
  jumps to 0x80118FF8 (an overlay region holding TEXT, not code) → bad opcode → abort (fail-fast). PERSISTS
  after reverting the CD changes ⇒ PRE-EXISTING (override-removal casualty / area-load/handler), exposed now
  that the front-end reaches GAME. Needs a GAME.BIN+overlay RAM dump (the code is in the overlay, not static
  MAIN.EXE) at the derail to RE the broken dispatch/overlay-load. Likely tied to the cooperative area-load
  task (FUN_80044bd4 → FUN_8001D940 async) not delivering the area overlay's code.
- ☐ **STILL OPEN — title renders BLACK** (frontier #1, unchanged; the s2 attract render reaches no VK present).


- Found "the object spawn handler" the user wanted by going top-down: traced spawn callers at field-load via a
  new `debug spawntrace` channel (logs each spawn-entry's `ra`). The two dominant callers were `FUN_80072A78`
  (the per-area placement-table loop) and `FUN_80072DDC` (single-object spawn-with-parent helper). The driver
  is `FUN_80072A78`, called by the GAME-stage area machine (GAME.BIN `0x80106bf4`/`801072a8`/`801077f0`/
  `80108e14`) when a field activates. It reads the area's placement TABLE → spawns each object via the owned
  `FUN_8007A980` → stamps node identity/pos/facing/handler.
- **OWNED `ov_place_objects` (engine/entity_spawn.cpp).** Resident, no yield → plain override. `placeverify`
  full-RAM+scratchpad A/B = **seaside 0-diff** (both per-load calls), 0 bad opcode. Record format + table
  select fully RE'd → docs/engine_re.md "field OBJECT-PLACEMENT DRIVER". This is the real top-down placement
  spine: GAME sm[0x48]==2 → area machine → `FUN_80072A78` → `FUN_8007A980` → spawn primitive (all owned).
- **`FUN_80072DDC` ALSO OWNED** `ov_spawn_with_parent` (the 2nd dominant field-spawn caller; spawn + link
  parent + flag). `spawnparentverify` = 100+ field calls 0-diff, 0 bad opcode.
- **Per-object behavior HANDLERS (node+0x1c) — descent STARTED (later-211).** Seaside placement installs 22
  distinct handlers; 2 resident/generic. **`FUN_800739AC` OWNED** `ov_beh_739ac` (engine/objbeh_739ac.cpp):
  a state machine / scene-UI trigger, control flow + memory owned native, sub-calls rec_dispatched.
  `obj739acverify` = 1050+ field calls 0-diff, 0 bad opcode (idle path; input transitions transcribed).
- **`FUN_80073CD8` OWNED** `ov_beh_73cd8` (engine/objbeh_73cd8.cpp) — the resident sibling (~558 instrs, same
  SM shape but bigger: state-0 per-node[3] sub-switch JT 0x80016B68 seeding box/size fields; state-1 node[5]
  sub-machine JT 0x80016BE8 driving FUN_8007E110/80040B48/42728 + scene/save flags). Control flow + memory
  owned native, all sub-calls rec_dispatched. NB it calls cull FUN_8007778C but IGNORES the result (unlike
  739ac). `obj73cd8verify` = 1400+ field calls 0-diff, 0 bad opcode; live field renders clean.
- **`FUN_800741DC` OWNED** `ov_beh_741dc` (engine/objbeh_741dc.cpp) — the third resident handler that actually
  FIRES in the seaside field (probe: of {741dc,52078,499e8,4c930} only 741dc runs there). Item/pickup scene
  trigger: state-0 cull-init (FUN_80051B70 a1=1,a2=0x18) + node+0x56 from DAT_800a4cec; state-1 node[5]
  if-chain (scene-register FUN_8007E110 keyed DAT_800a4cf8, pad-edge, child-spawn FUN_8007413C bounded by
  DAT_800a4d04 vs DAT_800bf874; case-4 emits 2× FUN_80027144 packets + sets per-type collected bit DAT_800bfa23
  + 0x1f all-collected reward). Mirrors the recomp `sp-=0x30` frame so case-4's stack buffer lands above the
  sub-call frames. `obj741dcverify` = 500+ field calls 0-diff, 0 bad opcode; live field renders clean.
- NEXT top-down: the scene-overlay handlers (0x8012/0x8013xxxx) run only in OTHER scenes → NOT headless-
  verifiable in seaside (cross-area warp floods bad opcodes). Resident handlers reachable in seaside are now
  EXHAUSTED (739ac/73cd8/741dc all owned). Item 3 (FUN_800520e0 callees) was already fully owned (lines 104-119).


**SESSION 2026-06-21 (later-200) — FILE ORGANIZATION + "PC-owned applies to EVERYTHING".**
- **SWEEP landed (99e6df5):** `engine/game_tomba2.cpp` split 2042→300 lines into 8 discrete subsystem
  modules — `engine/{mathlib,cull,collision,entity,script,animation,input,menu}.{cpp,h}`. Pure file
  organization (USER goal #1: distinct per-subsystem files, no piling into one file). Verified 0-diff:
  full RAM+scratchpad A/B at the GAME field (newgame; skip 650; run 8) IDENTICAL vs pre-sweep main.
- **USER DIRECTIVE (2026-06-21): "PC owned" applies to EVERYTHING — incl. HUD/UI.** A native fn that
  mirrors the PSX stack frame, `rec_dispatch`es the PSX emitter, rebuilds the guest packet byte-for-byte,
  and gates on full-RAM 0-diff is STILL a PSX transcription — the WRONG deliverable. The RAM-byte gate is
  ONLY for the content-INTERFACE (guest RAM still-recomp content reads), NOT the definition of "owned".
  Own engine systems by REBUILDING PC-native + gating on the OBSERVABLE RESULT (USER eyeball).
- **REJECTED (not landed):** `engine/hud.cpp` (FUN_8007E938/8007E8DC) and `engine/inventory.cpp` —
  both built by prior subagents as PSX transcriptions (mirror frame + rec_dispatch + `hudverify`/`invverify`
  RAM-diff gate). hud.cpp even spilled a 0xDEAD0000 sentinel `ra` into a guest stack slot (the frame
  transcription leaking). HUD must be REBUILT: read gauge/weapon-icon STATE → draw textured quads with the
  PC renderer. Worktrees: agent-ae0c0c3e (HUD), agent-a10b61c3 (inventory).
- **LANDED this session (main b4c62ce):** entity_spawn (FUN_80079C3C) + inventory (FUN_8004D338 + wrappers)
  + area cel-load (FUN_800753D4) all PC-native 0-diff (71acfd9); physics move-and-collide RE trace, no clean
  shared integrator to own → per-actor SM is the unit (2bd2f8c); fps60 dynamic-shadow STROBE FIXED (167c574,
  keep_shadow across the two presents); retired SW fps60 re-rasterizer REMOVED −515 lines (405cae1); texture
  VRAM-transfer path owned + `vramguard` live atlas-clobber catcher (b4c62ce — corruption is timing-driven,
  catcher names the offending GP0-0x80 copy when reproduced live; the FIX of that path is the followup).
- **DUST: PARKED by user.** It is a quad-cluster particle effect, not a sprite strip; format owned + tooling
  in 85ebb98 (tex_export --scanbav/--bavsheet/bav_layout, REPL `vramraw`). Finish = decode per-quad records.
- **HUD rebuilt PC-NATIVE (88741d6):** engine/hud.cpp draws the gauge/icon as textured quads on the engine's
  own 2D overlay layer (gpu_gpu_draw_tritri + gpu_gpu_set_order_2d), owning overlay order — NO GP0 packet/OT/
  rec_dispatch. Engine owns visibility → draws all 3 indicator sprites (the PSX 4:3 da-clip ate 2; #14 fix).
  Content interface 0-diff (all field-A/B diffs in the PSX render packet pools the native HUD stops writing).
  TODO: glow-pulse modulation; panel/numeric-glyph counts + quest-banner text (state not yet RE'd).
- **Per-area LIGHTING (e03a7ad):** engine/lighting.cpp per-area registry; Fisherman Village (key 0xca184188) →
  warm SUN; mine lava/torch CFG scaffolded (needs a mine area key). Integrated at engine_shade_face (per-face,
  not the deferred pass). Render-only, gated on g_mods.light; light-off field dump byte-identical to pre-lighting.
- **60fps design (user-chosen):** interpolate CHARACTERS only; static world/terrain/2D/shadows from the REAL
  composite. Live tier = actor-transform VK (fps60_present_vk/build_lerp).

**USER REDIRECT 2026-06-20 (later-177) — supersedes the camera items below.** Port top-down boot→gameplay;
the ASSET PIPELINE must be fully PC-owned so assets reconstruct with the emulator OFF. Acceptance test:
reconstruct the sea backdrop bit-identical.
**STATUS: the texture asset pipeline is now FULLY PC-OWNED and the acceptance test PASSES (later-177).**
Owned this session: the unpack post-step (libgs DrawSync dropped from the synchronous native upload path), the
per-group LOADER ORCHESTRATION `FUN_80044F58` → `ov_load_texgroup` (header+archive CD-load, unpack, metadata,
terminal yield), and the OFFLINE reconstructor `tools/tex_reconstruct.c` that rebuilds the seaside VRAM atlas
from raw compressed bytes with NO PSX — 99.95% bit-identical to live VRAM, residual = runtime-animated CLUTs only.
**title/menu BG render: FIXED (later-178).** The title's two full-screen background sprites rendered BLACK
because later-172's linear packet-pool walk decoupled each 2D sprite from its E1 DR_TPAGE (memory order ≠
draw order); restoring OT LINK-ORDER enumeration in `gpu_dma2_linked_list` binds the texpage correctly. The
title now composites the full TOMBA!2 logo/brick/characters/menu; field unchanged. (NOT a missing-submission
or asset-pipeline bug — the assets were in VRAM all along; later-177b's root-cause was wrong, FALSIFIED.)
**Menu BG EXPORTED OFFLINE from the CHD (USER request) — `tools/menu_bg_export.cpp`.** Opens the disc,
resolves `CD/TOMBA2.IDX`/`CD/TOMBA2.IMG` by name, decodes the title set (set 2: LZ + unpack), composites the
320×224 title via 8bpp+CLUT sampling, writes a PNG — game NOT run, no OT, no runtime. Output matches
`fl_06.ppm` (sans the runtime menu-text prims). Run: `build/tools/menu_bg_export <disc.chd> out.png`.
**REMAINING under this redirect:** the on-screen 288×576 backdrop COMPOSITION (engine-owned 2D layer/sort,
ties into render-ownership, NOT the asset pipeline). (Camera sub-fns below are DEFERRED behind this.)


1. **Camera update system — DONE (later-180).** All `FUN_8006e0f0` sub-fns owned PC-native in
   `engine/engine_camera.cpp`: 0x8006c80c Y-floor, 0x8006d654 pitch, 0x8006dcf4 heading, 0x8006d02c
   orient/look-at matrix (rec_dispatches libgte/GTE as the math library per the RENDER boundary), 0x8006e010
   angle-step, plus the earlier 0x8006e464 rotbuild / 0x8006d2ac dist-solve / 0x8006d960/da54 track xz·y. The
   three per-mode ORCHESTRATORS `FUN_8006e0f0` (active follow) / `FUN_8006e228` / `FUN_8006e3f4` are collapsed
   native too. camverify is now an end-to-end gate (native-everything vs a PURE-gen oracle, sub-fn overrides
   cleared around the orchestrator rec_interp): e0f0 0-diff over 1000+ calls (AUTO_SKIP=500 AUTO_WALK=r);
   e228/e3f4 are latent alternate modes, faithfully transcribed, verify when a scene drives them. Two e228
   sub-fns (`FUN_8006dad8`/`FUN_8006def0`, a0-only) still route via rec_dispatch — own them if/when e228 is exercised.
2. **DEMO / front-end MENU stage `0x801062E4`** — the big un-owned system in execution order between boot and
   gameplay. Title→New Game. Own its substate machine PC-native. **FULLY RE'd (later-181) — see docs/engine_re.md
   "DEMO / front-end MENU stage" section**: root dispatcher + per-frame loop + the 8 substate handlers
   (s0..s7), their inner menu sub-machines, sm field map, transitions, and callees are all mapped.
   **PARTIAL — OWNED (later-182): substates s1/s2/s3/s6 owned PC-native in `engine/engine_demo.cpp`** (the
   ones whose only sub-call is SYNCHRONOUS — verified yield-free via the new `tools/yield_reach.py`). They
   rec_dispatch the inner machine, then own the transition LOGIC (sm[0x48] selection + field writes) natively
   and coro-redirect to the guest TAIL (like ov_game_s4c). Registered AUTO from the overlay-load scan
   (`demo_scan_overlay`, distinguishes DEMO from the GAME.BIN that aliases the same base via the root prologue
   sig 0x27bdffd0). **A/B gate PASSED**: override-on vs override-off at a steady menu frame (REPL `run 150`)
   = main-RAM 0-diff and **scratchpad 0-diff** except ONE word — task-0's saved-ra slot 0x801FE9CC
   (CORO_SENTINEL vs the guest return-PC), the inherent coro-redirect artifact, not behavioral state.
   `dumpram` now also dumps a `.spad` sidecar so the A/B covers scratchpad (was blind before).
   **s0 NOW OWNED (later-185)** via the coro-redirect-INTO-the-yielder handshake: it has genuine pre-yield
   engine state (sm[0x68]=0, sm[0x48]++, sm[0x4a]=0) owned native, then redirects to its first loader jal
   0x801063E4 (yields) so the loaders + fall-through to s1 run in-context. A/B (run 150) main-RAM + scratchpad
   **0-diff, no saved-ra artifact**. **s4/s5 STAY GUEST (final):** verified (later-185) the override table is
   consulted ONLY on jal/j/jalr/computed-jr targets, NOT on `jr ra` returns — so s4's only logic (the sm[0x6b]
   branch post-yield at 0x8010658C, reached by jr ra) is unreachable by an override; s5 is one stage-transition
   call. (The earlier "post-yield override" idea was retracted as not implementable.) **NEXT (this item): s7 —
   OWNABLE.** Its `jal 0x80106C24` is an override-checked jal target and 0x80106C24's phase-selection prologue
   (sm[0x4a]) is pre-yield + phase2 teardown is all-SYNC; own selection + phase2, redirect yielding phase0/1.
   Needs reaching s7 (confirm a menu option) to A/B-verify. Overlay disasm via
   `tools/disas.py <addr> --ram scratch/bin/tomba2/ram_menu.bin`.
3. **Init-prefix remainder:** `FUN_800520e0` engine subsystem init — ORCHESTRATION OWNED (later-183,
   eng_init_subsystems, boot A/B 0-diff); NEXT = descend into its 4 callees (8007b328/80088b00/80086620/87a60).
   `FUN_80075130` font/text init — ✅ DONE (ov_font_init, engine_font.cpp; 3 engine callees + memsets owned,
   libgpu callees kept dispatched in-context; boot run-2 0-diff vs initref.bin). See §A.

# OPEN / BLOCKED (not on the critical path)
- **ov_game_s4c verification** needs an in-game AREA transition (sm[0x4a]==2). Free-roam IS reachable headless
  (AUTO_SKIP=500 + AUTO_WALK) but the seaside exit isn't a plain walk/jump (hut door blocked by a barrel) —
  needs visual steering or RE of the exit trigger in GAME.BIN handler `0x801088d8`. (NB: there is NO pause menu
  — the reliable `PSXPORT_DEBUG=state` task-slot probe confirms 0 menu tasks; an earlier "pause menu" claim was
  a broken probe.) Controls (which button = jump) are not yet identified — leave it; it reveals itself in play.
