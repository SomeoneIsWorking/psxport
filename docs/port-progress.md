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
3. **Classify** (see § BOUNDARY): ENGINE → reimplement PC-native in `game/<subsystem>/`. CONTENT → leave PSX. PLATFORM
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
pinpoints the FIRST place pc_faithful diverges from recomp_path, with the exact corrupting-write GUEST
STACK TRACE. It is the self-contained oracle. **Gate EVERY subsystem rebuild on it.**

Vocabulary here matches CLAUDE.md: pc_faithful = default native OOP path; recomp_path = substrate
(psx_fallback=1). Both SBS cores get `mPcSkip=false` so the faithful branch of every fork runs.

- `PSXPORT_SBS_MODE=render` — A = pc_faithful + pc_render, B = pc_faithful + psx_render. pc_render MUST
  leave guest RAM identical to psx_render → drive `sbs diff` to ZERO. Nonzero = pc_render is writing
  guest memory (rule violation: pc_render is read-only from guest RAM). Each surfaced address is a leak.
- `PSXPORT_SBS_MODE=gameplay` — A = pc_faithful, B = recomp_path (psx_fallback=1), render identical. A
  native subsystem MUST byte-match the substrate → `sbs diff` ZERO. First nonzero + `sbs watch` +
  `sbs bt` = the exact instruction that drifted. Gate for owning any per-frame gameplay/AI/physics fn.
- `PSXPORT_SBS_MODE=full` — full pc_faithful vs full recomp_path (`both` still accepted as legacy alias).
  **This is Job #1's harness (2026-07-04).**
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
  - ✅ **`FUN_80045258` `Asset::loadDescriptorChunk` OWNED native (leaf indexed-chunk CD reader).** The
    collision-grid / area-8-extra-texgroup sub-chunk loader that the area-DATA load path (0x800452C0 and
    the SOP/transition mirrors) called ~5× via rec_dispatch. Body:
    `FUN_8001DC40((&DAT_800ECF58)[slot], DAT_800BE100 + ((&DAT_800FB170)[descIdx]>>11),
    (&DAT_800FB170)[descIdx+1]-(&DAT_800FB170)[descIdx])` — reads one CD chunk (byte-offset descriptor
    table @0x800FB170, word-indexed; sector = offset>>11, size = next-offset − offset) into the dest
    pointer already latched at module-pointer slot 0x800ECF58[slot] (slot 47/0x2f = collision grid,
    slot 8 = area-8 extra texgroup). Reuses `Cd::dc40Sync` (native FUN_8001DC40). Replaced all 5
    `rec_dispatch(0x80045258)` sites (asset.cpp ×3, sop.cpp ×3→ now direct calls). VERIFIED: SBS full
    autonav A/B IDENTICAL through f11250, zero sbs-div. 0x80044BD4/0x800452C0 left as substrate/existing
    native forks (see above) — they are the spawn-and-wait + task-body wrappers, not leaf targets.
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
    the per-object render GTE-free; audit remaining RTPS/RTPT callers at the field. NOTE: this whole
    paragraph predates issue #32 (2026-07-07, "PSX render path always executes underneath") — the
    "GTE-free picture" framing describes the READ-ONLY pc_render display pass (Render::perObjFlush /
    gt3gt4), a DIFFERENT thing from the guest-writing SUBSTRATE dispatch chain 0x8003CDD8/0x8003F698,
    which IS now owned (2026-07-08, see CURRENT FRONTIER above) as a byte-exact mirror (still GTE, by
    design — it feeds the SBS-gated faithful RAM, not a picture).
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
  - ◐ IN PROGRESS (gameplay top-down): descending SOP per-frame field update `FUN_801092b4`. Order
    matches call sequence in `Sop::fieldUpdate` (top → bottom):
    - ✅ scene cam-frustum PREPASS `FUN_8010A0E0` = `Sop::scenePrepass` (sop.cpp; Ghidra decomp
      scratch/decomp/sop_a0e0_a3ac.c). Per-frame 2D frustum triangle in scene-grid space (view dist
      0x5780, half-FOV 0x1C7, ÷640 grid scale); hands off directly to the native scanline gatherer
      below (no more guest-stack detour).
    - ✅ scanline triangle-raster GRID CULL `FUN_8010A3AC` = `scene_grid_gather` (static helper in
      sop.cpp; Ghidra decomp same file). Y-sorts 3 (X,Y) grid-cell corners, walks scanlines with
      16.16 fixed-point edge stepping, gathers each covered cell's u16 id (column-major grid at
      table+0xC) into SCENE_STATE.list @+0x10 / count @+6. Faithful cross-product LEFT/RIGHT edge
      pick, X-clamp against grid X-limit, Y-guard vs Y-limit, 254-entry cap. Pure integer math —
      no packet emit, no GTE.
    - ✅ Tomba/list-2 walk `FUN_8007B008` = `ObjectList::walkList2` (game/object/object_list.cpp).
      The trivial list walker on `T2_OBJLIST_HEAD_2` — clear render flag @+1, capture next @+0x24,
      dispatch handler @+0x1c(node) with node in a0. **This is the top-down layer immediately above
      Tomba's per-frame tick** (Tomba's node is one of these list-2 entries). Reuses the shared
      `BehaviorDispatch::dispatchObj` so any native beh_* handlers route directly. Enable
      `debug behhist` to enumerate the per-walk handler addrs (Tomba's included).
    - ✅ parallax BG state machine `FUN_8010BFFC` = `ParallaxBg::step` (own class embedded on
      Engine — game/scene/parallax_bg.{h,cpp}). Pure state SM: init from *(u16*)0x800ECF84,
      per-frame wrapped scroll offsets from yaw/pitch. No GP0.
    - ✅ zone-transition setup `FUN_8001D71C` = `Engine::zoneTransitionSetup` (engine_stage.cpp;
      Ghidra decomp scratch/decomp/sop_tail_8001d71c.c). Tiny record dispatcher; two leaves
      (FUN_8001CF2C engine tick, FUN_8001D2A8 XA/voice entry) stay substrate. Also wired into
      Engine::fieldRun (idx=9) and ScreenFade::sequence (idx=11).
    - ✅ organizational: `Engine::fadeSequencer` → `ScreenFade::sequence` (own the fade SM on the
      fade class, not on Engine). Same body, uses `zoneTransitionSetup(11)` and direct
      `rec_dispatch` for the still-substrate leaves.
    - ☐ DEFERRED (to render rebuild): `FUN_8010C26C` BG tile scroller and `FUN_8010C79C`
      end-of-area text scroller — both emit raw GP0 packets, belong to the PC-native BG renderer
      rewrite ("REBUILD, don't transcribe" in CLAUDE.md), not a mechanical port.
    - ✅ `bgSceneTransitionSm.step` sub-leaves — FUN_80075CEC (audio fade-target), FUN_80026470/
      80026510/800264BC (3 gated audio-fade variants), FUN_80050970 (bf816 dispatcher). All owned
      as file-local static helpers in bg_scene_transition_sm.cpp (Ghidra decomp
      scratch/decomp/bg_scene_subleaves.c). The SM's state bodies now hold ZERO substrate calls
      except the two innermost still-substrate leaves inside bf816_dispatch (FUN_800508A8 /
      FUN_8005082C — their own sub-frontier). Audio-fade helper will hoist to a proper audio
      class the first time a third caller shows up.
    - ✅ SOP intro-cutscene scene actors (3 handlers) — `beh_sop_intro_pilot` (0x8010ACFC,
      script-driven master-G actor; model 0x11), `beh_sop_intro_lifted` (0x8010B798, Y-lifted
      secondary; model 0x0F), `beh_sop_intro_narration` (0x8010B990, narration-void-beat spawner;
      model 0x0F). Each in its own game/ai/beh_sop_intro_*.cpp; registered in
      BehaviorDispatch::kTable. These are the 3 nodes Sop::fieldMode's state-0 spawns onto HEAD_2
      (the ONLY actors Sop::fieldUpdate/walkList2 dispatches during the SOP intro). Ghidra decomp
      scratch/decomp/sop_scene_actors.c. Control flow + node writes native; sub-behavior leaves
      (FUN_800519E0 model attach, FUN_80040CDC/80077C40 anim setup, FUN_8007778C bounds cull,
      FUN_800518FC post-cull, FUN_8004190C anim/graphics, FUN_8003116C spawn) stay rec_dispatched.
      ScriptInterp::step (FUN_80041098) routes directly to the native beh_script_interp_step via
      c->engine.script.step().
    - ✅ **`FUN_8010AE30` + `FUN_8010AB38` — the shared "SOP-overlay" drop-shadow helper pair, OWNED
      native** (game/ai/sop_overlay_shadow.cpp). `FUN_8010AE30` is the one-shot spawn both
      `beh_sop_intro_pilot` and `beh_sop_intro_lifted` fire once their model attach succeeds: it
      calls the already-native `Spawn::dispatch(cls=0, type=6, list=1)` (FUN_8007A980) and wires the
      new node as a per-actor drop-shadow quad — handler ptr node[+0x1C]=FUN_8010AB38, node[+0x10]=
      parent back-pointer, node[+0x18]=0x8002AB5C (the RESIDENT TERRAIN quad-draw address, later-227's
      `terrain_render_pc` — stored as a content-interface DATA value only, never called through).
      `FUN_8010AB38` is the shadow's own per-frame tick (registered `beh_sop_overlay_shadow` in
      BehaviorDispatch::kTable): copies the parent's world position each frame while the shared SOP
      scene-beat byte (0x800BF9B4) stays < 5, and derives two alpha/scale ramps ([0,0x80] / [0,0x100])
      from the parent's jump elevation (parent+0x84) that shrink the shadow the higher the actor
      jumps — a classic PS1 drop-shadow. Ghidra decomp scratch/decomp/cluster1.c (FUN_8010AE30) +
      scratch/decomp/cluster2.c (FUN_8010AB38). CONTROL FLOW + ALL memory ops owned native, byte-exact
      transcription (no sub-behavior calls besides the already-native Spawn::dispatch/despawn).
      VERIFIED: SBS full 0 sbs-div / 0 VIOLATION over 10900+ frames (unaffected — this is a
      pc_skip=true-only kTable shortcut, SBS full always takes the substrate body); live REPL run
      (`newgame; skip 400; run N`) shows `beh_sop_overlay_shadow` (0x8010AB38) firing 2×/frame (one
      per spawned shadow) with zero derail/crash. Closes the `FUN_8010AE30/8010B588` frontier note —
      `0x8010B588` (the "lifted" actor's OWN multi-state sub-tick, a deeper 6-state SM synced to the
      same scene-beat global, calling ScriptInterp::step + 3 substrate anim-install leaves) is a
      SEPARATE, more complex function — RE'd (scratch/decomp/cluster3.c) but left un-owned this pass.
    - ☐ **`FUN_80111CCC` / `FUN_80114B90` — RE'd, NOT ported this pass (deliberately deferred).**
      Both are the "rarer paths" `beh_record_list_scanner` (FUN_8004CE14, already owned) can call
      per-record from its state-1 command loop (see "Rarer paths (0x80111CCC, state 3, G0==6)
      transcribed from disasm, verify when a scene drives them" above). Ghidra decomp:
      scratch/decomp/probe_field_ents.c (both), scratch/decomp/probe2.c (FUN_80023870, a dependency).
      * `FUN_80111CCC(param_1)` — Ghidra decompiles it reading `unaff_s2/s3/s5/s6/s7`: it does NOT
        use a normal a0..a3 ABI, it's a tight-loop continuation that consumes ITS CALLER's live s-regs
        directly (a common PSX-compiler idiom for an inlined per-record command, not a real
        function-call boundary). Porting this byte-exact requires first RE'ing exactly what
        `beh_record_list_scanner`'s (FUN_8004CE14, game/ai/beh_record_list_scanner.cpp) inner loop
        holds in s2/s3/s5/s6/s7 at the call site — a cross-function register-flow analysis, not a
        clean single-function port. Left un-owned rather than guess the register mapping (CLAUDE.md:
        RE first, no black-box).
      * `FUN_80114B90(param_1, param_2)` — clean ABI (`FUN_80023870(param_1, param_2, param_2+0xc0)`;
        if nonzero, clear `DAT_1f800182`), called from `Engine::fieldRunFaithful` gated on
        `area==8`/`0x800bf870==8`/`0x800bf839==7` (game/core/engine.cpp:1162/1182/1260/1359/1370/1419)
        — per pc-game-architecture.md "the area==8 conditional 0x80114b90 leaf … never hit on
        [reachable areas]" (seaside is area 0). Its dependency `FUN_80023870` is a proximity/facing
        hit-test (X/Z distance vs summed radii + Y-band check via `FUN_80084080` isqrt) that on a hit
        computes a heading (`FUN_80085690`) and then TAIL-JUMPS (Ghidra: "Subroutine does not return")
        into `FUN_80083f50`/`FUN_80083e80` — almost certainly a branch into the cutscene SCRIPT-VM
        (sibling of the already-owned `ScriptInterp::step`/`FUN_80041098`), not a normal return. Both
        the tail-jump control transfer and `FUN_80083e80/f50`'s own bodies need RE before this can be
        ported correctly; left un-owned rather than port a guessed call-then-return shape. Neither
        address is reachable in the exercised seaside/SOP-intro path, so this has NO coverage impact
        on the current SBS gate — safe to leave for a dedicated pass once area 8 (or the script-VM's
        tail-jump primitive) is being worked.
    - ✅ mode-state ARM primitives — `Engine::armModeState(a,b,c)` (guest FUN_8005082C) and
      `Engine::armModeStateFromAreaTable()` (guest FUN_800508A8). The 3-byte payload arm/backup
      pattern used by the intro fade sequencer, front-end DEMO prologue, scene-UI trigger's confirm
      branch, pool.finalViewInit's fall-through, bf816_dispatch, and the SOP-side field-run
      state 3/4/5. Widely-called; every native call site (8 total across engine_stage / pool /
      screen_fade / bg_scene_transition_sm / beh_scene_ui_trigger / engine_demo) now routes to the
      native methods. Ghidra decomp scratch/decomp/bf816_leaves.c.
    - ✅ SEASIDE PLACEMENT TABLE — 100% NATIVE. Enumerated the seaside placement table at
      0x801469BC (in the A00 overlay, area=0 index into 0x800A4C28[]): 62 records installing 22
      distinct handlers. Every handler in that table is now native, with `beh_seaside_prox_substate`
      (0x8013C1DC — a proximity-gated 3-way substate router; the last unowned entry) added this
      pass. So the entire top-down chain `Engine::fieldRun state-0 → Placement::placeAreaObjects →
      each placement record's node[+0x1c] handler` is native for seaside. Ghidra decomp
      scratch/decomp/beh_8013c1dc.c.
    - ✅ BEH_SEASIDE_PROX_SUBSTATE FAMILY — the 5 A00-overlay sub-behavior leaves this handler
      drives directly are now ALSO native (2026-07-08): `FUN_8013B70C` (drawInit — event-slot
      proximity/commit check), `FUN_8013B868` (subA — anim/SFX arm sequence), `FUN_8013BAB0`
      (subB — camera-nudge/facing machine, nudges the shared camera-G position 0x800E7EAC/B0/B4),
      `FUN_8013BCC8` (subC — cutscene data blast + collectible-style item spawn), `FUN_8013C0BC`
      (modeArm — single-slot sprite-frame cycler via Asset::uploadImage). All ported 1:1 in
      `game/ai/beh_seaside_prox_substate.cpp` from Ghidra decomp
      (scratch/decomp/beh_seaside_cluster.c, scratch/decomp/beh_seaside_helpers.c) cross-checked
      against a capstone disas of a fresh seaside RAM dump for every branch + address computation,
      including exact guest-stack-frame depth for the two functions that stage a local struct
      (`modeArm`'s and `subC`'s own `-0x30` prologues, folded on top of the top-level handler's own
      `-0x20` — now applied via a `beh_seaside_prox_substate` wrapper). Their own un-owned callees
      (FUN_8006E1C0/E1E4 sound-loop start/stop, FUN_8004766C/80048750 tile-move+anim-link,
      FUN_8013B534 sub-object init, FUN_80027144 GPU packet emit, FUN_8009A450 PRNG) remain
      rec_dispatch leaves — each its own future frontier item, shared by many other still-substrate
      callers. NOTE: this actor is the SINGLE deep-underwater seaside placement (record 61,
      world pos 16606,-4980,11200) — headless autonav does not reach it, so the SBS gate ran
      trivially zero-diff (10800+ frames, no [sbs-div]/VIOLATION); correctness rests on the 1:1
      RE + disas cross-check, not live exercise. `codemap.py` does not index these (free functions
      in an anonymous namespace, matching the sibling beh_area_transition_machine.cpp /
      beh_pickup_collect_trigger.cpp convention for behavior-handler internals — a known gap:
      codemap only tracks Class::method / ov_/native_/eng_-prefixed natives, not BehaviorDispatch
      leaves; even the already-owned top-level `beh_area_transition_machine` at 0x80127798 shows
      "NO native owner found").
    - ✅ THIS PASS — 5 previously-RE'd but unported leaves owned as native:
      * `Pool::clearBf548Region` (FUN_8004FB20) — 700-byte zero-init at 0x800BF548 (per-area
        scene control block). Wired into `Pool::init`.
      * `Pool::initTypedPools` (FUN_800798F8) — the object-subsystem heart: 5 typed free-list
        pools built via +0x24 next-ptr chains (52/58/42/40/5 slots at strides 0x88/0xC4/0xD0/
        0x108/0x140, classes 0-4), all 3 active list heads (T2_OBJLIST_HEAD_1/2 + AUX) zeroed,
        3 aux-render list head/tail pairs seeded at scratchpad 0x1F80013C/148/154. Wired into
        `Pool::init`. Pool 2 (0xD0 stride, class 2, 42 slots) is where the 208-byte NODES come
        from that spawn.dispatch pops. Ghidra decomp scratch/decomp/pool_init_leads.c.
      * `Engine::gStateMutate(G, op)` (FUN_80058304) — 15-case master-G flag-bit mutator with
        associated announcer cue calls. Called from `Engine::fieldRun` state 2 with op=0xC (the
        clear-G-flags body). Every case ported faithfully so the other callers (still-substrate
        code paths) will get the native impl once their parents move over. Ghidra decomp
        scratch/decomp/fieldrun_s2_init.c.
      * `Engine::uploadModeSprites` (FUN_80067DA8) — mode-selected VRAM sprite-pattern upload:
        5 × 16×1 BGR555 strips loaded at fixed X=0x1F0 with per-strip Y offsets, source pointers
        picked from 3 sets at 0x800A4800/40/80..0x800A49C0 by DAT_800BF88D. Substrate leaf
        `FUN_80081218 = LoadImage(RECT*, data)` kept dispatched. Wired into `beh_sop_intro_pilot`
        (was `rec_dispatch(0x80067DA8)`; the guest a0 = master-G was unused by the callee). Ghidra
        decomp scratch/decomp/fun_80067da8.c.
      * `Engine::audioSettleField` (FUN_80074BC4) — the 4-step audio state settle used by
        `Engine::fieldRun` (states 3 / 1's mode-3 branch / 6) + `Engine::fieldRunX`. Clears the
        audio-cue scratchpad flag @0x1F80027E, runs 3 libsnd/BIOS-wrapping leaves that stay
        substrate (FUN_8001CF2C / FUN_80074B44 / FUN_80074E48). 4 native call sites rewired.
        Ghidra decomp scratch/decomp/fun_80074bc4.c + scratch/decomp/74bc4_subs.c.
      * `Engine::audioDispatch3Way(idx, arg2)` (FUN_800750D8) — 3-branch tiny dispatcher used by
        `Pool::selectStateIndex` (native — rewired) and area-machine state 0 (still substrate).
        Ghidra decomp scratch/decomp/fun_800750d8_v2.c.
      * `Engine::audioVoiceFetchBits(bits, flag)` (FUN_8001D364) — bit-packed XA/voice fetch
        selector; sister to `zoneTransitionSetup` (same substrate leaf FUN_8001D2A8, different
        index shape). Reached from `audioDispatch3Way`'s else-branch. Ghidra decomp
        scratch/decomp/fun_8001d364.c.
    - ★ **KEY FINDING (this pass): the master G block at 0x800E7E80 IS Tomba's node.** He is NOT
      dispatched by walkAll / walkList2 / walkAux; instead his state lives on the shared G block
      that pool_init sub-init FUN_8007A810 zeros (0x184 bytes at 0x800E7E80..0x800E8004). Verified
      by RE'ing FUN_80057DC0 = `Engine::playerGrowthStep(G, mode)`: it toggles G+0x17E bit 0x8000
      (grown flag) with a G+0x32 Y-compensation of ±0x46, then rescales G+0xB8/BA/BC (Q12 world
      scale), G+0x80/82/84/86 (bounding box), G+0x62/64/66/68 (physics constants), and DAT_800E802A
      by 1/(mode+1). That's Tomba's growth transformation directly on the G block. Callers pass
      obj = 0x800E7E80. `beh_area_transition_machine` writes G+0x2C/0xB0/0xB4 (master pos XYZ) too,
      consistent with G being Tomba. So walkable-Tomba's "spawn" is actually the pool-init zero of
      the G block; his per-frame update happens via a hardcoded per-area callback (candidate:
      `Engine::modePerFrameDispatch`'s 24-entry table @0x8009D1D4 — seaside entry [0] is the next
      RE target).
    - ✅ THIS PASS — 6 more small leaves owned (per-object utilities shared across many handlers):
      * `Engine::animEnvInit(obj, envArg, animData)` — FUN_80040CDC (anim env seed + bit-decode
        of animData bits 0x1000/0x4000 → obj+0x71).
      * `Engine::animTick(obj)` — FUN_8004190C (anim VM stepper + obj+0x79 byte).
      * `Engine::announcerCue(id, flag)` — FUN_8004ED94 (announcer-cue queue push, table lookup at
        0x800BF7FC/0x800BF800).
      * `Engine::objMatrixCompose(obj)` — FUN_800518FC (post-cull matrix compose with GTE leaves
        kept substrate).
      * `Engine::walkStart(obj, mode, subMode)` — FUN_80054D14 (anim-mode transition with 2
        substrate leaf variants for the subMode split).
      * `Engine::playerGrowthStep(G, mode)` — FUN_80057DC0 (**Tomba growth/shrink**; see key
        finding above).
      Rewired 8 native call sites: 3 beh_sop_intro_* handlers × 2 leaves + gStateMutate's cue
      helper (all 12 cue call sites) + gStateMutate cases 6/7 (playerGrowthStep). Ghidra decomp
      scratch/decomp/batch_leaves.c.
    - ✅ SEASIDE PER-FRAME UPDATE `FUN_80113C5C` = `area_seaside_perframe(c)` — a new file
      `game/ai/area_seaside_perframe.cpp`. Wired into `Engine::modePerFrameDispatch`: area-0
      (seaside) special-cases into the native handler instead of `rec_dispatch`. Ownership shape:
      control flow + mode-byte dispatch + aux-list walk owned native; all 11 sub-behavior leaves
      (Tomba per-frame tick FUN_80022760, mode-2 sub-tick FUN_8011334C, the trio 22554/113700/
      1138E8, per-item leaf 80112A60, post pair 112C0C/112F14, Tomba post-frame 8010E904, pre-tick
      2288C, default-mode post 130C4) stay dispatched — each becomes its own future sub-frontier.
      This is the top of Tomba's per-frame update tree; owning it puts the engine (not the recomp
      body) in charge of iterating Tomba's mode/tick each seaside frame. Ghidra decomp
      scratch/decomp/seaside_perframe_113c5c.c.
    - ✅ MORE `ActorTomba` methods (this pass): `settleStep(mode)` = FUN_80054650 (the two-way grid
      probe called from `velocityIntegrate`'s tail; picks probe base + fires 2 substrate probes,
      stamps G+0x5F/0x60 on hit); `postFrameWaterCheck()` = FUN_8010E904 (water-mode Y-snap +
      area-exit trigger; final call in `area_seaside_perframe`). Also fixed `Engine::animTick`
      (FUN_8004190C) to route through the existing native `Animation::step` (FUN_80076D68)
      instead of `rec_dispatch`, and dropped the missing-a0 bug (the recomp reads a0 = obj that
      the caller left in $a0). Ghidra decomp scratch/decomp/footstep_hunt.c +
      scratch/decomp/tomba_postframe_10e904.c + scratch/decomp/anim_event_75ff8.c.
    - ✅ **ObjectTable::dispatch bug fix + handler port** — `game/world/object_table.{h,cpp}`.
      Discovered a wrong constant: `HANDLER_TABLE = 0x800AD52C` in the port was off by one hex
      digit vs the disas (`addiu s2, v0, -10964` with v0=0x800A0000 → **0x8009D52C**). The bug
      was silent at seaside because every 40-slot pool entry has obj[0]==0 there so the
      handler-ptr load never fires; any cutscene using this pool would have crashed. Also RE'd
      + ported the sole handler installed in the table (`FUN_80027254`, all 4 valid slot indices
      point at it) as `handler_27254(c, obj)` in the same file — a 4-state PARTICLE SM (INIT
      seeds vel/gravity from per-pattern tables at 0x8009D53C/4C/5C/6C/7C using the LCG PRNG;
      RUNNING integrates position + life countdown; DESPAWN pool-returns via FUN_8007B2AC).
      Verified NO SFX. `dispatch()` special-cases the native handler; unknown fns fall through
      to `rec_dispatch`. Ghidra decomp scratch/decomp/objtable_handler_27254.c.
    - ☆ **FOOTSTEP-SFX HUNT — negative results this pass, path narrowed:** every Tomba leaf I've
      RE'd + ported so far has NO SFX trigger. Verified by RE'ing the anim VM (FUN_80076D68 =
      Animation::step and its 3 keyframe sub-leaves FUN_80075F0C/FF8/76904 — all pure 12-bit
      vector unpackers, no calls to FUN_80074590), `settleStep` (grid probes only), and
      `postFrameWaterCheck` (water snap only). Searched every still-substrate leaf in the
      seaside per-frame chain for `jal 0x80074590`: found ONLY (a) FUN_80022554 → SFX 0x2F
      (interaction "bonk" when Tomba touches an interactive aux item), and (b) FUN_8010E408 →
      SFX 0x07 (splash when Tomba walks INTO pond water). Neither is footsteps. The walk-cycle
      footstep must live in one of the still-substrate sub-callees of `area_seaside_perframe`'s
      trio (FUN_80113700 / FUN_801138E8) or the per-aux-item leaf FUN_80112A60 (which fires for
      each aux item Tomba interacts with each frame, potentially including invisible "ground
      contact" triggers). Those are the next Ghidra RE targets.
    - ✅ **class ActorTomba (game/player/actor_tomba.{h,cpp})** — one class owning every native
      Tomba primitive over the G block (0x800E7E80). Embedded on Engine as
      `c->engine.actorTomba`. Consolidated this pass:
      * `interactWalk()` — FUN_80022760 (aux-list walker + 3 private sub-handlers
        proximityCheck / type4GuardedCheck / subHitboxCheck for FUN_80022060/80114E74/80022190).
      * `postInteractWalk()` — FUN_801130C4 (default-mode post-tick — walks the *0x1F80013C
        render/interaction queue, type-keyed dispatch to 8 substrate sub-handlers including a
        detailed case-4 state-transition path that stamps G+0/4/5/6/0x172/0x173/0x2B and issues
        the announcer cue via `c->engine.announcerCue`). Ghidra decomp
        scratch/decomp/fun_801130c4.c.
      * `growthStep(mode)` — FUN_80057DC0 (moved from `Engine::playerGrowthStep`; grow=1, shrink=0
        toggles G+0x17E, ±0x46 Y-comp, rescales bounds/physics by 1/(mode+1)).
      * `velocityIntegrate(suppressY)` — FUN_80056B48 (was static `player_move_56b48` in the
        retired `game/player/engine_player.cpp`).
      Retired files: `game/player/engine_player.cpp` + `game/player/tomba_interact.cpp` (both
      reduced to one-line forwarding comments; source removed from CMake). Callers rewired:
      area_seaside_perframe → `interactWalk()` + `postInteractWalk()`; gStateMutate cases 6/7 →
      `growthStep(mode)`.
    - ☐ WALKABLE TOMBA — his NODE location is now known (= G block); his per-frame update fn:
      * seaside placement table (62 records dumped, no Tomba-shaped handler).
      * pool.init's 8 sub-inits (all zero-inits, not spawns).
      * FUN_801088D8 GAME sub-mode-1 bridge (verified = already native as `Engine::submode1`;
        its case 0 just runs `Sop::transitionAreaLoad` — no per-actor spawn).
      * Engine::fieldRun state 0 init chain (pool/placement/reset/view — nothing spawns Tomba).
      * Engine::fieldRun state 2 (FUN_80058304 case 0xC = "clear G flags", not a spawn).
      * ~~Engine::fieldRun state 3's `d0(0x80074BC4)` — needs RE (candidate).~~ STALE, RESOLVED:
        0x80074BC4 was RE'd and owned earlier this same section (see "`Engine::audioSettleField`
        (FUN_80074BC4)" above) as `AudioDispatch::settleField` — a 4-step AUDIO STATE SETTLE
        (clears the audio-cue scratchpad flag, engine-tick, 2 voice-table cleanups). NOT a spawn.
        This note went stale because the walkable-Tomba write-up below wasn't updated when the
        RE landed — corrected now (later pass, see below).
      * Area-machine `Engine::s4c` state 0 (0x801064C4 disassembled — audio-fade + sm bump +
        FUN_8004D8B0 which is a 128-byte zero-init — NOT a spawn).
      * Area-machine state 3 (0x801065B8) — calls FUN_8007ED5C (save-menu text render) and
        FUN_80078824 (writes AREA START POS for the player at 0x800BF890/894/898 from per-area
        table @PTR_DAT_800A54A8[area] + sub_area*8). This sets Tomba's spawn POSITION but the
        node itself must already exist.
      * ✅ LATER PASS — the `s4c`/area-machine sm[0x4c] sibling frontier (states 1/2/4..8, guest
        FUN_80106478) is now FULLY RE'd + OWNED as `Engine::areaLoadState()` (game/core/engine.cpp,
        wired onto the live `Engine::s48_2_frame`'s sm[0x4a]==2 branch, replacing the rec_dispatch).
        NEGATIVE RESULT: none of its 9 states spawn anything either — it's the PAUSE/SAVE/QUIT-menu
        confirm-dialog sequencer (fade-out timer, SAVE-prompt/CONTINUE-prompt/QUIT-confirm text
        renders gated on pad-edge bits @0x800E7E68, SFX cues via `c->engine.sfx.trigger`). This
        rules out the LAST un-RE'd sibling of the sm[0x4c] area machine as a spawn candidate.
        Ghidra decomp scratch/decomp/game_all_list.c (FUN_80106478) + area_load_leaves.c (its
        sub-leaves — FUN_8007E8DC/ED5C/EE74/EF60/BF20/B3F4/B2C0/750A4, RE'd to classify + rule
        out). Two more tiny leaves owned alongside it: `AudioDispatch::selectState` (FUN_800750A4,
        audio_dispatch.cpp) and `Engine::seedDirectionMasks`/`reloadEntityPool` (FUN_8007B2C0/
        FUN_8007B3F4, startup.cpp — entity-pool control-header reload + facing-direction mask
        swap, used by the QUIT-confirm-accepted branch). COVERAGE CAVEAT: sm[0x4a] never reaches
        2 under `PSXPORT_SBS_AUTONAV=1` (confirmed via a temporary `[stage] s48_2_frame` debug
        print, both with and without `PSXPORT_SBS_KEYS=...:start` scripted presses at several
        frame windows during the SOP intro) — opening the pause menu needs actual controllable
        gameplay, which isn't reached yet. Verified instead by: build/boot clean, 0 SBS diffs
        across the (unaffected sm[0x4a]==0/1) exercised path, and a careful 1:1 transcription of
        the Ghidra decompile with the one uncertain point flagged in-code (a probably-spurious
        `func_0x8001cf2c(0x11)` decompiler artifact — every other of the ~15 call sites to this
        leaf in the codebase is niladic).
      NEXT: with both `s4c`-family address groups (0x801064C4-family AND 0x80106478/state-machine)
      now ruled out as spawns, the walkable-Tomba NODE-ACTIVATION site is still open. Remaining
      candidates: the per-area mode-dispatch table @0x8009D1D4 (`Engine::modePerFrameDispatch`,
      seaside entry [0] already native — check entries [1..23] for other areas), or a write to
      the G block's "active"/"visible" flag word that hasn't been traced yet.

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
  - ✅ later-296 (2026-07-01): **swept for more orphaned-but-verified natives with the same shape** (a
    `docs/code-map.md` ORPHAN entry whose exact guest address is still `rec_dispatch`'d from other native
    files, even though a verified native impl already exists right there) and wired the biggest ones onto
    the live path — same pattern as later-295, direct drop-in since `record_gate`/the `ov_*` entries already
    set `c->r[2]` exactly like `rec_dispatch` would:
    - `ov_obj_render_update` (`FUN_800517F8`, per-object render-state refresh, `graphics_bind.cpp`) — 5 sites
      (`game/world/entity.cpp` ×4, `graphics_bind.cpp`'s own `obj_pos_compose`).
    - `ov_obj_set_geom` (`FUN_80077B38`, geometry-block/model attach, `graphics_bind.cpp`) — 10 sites across
      `beh_jumptable_release_trigger`/`beh_typed_variant_router`/`beh_typed_init_exit_poker`/`beh_typed_anim_spawn`.
    - `ov_obj_record_init` (`FUN_80051B70`, per-object cull-record init, `graphics_bind.cpp`) — 8 sites across
      `beh_cull_tick_render`/`beh_area_transition_machine`/`beh_jumptable_flag_gate`/`beh_sibling_angle_track`/
      `beh_typed_jumptable_pair`/`beh_typed_table_seed_gate`/`beh_typed_anim_spawn`.
    - `ov_rand` (`FUN_8009A450`, the platform LCG PRNG, `game/math/mathlib.cpp`) — 9 sites across
      `game/world/entity.cpp`, `beh_id_compare_motion_dispatch`, `beh_typed_variant_router`'s `prng()`
      helper, `beh_scatter_record_dither` (×6).
    32 call sites total, all mechanical (register-set-then-`rec_dispatch` → the same registers then the
    native fn), verified by rebuilding clean + a headless free-roam smoke run + turning on the LIVE
    diagnostic gates (`rendupdverify`/`recinitverify`/`setgeomverify`/`randverify`) under real gameplay
    traffic: **0 mismatches** (`randverify` alone saw 220000+ live calls).
    `ov_inventory_give_and_flag` (`FUN_8004D4C4`, 8 sites) similarly has no header yet — a follow-up.
  - ✅ (2026-07-08): owned the A00-overlay attack-orbit sub-behaviors reached from
    `beh_id_compare_motion_dispatch` (`FUN_80145230`, already native) when node[3]==0x80/0x81 — the last
    two rec_dispatch-only leaves in that dispatcher's own address band. `FUN_80145AF0` →
    `AttackOrbitSubstate::aimAtTargetAnchor` (aim-point recompute from a captured target + one-shot
    attack-window trigger via `func_0x800331D8`), `FUN_801458E0` → `AttackOrbitSubstate::orbitTargetMotion`
    (6-phase acquire/orbit state machine). New `game/ai/attack_orbit_substate.{h,cpp}`, engine member
    `c->engine.attackOrbit`. RE'd via Ghidra headless (A00 overlay project) + cross-checked against the
    recompiled substrate (`generated/ov_a00_shard_{0,1}.c`). SBS full ran ~150 frames autonav, 0 diffs,
    but autonav didn't confirm it actually drove an object through this specific attack sub-state —
    correctness rests on the RE + zero-regression, flagged per CLAUDE.md.
  - ✅ (2026-07-08): owned the six field-overlay sub-motion leaves `beh_jumptable_release_trigger`
    (already-native `FUN_80124E74`, address band 0x8012xxxx) dispatches into via `rec_dispatch`:
    `FUN_80123E9C` → `ReleaseTriggerMotion::hoverBobCycle` (node[5] 5-state Y-bob timer), `FUN_801241BC` →
    `leaderFollowSync` (snap-to-leader vs free-run off a node[0x10] "anchor" struct + a 0x800BF9CC
    type-bitmask gate), `FUN_801244E8` → `driftReposition(obj, variant)` (2-state re-seed/idle drift,
    variant picks camera-relative vs a DAT_801498B0 per-type table), `FUN_801246B4` → `arcSwoopMotion`
    (4-state swoop ramp off DAT_80109B44), `FUN_801249D4` → `doubleArcMotion` (9-state two-pass X-impulse
    off DAT_80109B50), `FUN_80124C6C` → `circleOrbitMotion` (3-state orbit toward a DAT_80109B7C
    (X,Y,Z) table entry). New `game/ai/release_trigger_motion.{h,cpp}`, engine member
    `c->engine.releaseTriggerMotion`, wired via `EngineOverrides` (not `BehaviorDispatch` — these are
    internal `rec_dispatch` CALL TARGETS reached by the already-native outer dispatcher, not per-object
    outer handlers) in `runtime/recomp/boot.cpp`. RE'd via Ghidra headless on a fresh free-roam RAM dump
    (`scratch/ram/band12.bin`, PSXPORT_AUTO_SKIP=1), cross-checked with `tools/disas.py --ram` for the
    hoverBobCycle jump table (0x80109B18), the shared FUN_80077B38 model pointer immediate (0x8014C808),
    and the 0x800BF9CC bitmask width (byte, not word — the decompiler's `(int)(uint)DAT_800bf9cc`
    notation was easy to misread). SBS full ran ~3150 frames autonav, 0 diffs; autonav's single field
    scene only drove `driftReposition`/`leaderFollowSync` (node[3] types 0/1/4) — `hoverBobCycle`/
    `arcSwoopMotion`/`doubleArcMotion`/`circleOrbitMotion` (types 4-v2/5/6, and the internal
    hoverBobCycle call from leaderFollowSync's non-gated branch) weren't exercised in this window;
    correctness for those four rests on the RE + zero-regression, flagged per CLAUDE.md.
  - ✅ later-297 (2026-07-01): **wired the whole GTE-transform cluster onto the live path** — `ov_mat_mul`
    (`FUN_80084110`), `ov_apply_matlv` (`FUN_80084220`), `ov_rot_x/y/z` (`FUN_80084D10`/`EB0`/`85050`).
    These were `static` in `engine_math.cpp` (not even visible to other TUs), so this is a BIGGER version of
    the later-295/296 pattern: the "115000+/55000+ live calls, 0-diff" verification numbers earlier in this
    doc were measured back when the (now-removed, 2026-06-22) override-flip system hooked these addresses —
    since that removal these GTE natives had been silently orphaned too, running as pure interpreted
    substrate the whole time despite being GTE-exact verified. Exposed all 5 via a new
    `game/math/engine_math.h` and replaced 26 call sites in `game/render/engine_submit.cpp`'s node-transform
    propagation (`xform_propagate_body` + 3 sibling transform-compose functions) with direct calls — this is
    the HOT per-object transform path (every rendered node with children), so the highest-value substrate-
    reduction found this session. The `mathverify`/`rotXverify`-style per-call gates are themselves now dead
    (they were also only reachable via the removed override hook — nothing calls the `_verify` wrappers),
    so verification here was: clean rebuild, headless free-roam smoke run (no regression), and a `shot`
    screenshot of the seaside village scene — Tomba + the tree-hut render correctly, matching
    `docs/reference/ORACLE_village_f520.png` (no corruption, correct object placement/shading). Flag for the
    user to eyeball other scenes too since this touches every 3D object's transform.
  - ✅ later-298 (2026-07-08): **TYPED-CHILD SPAWN cluster owned** — 4 A00-overlay leaves
    (`FUN_801360F4`/`FUN_80139838`/`FUN_8013AC34`/`FUN_8013A730`, band 0x80135000-0x8013AFFF) called via
    `rec_dispatch` from the already-native `beh_box_seed_phase_gate` (STATE 0) and `beh_single_child_cull`
    (STATE 0). Each is byte-identical in shape: allocate via the already-owned `Spawn::dispatch(cls,
    type=4, list=0)`, then on success stamp the fresh child's `[+0x1C]` handler (one of the ALREADY-native
    siblings `beh_quad_record_table_seed`/`beh_sibling_angle_track`/`beh_child_trig_motion`/
    `beh_lift_platform` — confirmed by decoding each function's `32787<<16 + imm` / `32788<<16 + imm`
    constant back to the target address), `[+0x10]` owner back-pointer, `[+2]` content-type byte, and
    (3 of 4) `[+3]` caller sub-index byte. RE'd directly from the recompiler-generated C
    (`generated/ov_a00_shard_{0,1}.c`, `ov_a00_gen_<addr>`) — instruction-exact ground truth, cross-checked
    against the existing `beh_box_seed_phase_gate.cpp`/`beh_single_child_cull.cpp` header comments which
    already documented these as callees. Added `Spawn::spawnTypedChild` (shared body) +
    `spawnQuadRecordChild`/`spawnSiblingAngleChild`/`spawnChildTrigChild`/`spawnLiftPlatformChild`
    (game/world/spawn.{h,cpp}), wired via 4 new `EngineOverrides::register_` entries
    (`Spawn::registerTypedChildOverrides`, called from `boot.cpp`) since the existing callers reach these
    addresses through plain `rec_dispatch`, not `BehaviorDispatch`. SBS full: two runs (4530 + 4320 frames)
    both 0 `sbs-div`/`VIOLATION`. Autonav within the ~85s window did not exercise ANY EngineOverride project-
    wide (verified with `PSXPORT_DEBUG=all` — zero `[dispatch]` lines total, not just for this cluster), so
    hit-coverage for these 4 specifically is unconfirmed this session; correctness rests on the RE +
    zero-regression SBS run, flagged per CLAUDE.md.
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
- ✅ **2026-07-08 (band 0x8004-0x8005FFFF sweep) — 4 more NodeXform siblings owned:
  `NodeXform::seedBlock/propagateRotmat/propagateAxis/buildAxis` (0x800517BC/80051300/80051464/
  80051C8C, game/render/node_xform.cpp).** `PSXPORT_DEBUG=recdep-all` free-roam histogram (3000
  frames) ranked these the busiest still-substrate leaves in the band once the earlier 0x8003xxxx/
  math/animation ownership had drained the top of the list: seedBlock 19971 calls, propagateRotmat
  10998, buildAxis 2672 (propagateAxis is buildAxis's own tail-callee, not separately dispatched).
  RE'd from Ghidra (`tools/decomp.sh`) then cross-checked verbatim against the ground-truth
  recompiled bodies (`generated/shard_1.c` gen_func_80051300, `shard_2.c` gen_func_80051464,
  `shard_5.c` gen_func_800517BC/gen_func_80051C8C):
  - `seedBlock(ptr,x,y,z)` — trivial `{x,0,y,0,z,0,0,0}` 8-word seeder (the same diagonal shape
    build()/buildWithOffset build inline for their scratch source matrix).
  - `propagateRotmat(node)` — sibling of the existing `propagate()` (0x80051128): same per-child
    root/sibling parent-frame compose + world-position accumulate, but seeds the child's rotation
    via a single `Math::rotmat(child+8)` call (not the 2-stage rotmat+matMul-to-scratch40 propagate()
    uses) and writes the child's matrix to child+0x18 / position to child+0x2C (not +0x24/+0x44).
    Reached both via `rec_dispatch` (5 overlay cross-module callers, A00-A0A) and directly by
    `GraphicsBind::renderUpdateBody`'s own recompiled body (`func_80051300(c)` in shard_3/shard_6) —
    the latter REQUIRED the `shard_set_override` dual-wire (EngineOverrides alone would have missed
    it, same gotcha `docs/findings/tooling.md` already flagged for ActorReward/PcScheduler).
  - `propagateAxis(node)` — sibling of propagateRotmat using an EXPLICIT identity + rotX(child+8)/
    rotY(child+0xA)/rotZ(child+0xC) composition instead of one rotmat() call; otherwise identical.
    Only same-module caller found was `buildAxis`'s own body (shard_5.c) — dual-wired anyway
    (defense-in-depth; no other caller currently exists to protect against, but the g_override[]
    slot is process-global so a future substrate addition reaching it directly would still be safe).
  - `buildAxis(node)` — node-level sibling of `build()`: composes THIS node's own world matrix via
    identity+rotX/Y/Z (not rotmat) at node+0x98, copies raw local position straight into the
    world-pos triple (NO rotation, unlike buildWithOffset), tail-calls propagateAxis(node). Every
    caller found was `rec_dispatch(c, 0x80051C8Cu)` from an overlay (A00/A02/A06/A08) or the native
    `beh_anim_trigger_gates.cpp` handler (via the `leaf()` rec_dispatch wrapper) — no same-module
    substrate caller exists, so EngineOverrides alone suffices (no shard_set_override needed).
  All four wired in `NodeXform::registerOverrides()`, called from `register_engine_overrides()`
  (runtime/recomp/boot.cpp) so SBS's separately-constructed Games get them too. v0 note:
  propagateRotmat/propagateAxis structurally always leave v0==0 at return (the loop-guard idiom
  guarantees it on every path) — `GraphicsBind::renderUpdateBody` propagates this v0 as its own
  return value, so the EngineOverrides trampolines set `c->r[2] = 0` explicitly rather than leaving
  it as an accidental leftover. VERIFIED: `PSXPORT_SBS_MODE=full` autonav headless, zero sbs-div/
  VIOLATION through f7500 (90s wall-clock, process still running when killed); `PSXPORT_DEBUG=ovhit`
  confirms all four registrations actually fire in a free-roam session (seedBlock 19971,
  propagateRotmat 10998, buildAxis 2672 — propagateAxis 0 external hits is EXPECTED, it's reached
  only as buildAxis's internal native call, never through the override table); a follow-up
  `recdep-all` histogram shows all 4 addresses gone from the substrate-dispatch list entirely
  (substrate no longer executes for them at all).
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
- ✅ **later-XXX — `FUN_80076904`/`FUN_80077B5C`/`FUN_80077C40` (Animation::loadFrame/advanceLinkChain/
  attach, game/object/animation.{h,cpp}).** RE'd by hand via `tools/disas.py` with careful delay-slot
  tracing (Ghidra headless's decompile pass mis-resolved this cluster — verified wrong against the
  already-committed `Cull::decide()` — so its output was discarded in favor of the manual MIPS walk).
  - `loadFrame` (FUN_80076904) — POSE-TABLE FRAME LOADER: resolves the table entry `rec` at
    `tableBase[idx]` (idx = cursor[0], tableBase = obj+0x3C), stamps the flags byte to obj+8, and — when
    flags&0x40 is set — unconditionally unpacks 6 packed-nibble 12-bit fields (9 bytes) per limb into an
    array of struct* at obj+192 (offsets 8/10/12 plain, 0x38/0x3a/0x3c then <<3); when flags&0x40 is
    clear, unpacks 3 fields (4.5 bytes, parity-alternating nibble-sharing) into offsets 8/10/12 only.
    Either arm may ALSO unpack a signed-12-bit XYZ triple into obj+0x88/0x8a/0x8c when flags&0x80 is set
    (same 5-byte code, shared as `anim_unpack_pose_triple`). Loop bound = obj[8]&0x3f (top-checked) AND
    obj[9] (bottom-checked, also the initial "any data" guard) — same dual-bound idiom in both loops.
  - `advanceLinkChain` (FUN_80077B5C) — ticks obj+0xE (countdown); while running returns 0 (no-op). On
    expiry, walks ONE node of a distinct 4-byte-stride tag chain rooted at the SAME cursor field obj+0x38
    (tag halfword @cur+2, jump pointer @cur+4 — NOT the anim-VM's 8-byte/+6/+8 shape): tag 0/0x4000
    advance-or-follow + reload payload, return 0; tag 0x8000 hold (no cursor move), return 1; tag 0xc000
    follow + reload, return 1. Reused as a generic "tick + advance one event chain" leaf by ~10 beh_
    handlers unrelated to skeletal animation (same obj+0xE/+0x38 field convention, different format).
  - `attach` (FUN_80077C40) — installs table[id] (array of struct*, stride 4) onto a node: cursor =
    entry, countdown = entry's descriptor low-12 (entry+6), calls `loadFrame`, then — if the descriptor's
    0x2000 bit is set — dispatches the SAME frame executor (FUN_80075ff8, kept `rec_dispatch`) the anim-VM
    uses, with the same tag-keyed (follow-pointer vs address) argument split.
  - Wired via `EngineOverrides` (game/object/animation.cpp `registerOverrides()`, called from
    `boot.cpp`) at all 3 guest addresses — every existing `rec_dispatch`/`leaf1`/`call3` call site across
    the beh_ handlers reaches the native method uniformly with zero call-site edits. `step()`'s own
    internal calls to FUN_80076904 (3 sites in `anim_vm_76d68`) call `loadFrame` directly (native→native).
  - **SBS full 0-diff over ~7860+ frames of autonav gameplay** (loadFrame fires every frame via the
    anim-VM — Tomba's own walk-cycle — so this is a high-frequency proof); `advanceLinkChain`/`attach`
    did not fire within the autonav-reachable field area in this session (gated to specific NPC/trigger
    object types) — structurally verified by RE + SBS-gated (any future call site that reaches them will
    fail fast on divergence, not silently mismatch).
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
  - ⚠️ **RE'd/DRAFTED/UNWIRED (2026-07-08, 0x8006 band RE-ahead pass):** 5 residual leaves added as
    `CutsceneCamera` methods — `resetFollowAccum` (FUN_8006E8F8), `pushMode`/`restoreMode` (FUN_8006E1C0/
    E1E4), `snapToMasterOffsetY200` (FUN_8006EA00), `orbitTick` (FUN_8006EF38). Compiles; NOT registered in
    EngineOverrides/shard_set_override, NOT run through SBS, callers not yet identified for pushMode/
    restoreMode/orbitTick. See docs/engine_re.md "CutsceneCamera — 5 residual leaves drafted".
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
- ✅ later-298 (2026-07-08): **"variant overlay" spawn/tick cluster** — the un-owned leaves
  `beh_pad_child_linker.cpp` / `beh_typed_jumptable_pair.cpp` / `beh_cube_text_spawn.cpp` were calling
  via `rec_dispatch`, RE'd with Ghidra + disas cross-check and owned:
  - `Spawn::spawnOverlayVariant(recordIndex, variant)` (`FUN_8007E038`, game/world/spawn.cpp) — SAME
    `FUN_8007A5A8` class-3 allocator `sceneEntity` (`FUN_8007E110`) already uses, installing the sibling
    per-frame handler `beh_variant_overlay_lifecycle` (`FUN_8007DC38`) instead of the scene-entity one.
    Guard: `(DAT_800BF81E==2) || (variant!=0) || (DAT_800BF822==0)`, else miss (returns 0).
  - `Spawn::tickLinkedOverlay(obj, recordId)` (`FUN_800735F4`) — per-object controller that owns exactly
    one `spawnOverlayVariant` child at `obj[0x14]`, on a state byte `obj[7]` + countdown `obj[0x40]`;
    calls `spawnOverlayVariant` directly (no other substrate calls in the body).
  - `Bit::setFE48(idx)` (`FUN_8006F00C`, game/math/mathlib.cpp) — sibling of the existing `setFE34`, same
    u32 flag word `0x800BFE48` `testFE48` already polls.
  - `Bit::processLinkRequest()` (`FUN_8006F04C`) — the child-link REQUEST-mailbox arbiter (byte
    `0x800BF840`): ids 0/1/6 are retry-limited via a 3-try counter at `0x800BFE3A[id]`, ids 7/8 grant
    immediately (raising `setFE34` first), ids 2..5/9..15 are silent misses; a grant calls `setFE48`.
  - `Font::measureLineWidth(c, strAddr)` (`FUN_80073750`, game/ui/font.cpp) — pure string measurer (used
    by the "cube letters" text actor). ABI GOTCHA preserved: the guest body's loop cursor IS a0, so on
    return a0 == the NUL terminator's address — a register LEFTOVER `beh_cube_text_spawn.cpp`'s following
    `GraphicsBind::recordAlloc()` call relies on implicitly (documented in its own "GOTCHA" comment); the
    native version reproduces this by writing `c->r[4]` before returning.
  - **LEFT substrate:** `FUN_8007BE18` — the resident LOAD-GAME menu UI driver (already documented at s4
    0x80106580 above): a thin content-routing dispatcher into un-RE'd overlay UI screens
    (`FUN_8018fa88`/`FUN_8018fbcc`) + SFX triggers (`FUN_80074590`), not game logic, already correctly
    reached via `rec_dispatch`.
  - **VERIFY:** `PSXPORT_SBS_MODE=full` autonav run, A/B identical through frame 8220, zero `[sbs-div]`,
    zero VIOLATION. **EXERCISE COVERAGE CAVEAT:** none of the 5 call sites fired during that run (confirmed
    via temporary call-counters on `beh_pad_child_linker`/`beh_typed_jumptable_pair`/`beh_cube_text_spawn`
    themselves — all 3 owning behavior files were never invoked at all by the shallow autonav walk, which
    only reaches basic free-roam past the caught pose). The ports are byte-exact BY CONSTRUCTION (1:1
    disas transcription, cross-checked instruction-by-instruction against sibling patterns already proven
    live — e.g. the identical `FUN_8007A5A8` allocator shape in `sceneEntityBody`) but are NOT yet exercised
    by any headless run; a scene/area that spawns a `pad_child_linker`-handled object or a `typed_jumptable_
    pair` overlay object is needed to actually drive `spawnOverlayVariant`/`tickLinkedOverlay`/
    `processLinkRequest` and settle the SBS gate against real traffic. Same caveat class as
    `beh_cube_text_spawn`'s own pre-existing "~x142/field-frame on seaside" note.

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
**SESSION 2026-07-08 (wide-RE-ahead-of-frontier, isolated worktree) — 0x80040000-0x8004FFFF band:
cube-text popup ledger RE'd + DRAFTED (UNWIRED, not gated); collision-walk cluster RE'd + MAPPED
(NOT drafted — too risky to fold blind); ~180 addresses in-band still untouched.**
- RE'd/DRAFTED (game/object/cube_text_ledger.{h,cpp}, added to cmake, compiles+links clean):
  `FUN_80040A58` (cost-table lookup), `FUN_80040AA4` (popup spawn), `FUN_80040B48` (ledger
  activate), `FUN_80040C00` (ledger deactivate) — the two leaves `actor_sm_reward.cpp` already
  calls through `rec_dispatch` under "not independently RE'd" (`FN_40B48`/`FN_40C00`), plus their
  siblings. Full derivation + layout in `docs/engine_re.md` ("Region 0x80040000-0x8004FFFF sweep").
  **UNWIRED**: no `shard_set_override`/`EngineOverrides` registration, SBS gate NOT run. A follow-
  up wave wires (dual-registration, matching `actor_sm_reward.cpp`'s pattern) + gates it.
- RE'd/MAPPED, NOT drafted: the collision-walk / room-cell-graph cluster (`FUN_8004720C` +14
  siblings) — dense scratchpad-resident (0x1F8001A8-0x1F8001EC) fixed-point slope/edge-code
  traversal with `trap()` div-by-zero paths. Deliberately left unported pending a cross-check
  against whatever already-owned terrain code touches 0x1F8001Cx (CLAUDE.md's later-158 warning).
  Decomps banked: `scratch/decomp/region4004x.c`, `scratch/decomp/region4004x_b.c`.
- Untouched: `FUN_80040558` (large per-object SM, same shape as `actor_sm_24448.cpp`) + its
  callees `FUN_8004022C`/`FUN_80040390`/`FUN_80040410` (RE'd via Ghidra, decomp banked, not
  drafted — entangled with un-RE'd sub-dispatchers). `FUN_8004005C`/`FUN_80040400` resisted a
  clean Ghidra function-boundary decompile — needs a disas.py boundary spot-check first.

**SESSION 2026-07-08 (WIDE-RE, isolated worktree) — A00-overlay band 0x80108000-0x80125000 census +
one drafted leaf. RE-AHEAD-OF-FRONTIER mode: UNWIRED, UNVERIFIED — do not build on this without an
independent cross-check.**
- Full census of the assigned band: 260 top-level `ov_a00_gen_<addr>` functions, 21 already owned
  by prior sessions, **239 still substrate**. Sorted-by-size list banked at
  `scratch/logs/inband_sizes_unowned.txt` (worktree-local, not committed) for whoever picks this up
  next — see `docs/engine_re.md` "A00 OVERLAY BAND 0x80108000-0x80125000" for the full writeup and
  the top-40 list inline.
- `0x80112188` RE'd + drafted as `ActorMeleeEngage::doIt` (`game/ai/actor_melee_engage.{h,cpp}`) —
  an AI melee-engage proximity/angle/arm-state gate. Compiles (added to
  `cmake/tomba2_port.cmake`); **not wired** (no EngineOverrides/shard_set_override entry, no call
  site) and **not verified** — a dense hand-transcribed MIPS DAG without a decompiler pass; treat
  as a draft needing independent RE confirmation before use. Full field-offset map + callee list in
  the header; honesty caveat about the transcription risk is in the .cpp banner.
- `0x8010CF90` (497 lines, the single largest unowned function in-band) scoped only — a 16-way
  jump-table NPC schedule/area-event dispatcher keyed on day-cycle globals. Too large to safely
  hand-transcribe in one session's remaining window; flagged for a follow-up with an actual
  Ghidra decompile pass (this session's Ghidra import of the RAM dump did not finish in time).
**SESSION 2026-07-08 — 0x8006xxxx-0x8007xxxx band sweep: 5 new leaves owned + wired (AreaSlots
grid-cell tracker, MusicCoord second-stage gain setter, Animation frame-applier, Math isqrt16/
approxDist3 wiring); 6 Cull camera-wrapper variants ported but left UNWIRED (guest-stack risk).**
- `PSXPORT_DEBUG=recdep-all` histogram (autoskip free-roam, ~1500 frames) ranked the busiest still-
  substrate leaves in the assigned 0x80060000-0x8007FFFF band. RE'd via Ghidra headless
  (`tools/decomp.sh` on a live free-roam RAM dump, `scratch/decomp/cluster1.c`), cross-checked
  against `generated/shard_*.c`.
- ✅ **`AreaSlots::primeCountdown(idx)`** (FUN_80074A38) + **`AreaSlots::updateCell(sigArg,dx,dy)`**
  (FUN_8007496C) — game/world/area_slots.{h,cpp}. Sibling leaves of the already-owned `ackIfMatch`
  over the same 24×12 slot table at 0x800BE238. Wired via EngineOverrides only (no static
  `func_<addr>(c)` call site exists for either — dynamic-dispatch-only). ovhit: 6317 / 7980 hits.
- ✅ **`MusicCoord::setGain2(val)`** (FUN_80075D24) — game/audio/music_coord.{h,cpp}. Sets the
  ambient-voice second-stage gain (0x800BE1F8+0x2E) that `voiceMixTick`'s smoother chases; negative
  `val` is an instant snap (writes both target+current). EngineOverrides only. ovhit: 1379 hits.
- ✅ **`Animation::applyFrame(node,snapCursor)`** (FUN_80075F0C) — game/object/animation.{h,cpp}.
  The per-frame keyframe applier `Animation::step`'s header comment already named as staying
  substrate; now native, dual-wired (EngineOverrides + shard_set_override — several direct
  `func_80075F0C(c)` call sites in generated/shard_{4,6,7}.c). `anim_vm_76d68`'s own DELAY branch
  now calls it directly instead of `rec_dispatch`. Safe to shard-wire: verified via
  `generated/shard_4.c` that its guest body never touches `r[29]` (no stack frame to mismatch).
- ✅ **`Math::isqrt16`/`Math::approxDist3`` WIRING** (0x80077FB0/0x80078240) — game/math/gte_math.cpp.
  `eng_isqrt16` was already fully RE'd/correct (cull.cpp's own distance calc uses it) but had NEVER
  been registered under EngineOverrides/shard_set_override — a pure dead-code leaf despite being
  the #1 profiled hot function historically (docs §D). Added `eng_approxDist3` (new RE, FUN_80078240
  — sort-3-abs-values + weighted-sum fast magnitude estimator) alongside it. Both dual-wired.
  ovhit: 2858 / 5442 hits.
- ✅ **RESOLVED (2026-07-08, later session) — all 6 Cull camera-relative-wrapper variants WIRED,
  byte-exact, 0-diff** — game/render/cull.{h,cpp}. The "wiring broke SBS, reverted" note above is
  superseded: the divergence was a genuinely missing piece, not a reason to leave it unwired.
  Fixed in two layers: (1) `Cull::wrapFrame(raConst)` mirrors each wrapper's own real 24-byte frame
  (`addiu sp,-24; sw ra,16(sp)`, RE'd instruction-exact per-site from
  generated/shard_{0,1,2,4,5,7}.c) — the piece the earlier attempt was missing; (2) a SECOND,
  nested gap found only once (1) was fixed: FUN_8007712C (the cull body every wrapper calls) ALSO
  pushes its own real 40-byte frame that the existing native `performBaseCull()` never mirrored —
  added `performBaseCullFramed()` for that inner frame, used only from `wrapFrame()`. Additionally,
  `cullWrapperFlag2`/`cullWrap77acc` already had EXISTING native C++ callers
  (beh_id_compare_motion_dispatch.cpp; beh_record_list_scanner.cpp, script_vm.cpp) — framing the
  shared method broke those; split into unframed public methods (native-facing, unchanged) +
  `*Framed()` twins (guest-ABI-facing, used only by the `shard_set_override` trampolines). Full RE
  + bisection trail in docs/findings/animation.md.
- Gate: `PSXPORT_SBS_MODE=full` autonav, 95s / 8790+ frames, **zero `[sbs-div]`, zero VIOLATION**,
  with all 6 Cull wrappers wired. Verified firing via a temporary hit-counter (removed after
  confirmation): 8007778C≈160k, 80077ACC≈67k, 800777FC≈8k, 800779D0≈8k, 80077A4C≈5.6k,
  800778E4≈2.7k hits in a 30s sample.
- ⚠️ **`Animation::step` (FUN_80076D68) wiring investigated, reverted (2026-07-08)** — the guest
  frame mirror (`Animation::stepFramed()`) IS correct (RE'd + verified byte-exact in isolation),
  but wiring it exposes an UNRELATED, pre-existing fidelity gap in `anim_vm_76d68` (not written
  this session) for node address `0x800E7E80` (pool-adjacent — sits right after the active-object
  free-counter at 0x800E7E7C), called from one site (ra=0x8005AA90). Root-caused via per-call A/B
  trace: every OTHER traced call matched exactly; excluding just this one address made the
  divergence disappear entirely. Not fixed here (would need special-casing one address — a banned
  magic-constant bandaid, not a fix); reverted to unwired. `stepFramed()` kept, documented, for
  whoever RE's that call site next. Full trail in docs/findings/animation.md.
**SESSION 2026-07-08 (WIDE-RE, unwired) — ground/scene GT3/GT4 sibling pair RE'd + DRAFTED, NOT
wired/verified: `FUN_8013FB88`/`FUN_8013FE58`/`FUN_801401B8` → `class OverlayGroundGt3Gt4`
(game/render/overlay_ground_gt3gt4.{h,cpp}).**
- ☐ **RE'd + drafted, UNWIRED, UNVERIFIED (no SBS run performed this session — mandate was RE
  ahead of the frontier, bank drafts, don't gate).** The sibling of the field-object pair directly
  below (this file's own next entry) — ground/scene entities' copy of the same GT3/GT4-into-
  packet-pool-and-OT emitter, reached via `0x8003D0BC → 0x801401B8 (entity loop) →
  0x8013FE58/0x8013FB88`. RE'd off `generated/ov_a00_shard_{0,1}.c` (fresh recompile off the real
  disc, this session). Full region-map + per-fn RE trace: `docs/engine_re.md` "A00-overlay region
  map" section. Compiles clean into `scratch/bin/tomba2_port` (verified this session — full CMake
  build, `-target tomba2_port`); `registerOverrides()` exists but is never called anywhere.
  **Next session: wire via the overlay's own `ov_a00_set_override` table (NOT EngineOverrides,
  same as OverlayGt3Gt4 below), gate SBS-full 0-diff, confirm firing via a hit-counter before
  trusting a 0-diff.**
- Also swept the whole A00-overlay upper-half region (0x80125000–0x8014E944, this session's
  assigned band): 334 function addresses total, 44 already owned before this session, 287 STILL
  fully substrate after landing the 3 addresses above — mapped (not RE'd) by adjacency/size into
  three rough clusters for next-session triage in `docs/engine_re.md`. Two addresses that looked
  like new targets in a naive scan turned out NOT to be: `0x801469BC` is DATA (the seaside
  placement table, already 100% native per its own entry above) and `0x80147FC4` is a duplicate
  tail-shared copy of `0x80146478`'s call sequence (redundant with the field GT3/GT4 pair below).

**SESSION 2026-07-08 — A00-overlay GT3/GT4 render-packet emitter (0x801465EC/0x801467BC, the
busiest still-substrate `rec_dispatch` leaf in free-roam): both owned as `class OverlayGt3Gt4`
(game/render/overlay_gt3gt4.{h,cpp}).**
- ✅ **DONE — both leaf addresses.** `PSXPORT_DEBUG=recdep` histogram flagged `0x80146478` (the
  thin 2-instruction FUN that splits its record-count header and calls the two leaves below) as
  ~75 calls/frame, 2.7x the runner-up. RE'd via Ghidra headless on a live seaside-field RAM dump
  (`scratch/decomp/render146.c`, cross-checked against the recompiler's own register-accurate
  translation `generated/ov_a00_shard_{0,1}.c` — the recompiler output is the more precise source
  for GTE register indices/opcodes, which Ghidra's COP2 decompilation garbles).
  - `FUN_801465EC` = **POLY_GT3** (gouraud-textured triangle) emit: GTE RTPT (perspective
    transform) + NCLIP (backface/MAC0 cull) + AVSZ3 (or a custom near-plane-clamped Z blend when
    the record's flag byte is set) to pick the OT bucket, then bump-allocates a 40-byte packet
    into the shared packet pool (`0x800BF544`) and links it into the OT (`ot_base + idx*4`).
  - `FUN_801467BC` = **POLY_GT4** (gouraud-textured quad) emit: same shape, plus a 4th-vertex RTPS
    (RTPT only handles 3 points) and a 52-byte packet (AVSZ4 / widened near-clamp blend).
  - **Framed as a FAITHFUL SUBSTRATE MIRROR, not pc_render**: this is the render-UNDERNEATH path
    (writes packet-pool/OT guest memory on BOTH SBS cores), distinct from `game/render/submit.cpp`'s
    `Render::submitPolyGt3/Gt4Native` (the MAIN engine's GT3/GT4 path, deliberately GTE-free /
    no-guest-write for pc_render). Transcribed 1:1 including a real asymmetry the recomp body has:
    the GT3 leaf's rgb0|code word is written UNMASKED while the GT4 leaf's rgb0 word IS masked
    (`0xFFF0F0F0`) — verified against both decompilations, not "fixed" to match.
  - **Wiring**: neither leaf is reached via `rec_dispatch` — both are called by a direct C function
    pointer generated inside the ov_a00 overlay shard (from `FUN_80146478` itself AND from a
    duplicate tail-shared copy of the same call sequence the recompiler folded into a second giant
    function, `FUN_80147FC4`). Overriding via the overlay's own `ov_a00_set_override` (not
    `EngineOverrides`) covers every call site uniformly — the same process-global-table discipline
    as `ActorReward::registerOverrides`.
  - Gate: `PSXPORT_SBS_MODE=full` autonav, 5340 frames, zero `[sbs-div]`, zero VIOLATION. Confirmed
    firing (not dormant, unlike the zoned-attacker session below): `PSXPORT_DEBUG=ovgt` shows the
    GT3 leaf alone firing ~250 calls/frame during a 350-frame free-roam probe.
  - **Missing-hut-creature bug (separate task, not chased here):** owning this leaf did not, by
    itself, make the seaside-hut decorative NPC appear on the default `pc_render` path — the
    bug is downstream of this leaf being reached at all (an object/geomblk-selection or
    visibility-flag issue upstream), not a packet-emission correctness issue this SBS-clean gate
    would have caught. Left for a follow-up debug session now that the leaf is a real, inspectable
    native function instead of an opaque substrate call.
**SESSION 2026-07-08 — GTE matrix cluster (0x80084110/80084220/80084470/80085480/80084D10/80084EB0/
80085050) FINISHED WIRING — `class Math` (game/math/gte_math.{h,cpp}) was fully RE'd + bit-exact but
DEAD CODE until now.**
- ✅ **DONE — completed the wiring for all 7 addresses.** `codemap.py --addr 0x80084110` reported
  "NO native owner found" despite `Math::matMul` sitting right there in gte_math.cpp, fully RE'd
  (44-bit GTE accumulator, MVMVA-exact) with a header comment calling it "16.2% of hot interpreter
  time... the biggest single perf lever" — because a prior session ported the class but never
  called `register_`/`shard_set_override` for it. `Math::registerOverrides()` was missing entirely;
  `boot.cpp`'s `main()` never called it, so every substrate call site (`func_80084110(c)` etc.,
  inline in shard_0/2/3/5/6/7.c + every overlay — 55k+/frame for matMul alone) fell through to the
  interpreted `gen_func_*` GTE-op body. Only the handful of DIRECT `c->math.*` call sites
  (node_xform.cpp/cutscene_camera.cpp/graphics_bind.cpp) ever actually executed the native class.
  Added `Math::registerOverrides()` (gte_math.cpp): guest-ABI trampolines wired into BOTH
  `EngineOverrides::register_` (native callers reaching via `rec_dispatch`) AND
  `shard_set_override` (the recompiler's own `g_override[]` table — the mechanism the substrate's
  DIRECT `func_<addr>(c)` calls actually consult; same dual-wiring shape as `ActorReward`, docs/
  findings/tooling.md "EngineOverrides::register_ is BLIND to a direct substrate call"). The
  `shard_set_override` trampolines are `psx_fallback`-gated (fall back to `gen_func_*`) since
  `g_override[]` is a single PROCESS-GLOBAL table shared by every Core, including SBS's A/B cores.
  Wired the call from `boot.cpp` alongside `Animation::registerOverrides()`. Also fixed the
  `codemap.py` false-negative: the `// FUN_xxxx` header comments sat above file-local helper
  functions (`load_mat3`/`clamp16s`/`sign44`), not directly above the `Math::` method defs, so
  codemap's "comment block immediately above the def" heuristic found nothing; added the standard
  trailing `// FUN_xxxx` def-line tag (same convention as `Camera::lookAt`) to all 4 methods that
  lacked one (matMul/applyMatlv/applyMatrixLV/rotmat — rotX/Y/Z already had it). All 7 now show
  `[LIVE]` in `docs/code-map.md`.
- **Verification:** confirmed the `shard_set_override` path actually fires under real execution
  (temporary instrumented print in `gov_matMul`, reverted before commit — `psx_fallback=0` hits
  observed on core A, i.e. the native `Math::matMul` genuinely runs, not a silent no-op). Gate:
  `PSXPORT_SBS_MODE=full` autonav headless, 3 separate runs across the wiring iterations (~7000,
  ~7650, and a final confirmation run after the codemap tag fix), **zero `[sbs-div]`, zero
  VIOLATION** each time — this is a REAL verification (unlike the zoned-attacker/reward clusters'
  "0 hits observed" caveat below): the dispatch trace confirmed actual execution on the
  non-fallback core, not just an untested 0-diff.
- **Housekeeping (this session, same as the zoned-attacker session below):** this worktree needed
  `vendor/beetle-psx/deps/libchdr` + `generated/` copied in from the main checkout, and the main
  checkout's UNCOMMITTED `spu.c` `SPU_PokeRAM` edit re-applied — same gap flagged there, not fixed
  here (shared submodule state, out of scope for a worktree).
- **WORKFLOW FINDING (flagging, not fixed — out of this session's band-scoped task):** `dc_boot_init`
  (the shared per-Core boot entry used by SBS/dualcore/selftest) does NOT call ANY subsystem's
  `registerOverrides()` — those calls only happen in `boot.cpp`'s `main()`, on the PRIMARY throwaway
  `Game` object, before it diverts into `Sbs::run()`/`DualCore`/selftest (which construct their OWN
  `Game`s via `dc_boot_init`, not `main()`'s sequence). Net effect: `shard_set_override`
  (`g_override[]`) still works under SBS because it's a PROCESS-GLOBAL table populated once by the
  primary `game` object before the diversion — but `EngineOverrides::register_` (per-`Game`) does
  NOT: SBS's `mA`/`mB` each get their OWN empty `engine_overrides` table, so EVERY existing
  `EngineOverrides`-registered subsystem (`pcSched`, `Animation`, `ActorReward`,
  `ActorZonedAttacker`, `Spawn::registerTypedChildOverrides`, `releaseTriggerMotion`, and now
  `Math`) is UNREACHABLE via `rec_dispatch` under SBS specifically. This matches — and likely
  explains — the "0 dispatch hits observed" caveats on the zoned-attacker and reward clusters below:
  it's not that autonav never reaches those actors, it's that `rec_dispatch`'s `EngineOverrides::run`
  can never fire on SBS's cores at all. A real fix would call every subsystem's
  `registerOverrides()` from `dc_boot_init` (or a shared init helper `main()` and SBS both call) —
  not done here since it touches every prior cluster's wiring call, not just this session's band.
**SESSION 2026-07-08 — per-object cmd-list dispatch chain 0x8003CDD8/0x8003F698 owned as
`Render::cmdListDispatch`/`Render::perModeDispatch` (game/render/perobj_dispatch.cpp, band 0x8003xxxx).**
- ✅ **DONE — both addresses.** These are the SUBSTRATE (guest-writing) leaves `0x8003CCA4`
  (submit_perobj_render, cases 0/4 — the only ones seaside objects hit) calls unconditionally as its
  entire body: for each active render command on an object's persistent list, compose the WORLD
  transform (camera-rot × object-local, via the SAME `gte_op`/`gte_write_ctrl` MVMVA calls the
  recompiled body makes — `0x4A49E012` per rotation column, `0x4A486012` for the translate, cross-
  checked against submit.cpp's pre-existing "later-133" RE comment, an independent source) into GTE
  CR0-7, then dispatch to the per-mode renderer (mode-select byte @0x800BF870 + 22-entry jump table
  @0x80015268 → `rec_dispatch` the resolved target — table entries are addresses of F698's OWN
  internal case labels, not final FUN_ targets; resolved via a literal 11-entry map, confirmed by
  reading all 22 live table words out of a free-roam RAM dump) or the generic GT3/GT4 packet emitter
  `func_800803DC` fallback.
- **NOT the `0x8003CCA4`/`0x8003C2D4`/`0x8003C464`/`0x8003C8F4` family**: those 4 addresses already
  carry a transparent `RenderObserver` wrapper (`render_observer.cpp`, g_override slots 845/840/841/
  844) that runs the literal gen body then tags host-side billboard depth
  (`obj_world_ord`/`gpu_obj_depth_add`/`fps60_bb_node`, the issue-#4 occlusion fix). Owning those too
  would mean reproducing CCA4's 5 special-effect sub-cases (`FUN_8003F4C4/F3F4/D584/F594/F344`, none
  of which fire at seaside) AND folding the observer's host bookkeeping into the replacement to avoid
  regressing that landed fix — out of scope for this focused 2-address slice. CDD8/F698 are NOT
  currently wrapped by anything, so this is a clean, additive, non-conflicting cut of the same chain.
- **MECHANISM NOTE (workflow-relevant): these are NOT reached via `rec_dispatch`.** `PSXPORT_DISPWATCH`
  confirmed `0x8003CCA4` fires (1375 hits/400 frames) but `0x8003CDD8`/`0x8003F698` show ZERO
  `rec_dispatch` hits in the same window — the generated CCA4 body calls `func_8003CDD8(c)` /
  `func_8003F698(c)` as a PLAIN intra-shard C call (see `generated/shard_5.c gen_func_8003CCA4`),
  bypassing `rec_dispatch` (and therefore `EngineOverrides`, which only intercepts inside
  `rec_dispatch`) entirely. The only interception point for this call shape is the `g_override[]` slot
  each `func_XXXX` wrapper in `shard_disp.c` already checks — `shard_set_override()`, the same
  mechanism `render_observer.cpp` uses. `PSXPORT_DEBUG=recdep` (which hooks `rec_dispatch`) is
  therefore BLIND to this whole call shape too — it under-counts any function reached only via direct
  intra-shard calls. Any future "what's hot and unowned" search in this band should cross-check with
  `PSXPORT_DISPWATCH=<addr>` before concluding an address is cold.
- **Gate:** `PSXPORT_SBS_MODE=full` + `PSXPORT_SBS_AUTONAV=1`, headless, zero `sbs-div`/`VIOLATION`
  through frame 8760 (two separate runs, 85s and ~200s wall-clock, both clean; the run only stopped on
  an external timeout/interrupt, not a crash or divergence).
- **Housekeeping (same gap a sibling session already flagged 2026-07-08):** this worktree needed
  `vendor/beetle-psx/deps/libchdr` submodule init + `generated/`/`scratch/bin/tomba2/MAIN.EXE` copied
  from the main checkout, and the main checkout's uncommitted `spu.c` `SPU_PokeRAM` edit re-applied
  locally to build at all — not fixed here (shared submodule state, out of this session's band).

**SESSION 2026-07-08 (follow-up) — the remaining 4: `0x8003CCA4`/`0x8003C2D4`/`0x8003C464`/`0x8003C8F4`
owned as `Render::perObjRenderDispatch`/`billboardCompose1`/`billboardCompose2`/`billboardEmit`
(game/render/perobj_billboard.cpp).**
- ✅ **RE + port done, all 4 addresses.** CCA4 = the case-0/13&0xB dispatcher (mirrors CDD8's own
  6-case switch, calling the already-owned `cmdListDispatch` plus 5 still-substrate special-effect
  leaves with a `(pre,post)` packet-pool-pointer bracket). C2D4/C464 = billboard "local Z-rotation ×
  persistent camera MATRIX" composers (C464 seeds its base operand via still-substrate
  `FUN_800517BC`); both hand off to C8F4. C8F4 = the particle-quad emitter: resolves the node's active
  sub-list, RTPT/RTPS-projects 4 corners via still-substrate `FUN_8003B220`, culls off-screen,
  quantizes depth into an OT bucket, fills color/UV via still-substrate `FUN_8003B054` + a 33-entry
  case table, and emits a 10-word packet into the OT/packet-pool. RenderObserver's depth-tag wrap
  (PktSpanSession + `obj_world_ord`/`gpu_obj_depth_add`/`fps60_bb_node`) is folded directly into each
  of the 4 methods (`withDepthTag`); `render_observer.cpp` no longer installs wrappers for these 4
  addresses (still wraps the 2 remaining unowned siblings C5F8/C788 + 80039F4C).
- **Two real bugs found + fixed during verification** (both would have been silent SBS-gate failures):
  (1) omitting a real guest-stack-frame allocation (`r29 -= size`) in C2D4/C464/CCA4 shifted C8F4's OWN
  frame-relative addresses (`func_8003B220`'s corner-vector output) by the callers' frame size — fixed
  with a `GuestFrame` RAII (real `r29 -= N` / `+= N`, matching each function's real recomp frame size:
  CCA4=32, C2D4=40, C464=32, C8F4=96). (2) C8F4's 33-entry color/sprite-index case table collapsed the
  switch's `default:` (an unrecognized table entry — the recomp does `rec_dispatch(caseTarget); return`,
  a FULL early return) into the same no-op as the explicit `CBC8` case (a genuine no-op that falls
  through to packet emission) — fixed to distinguish them, though this path is believed unreached by
  live data (33 valid indices, only 6 distinct case labels observed).
**SESSION 2026-07-08 (follow-up 2) — root-caused + fixed the f118 residual; deleted `isDeadStackScratch`.**
- ✅ **Real root cause of the f118 class: `NodeXform`'s 6 methods (`game/render/node_xform.cpp`,
  addresses 0x80051844/800518FC/80051128/800517BC/80051300/80051464/80051C8C) never mirrored their
  real recomp guest-stack frames** (landed in `d0eb6f9`, BEFORE `37594c8` mandated frame mirroring) —
  `build`/`buildWithOffset` (32 B), `propagate` (56 B), `propagateRotmat` (40 B), `propagateAxis`
  (48 B), `buildAxis` (32 B) all descend a real `r29` frame and spill live `r16../ra` in the RE'd
  recomp body (verified against `generated/shard_*.c` prologues) but the native port touched `r29`
  nowhere. Evidence: the f118 last-writer trace showed core A's write to 0x801FE8E8 coming from a
  totally unrelated function (`GraphicsBind::installSceneRecord`/other substrate leaves) while core
  B's came from `NodeXform::propagateAxis`'s own spill — i.e. on the native side this stack address
  belonged to a DIFFERENT logical frame than on the recomp side, because propagateAxis (and siblings)
  never staked out their own frame there. Fixed with the same LIVE-spill/restore `GuestFrame`-style
  RAII pattern as `Cull::performBaseCullFramed`/`Render::perObjRenderDispatch` (6 named frame structs
  in node_xform.cpp, one per RE'd layout).
- **Effect (first pass):** the frame-descent alone dropped the divergence from a hard "wrong function
  entirely" collision to a near-total convergence, but left exactly 2 `sbs-div` hits at f117/f157
  (0x801FE8E4..EF). Root-caused those to completion (below).
- ✅ **TRUE 0-DIFF (register-faithfulness, the second half of the fix).** Frame descent mirrors the
  SP trajectory but not the REGISTER state. Last-writer trace of the f117 residual (extended to dump
  sp — both cores at sp=0x801FE8D8, identical) plus a per-write UPPROBE dump named it precisely: the
  recomp bodies load `node` into CALLEE-SAVED registers (`gen_func_80051C8C buildAxis: r16=node,
  r17=node+152`) then TAIL-CALL a nested NodeXform fn (`propagateAxis`) whose prologue spills the
  caller's live `r16..r22` into its own frame — so the spilled bytes at 0x801FE8E8 ARE `node`
  (0x800FD010) on the recomp side, but the native C++ body uses local variables and never updates
  `c->r[16]`, so it spilled a stale 0x1000. Fix: each OUTER NodeXform fn that makes a nested
  NodeXform call now sets its callee-saved node/scratch registers to the RE'd recomp values after the
  frame descent (`build`/`buildWithOffset`: r16=SCR_M/r17=node/r18=SCR_R; `buildAxis`: r16=node/
  r17=node+152) — the nested callee then spills the correct bytes; the frame RAII still restores the
  caller's own incoming values on exit. The Math leaves (rotmat/matMul/rotX/…) are frameless and
  touch only r2..r15/r24/r25, so they never spill a callee-saved register — the ONLY nested spills
  that matter are the NodeXform->NodeXform tail calls, which bounds the fix. **No mask, no exclusion.**
- ✅ **`Animation::attach`'s OWN residual (the separate `0x801FE908..0x801FE914` exclusion) is FULLY
  CLOSED.** The 2026-07-08 revert of attach's frame mirror was based on the assumption that r29 at
  attach's entry differs between SBS cores (no canonical call site to mirror against, since every
  reacher is a native `rec_dispatch`/`leaf3`/`call3` convenience call). A direct probe
  (`PSXPORT_DEBUG=animstack`, `runtime/recomp/overlay_router.cpp`, comparing `c->r[29]` at every
  reach of 0x80077C40 on both cores over a full autonav run) DISPROVES this: r29 is IDENTICAL between
  A and B at every single call — `rec_dispatch` is a native C++ call on both cores and never itself
  touches a guest frame, so the CALLER's r29 passes through unchanged on both sides alike. Mirrored
  attach's real 32-byte frame (`game/object/animation.cpp`) with the LIVE-spill/restore pattern;
  **`isDeadStackScratch` is DELETED from `runtime/recomp/sbs.cpp`** — SBS full-mode stays clean at
  this address across 8000+ frames post-fix (verified; see docs/findings/animation.md).
- **Gate:** `PSXPORT_SBS_MODE=full` + `PSXPORT_SBS_AUTONAV=1`, headless, `isDeadStackScratch` deleted:
  **`grep -cE 'sbs-div|VIOLATION'` = 0** through f7440 (run stopped by external timeout, not
  crash/divergence). TRUE zero-diff, no masks. `PSXPORT_DEBUG=ovhit`-style hit confirmation for the 4
  billboard leaves + NodeXform's 2 previously-uninstrumented siblings done via gdb dprintf probe
  (24390/11106/3728/14834/6920/1854 hits respectively over a ~30s run) since `ovhit`'s atexit dump
  doesn't survive this harness's SIGTERM-based `_exit()` shutdown path (a pre-existing tooling gap).
- **Diagnostic left in place:** the `Sbs::Impl::LastW` last-writer map now also records `sp` (r29 at
  the store) — the enhancement that made this root-cause possible (proving both cores hit the divergent
  slot at the SAME sp, isolating the bug to register state rather than sp trajectory).

**SESSION 2026-07-08 — "zoned attacker" sub-behavior cluster (0x8014047C/80140544/801409C0/80143A00/
80144928/80144B50): all 6 owned as `class ActorZonedAttacker` (game/ai/actor_zoned_attacker.{h,cpp}).**
- ✅ **DONE — all 6 addresses.** These are SUB-BEHAVIOR callees of the already-native FUN_80145230
  (`game/ai/beh_id_compare_motion_dispatch.cpp`, guest 0x8014xxxx OVERLAY area), reached exclusively via
  `rec_dispatch(c, addr)` from that caller (never a direct substrate jal), so wiring is
  `EngineOverrides`-only (no `shard_set_override` dual-registration needed, unlike `ActorReward`).
  RE'd via Ghidra headless (`tools/decomp.sh decomp A00 ... list <addrs>`,
  `scratch/decomp/cluster1.c`) — mechanical, faithful transcription (control flow + node/global
  read-writes owned; every further callee, ~25 addresses like FUN_80141AC4/80142A94/etc., stays an
  un-RE'd PSX leaf reached uniformly via `rec_dispatch`, same discipline as `actor_sm_reward.cpp`).
  gateCheck/typeInit/pickAttackByRange are small self-contained predicates/inits; defaultSubStateMachine
  (~250 lines) and idleTick (~230 lines) are large per-type attack/idle state machines transcribed
  1:1 from the Ghidra C (goto/label structure preserved to avoid introducing logic drift).
  Gate: `PSXPORT_SBS_MODE=full` autonav, ~11000 frames, zero `[sbs-div]`, zero VIOLATION.
  **LOW COVERAGE CAVEAT:** `PSXPORT_DISPWATCH` on the outer caller (0x80145230) confirms this actor
  IS present and ticking (~88k hits/90s, state=1 "active" the whole time, node[3] cycling 2/4/5/128)
  — but `PSXPORT_DISPWATCH` on any of the 6 owned addresses themselves (0x8014047C, 0x80144928) shows
  **zero hits** even over a 110s/~10000-frame run: the object's node[0x2b] tick-gate counter never
  hits zero in this window, so the caller never falls through to the node[3] dispatch that would
  invoke these bodies. Correctness rests on the 1:1 Ghidra transcription + the zero-regression gate,
  NOT on an observed override hit — flagging per the verification caveat (same shape as the
  0x801244E8 session below).
- **Housekeeping note:** this worktree needed `vendor/beetle-psx/deps/libchdr` (nested submodule chain
  broken — `deps/lightning/gnulib` has no `.gitmodules` mapping) and `generated/` (recompiler output,
  gitignored) copied in from the main checkout to build at all; the main checkout's own
  `vendor/beetle-psx` submodule also has an UNCOMMITTED `spu.c` edit (adds `SPU_PokeRAM`) that every
  fresh worktree/clone will need until it's committed to the beetle-psx fork — flagging as a workflow
  gap, not fixed here (out of this session's scope to touch the shared submodule state).

**SESSION 2026-07-08 — entity-behavior cluster (0x801244E8/0x8012866C/0x8012E168/0x8013DD48): 1 owned, 3
correctly excluded (RE-first caught 3 non-game-logic/unsafe addresses before porting them).**
- ✅ **DONE — 0x801244E8 owned as `release_position_801244e8`** (game/ai/beh_jumptable_release_trigger.cpp,
  static fn alongside the file's existing `beh_jumptable_release_trigger`). Release-trigger POSITION/
  RESPAWN sub-behavior: state 0 = one-shot placement (camera-relative offset or table `0x801498B0` keyed
  by obj[0x60]); state 1 = per-frame respawn-adjacent-item roll (`Spawn::spawnAndInit`) + reposition +
  re-arm. Self-contained function (own prologue/epilogue, no inherited register state) — RE'd 1:1 from
  disas 0x801244E8..0x801246B0, cross-checked against Ghidra's decompile. Added `Engine::identityMatrixAt`
  (game/core/engine.h/game/scene/startup.cpp) to expose the previously file-local `eng_identity_matrix` so
  this new port (and any future caller) can reach that already-owned leaf without rec_dispatch. Gate:
  `PSXPORT_SBS_MODE=full` autonav, 2700+ frames, zero `[sbs-div]`, zero VIOLATION — but this behavior is an
  overlay-resident "release trigger" object that autonav's default field roam likely never reaches, so
  this is CLEAN-BUT-LOW-COVERAGE (flagging per the verification caveat); correctness rests on the 1:1
  disas cross-check, not the gate alone.
- ⛔ **0x8012866C / 0x8012E168 — BLOCKED, NOT ported.** Both are FALL-THROUGH CONTINUATIONS inside a larger
  enclosing function (no own prologue; Ghidra's isolated decompile flags `unaff_s0`/`in_v0`/`unaff_s1`),
  reachable via an external `jal` from `entity.cpp`'s `sm40558` (FUN_80040558) as well as by internal
  fallthrough with DIFFERENT register semantics. 0x8012866C's two external call sites ARE fully resolved
  (a0=obj, mode=v0=obj[1]) but porting it in isolation would assume its enclosing function's stack-frame
  convention without RE'ing that function. 0x8012E168 additionally reads `s1` with no local `lw s1,...` —
  provenance untraced (traced up through `FUN_80040558` and `ObjectTable::dispatchFaithful`, which
  repurposes s1 as a loop counter, without resolving it). Left as `rec_dispatch` (unchanged). See
  docs/findings/scene.md "Un-owned entity-behavior cluster" for the full trace.
- ⛔ **0x8013DD48 — LEAVE PSX, not game logic.** RE (Ghidra on `ram_derail2.bin`) shows a GTE cull/midpoint
  + RTPS-transform leaf — the same family as the already-excluded sibling 0x8013DD34 (later-283, "GTE
  compose banned"), 0x14 bytes away. Per the render directive, this is a hardware leaf to leave PSX, not
  an ownable behavior fn despite living in the 0x8012xxxx-0x8013xxxx "behavior region". See
  docs/findings/render.md.
- **lesson for future sessions:** address-range heuristics are not RE. `unaff_sN`/`in_vN` in an isolated
  Ghidra decompile is a real signal of a shared/mid-function fragment — verify against the caller's own
  decompile + the raw bytes immediately before the target address before porting.

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
- ✅ **DONE (later-290) — 0x8006ec44 wired to native.** The camera driver at 0x8006EC44 was ALREADY owned as
  `CutsceneCamera::update()` (game/camera/cutscene_camera.cpp; oracle-unit-tested end-to-end in
  cutscene_camera_test.cpp) but ORPHANED — the `d0(c, 0x8006ec44u)` calls in `ov_field_frame` and
  `ov_field_frame_x` still went through the substrate. Added `extern "C" void cam_update(Core* c)` shim
  (thin wrapper over `CutsceneCamera(c, CAM_OBJ).update()`) and replaced both sites. Smoke: newgame + skip
  500 + run 200 = 727 frames clean, no derail. ov_field_frame is now 7 native/9 direct children.
- ✅ **DONE (later-291) — 0x8001CAC0 wired native as `Engine::areaModeDispatch`.** 22-way area-mode
  jump table on the byte at 0x800BF870 (extracted verbatim from MAIN.EXE .text @0x80010000: 12
  overlay handlers + 10 no-op defaults). Skips the tiny resident stubs 0x8001CBxx and rec_dispatches
  the overlay handler directly (identical behavior; the stub is `jal <h>; j <default>`). Smoke: 2000
  frames clean. ov_field_frame is now 8 native / 9 direct children.
- ✅ **DONE (later-292) — 0x80050DE4 wired native as `Engine::sceneStateStep`.** The SCENE-INIT /
  SCENE-RUN state machine (phase byte @0x800F2418; two 22-entry overlay handler tables extracted
  verbatim from MAIN.EXE .text @0x80015A40 init / @0x80015A98 run, 21 overlay leaves + 1 default
  each, indexed by the same 0x800BF870 render-mode byte). Phase 0 = call INIT handler then set
  phase=1; phase 1 = per-frame RUN handler; phase <0 or ≥2 = no-op. Handlers get a0 = 0x800F2418.
  Smoke: 2000 frames clean.
- ✅ **DONE (later-293) — the last 3 substrate leaves in ov_field_frame owned native.**
  `Engine::modePerFrameDispatch` (0x80022A80): 24-way mode-keyed dispatcher, table @0x8009D1D4,
  mode 3 skipped, no bounds check (faithful).
  `Engine::postRenderTick` (0x80077D8C): 3-state fx-trigger + countdown on byte 0x800BF842
  (fx-trigger leaf FUN_80074590 stays substrate).
  `Engine::frameStartTick` (0x80059D28): per-frame prologue — counter + heading mask + flag
  reset + mode-keyed handler (2/3/7/20 or default 0x8005950C) + scratchpad master-pos seed
  + LFSR rand advance.
  **ov_field_frame per-frame gameplay block now has ZERO substrate dispatches** — every direct
  child is a native method or a native free function. The remaining `d0(...)` in the file are
  either (a) leaf callees of native methods (each overlay handler's leaves, mode-keyed FX
  trigger, per-frame area update 0x80075A80), or (b) inside the tail (0x80075A80 = ~156-line
  per-frame area update state machine — separate port task).
- ✅ **DONE (later-294) — first library subsystem class-ified: `class Rng` (game/math/rng.h/.cpp)
  for the PSX libc rand LCG at 0x8009A450.** Seed at guest 0x80105EE8 (hard ABI; shared with
  still-substrate callers so RNG streams don't diverge). Embedded as `Core::rng`, wired the same
  way as `ScreenFade` / `Engine`. `Engine::frameStartTick`'s per-frame rand tick now calls
  `c->rng.next()` directly instead of rec_dispatch — one less substrate hop per field frame.
- ✅ **DONE (later-296) — `Engine::areaUpdateTail` (guest FUN_80075A80, 156 instructions).** The
  LAST substrate leaf in ov_field_frame's tail is now native — ov_field_frame is 9/9 native direct
  children. Slot-table state machine over 24×12-byte entries at 0x800BE238, keyed by counter at
  0x800BED78; 3 arms (kind=0 skip / kind=0xFF action-leaf FUN_80092660 / other decrement-until-slot[8]==4
  latches a 24-bit mask at 0x800BE358) + a buf post-check that zeroes slot[1]; then mask-drain
  FUN_80098F90(0), common tail FUN_80075824+FUN_80099490, and a key2 branch (0x800BED80 s16)
  that probes an 8-byte table at 0x800BE368 via FUN_8008E0C0 and finally calls FUN_80074BF8 or
  FUN_80074E48. All 8 callees stay substrate. sp -= 88 to mirror the guest's stack frame (the
  buf-fill leaf and the action leaf's 4 stacked args need it). Wired every native caller (5):
  ov_field_frame / ov_field_frame_x (engine_stage.cpp), ov_game_s4c (the coro-redirect s4c handler),
  sop.cpp per-frame area update, and 3 sites in engine_demo.cpp (attract render + demo_tail_rend +
  demo_tail_cf2c). GATE: A/B RAM+scratchpad diff (500 frames free-roam) native vs substrate =
  **0 words differ** in main RAM AND 0 in scratchpad. Camera oracle 75k runs 0 mismatches; headless
  smoke 2027 frames clean.
- ✅ **DONE (later-295) — `Trig::ratan2` (guest FUN_80085690, ~94 instructions).** libgte atan2 owned
  native: sign-strip → first-octant reduction with an overflow-guard split (`(y<<10)/x` if `y` fits
  in low 21 bits, else `y/(x>>10)`) → 1025-entry int16 table at 0x800AA490 → quadrant fixup
  (`2048-v` for x_neg, negate for y_neg). Wired every native call site:
  cutscene_camera.cpp (4 — yawDistAccumulate yaw, ang, pitch, headBuild yaw),
  beh_child_trig_motion.cpp (2 — the trig motion pair), beh_area_transition_machine.cpp (1 — the
  cutscene camera-delta angle). Camera oracle (75000 runs, 25 methods) 0 mismatches; headless
  smoke 2027 frames clean.
- ☐ **NEXT — advance either axis:** (a) port 0x80075A80 (the last substrate leaf in ov_field_frame's
  tail, 156 lines — per-frame area update state machine, iterates a 24-entry 10-byte-per-slot table
  based at 0x800BE238 keyed by counter at 0x800BED78); or (b) descend into each overlay handler's
  callees (the recdep-hot ones — matrix 0x80051794, libgpu 0x80082xxx/0x80083xxx — each a small
  class in the same pattern as `Rng`, so multiple substrate hops fold at once). — they descend into per-area A00 object behaviors (the 0x8013xxxx
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
  **SUPERSEDED** — SBS `navStep`'s AWAIT_CONTROL phase (commit 303894b) already solved "how Tomba becomes
  controllable": tap Cross to release `fieldRun` sm[0x4e]==9 (the scripted caught-pose HOLD) while gate
  0x800BF89C==2, wait for sm[0x4e] to settle at 1 (30-frame sustain). Verified: autonav+postdrive reaches
  control at f246 and stays A/B identical through 20000+ frames of real walk/jump.

- ☐ **pc_skip=ON candidate — narration-end -> fisherman-cutscene multi-step LOAD.** This whole hand-off
  (SOP narration RESET → field-area machine states 0/1/2 → `sm[0x4e]` scripted-hold entry 9→10→11→…→1) is
  exactly the shape CLAUDE.md's per-fork rule describes: a collapsed multi-step init that currently only has
  the FAITHFUL leg (still substrate end-to-end under both `pc_skip` values — none of `Engine::submode0` /
  `Sop::fieldMode` / `Engine::fieldRun`'s `sm[0x4a]==1` handler is wired as an override, so `PSXPORT_GATE=1`
  and the default port run the identical un-owned guest chain here). A `pc_skip=true` SHORTCUT fork would
  seed the field-area machine straight to its post-load steady state (skip the CD-load ticks + the scripted
  fishing-line hold) instead of stepping through it frame-by-frame — matching the "load_in_one_step vs
  load_in_multi_step_faithfully" pattern already used for other collapsed inits (e.g. `Sop::areaLoad` vs
  `areaLoadFaithful`). NOT implemented this session (out of scope for the render-ownership bug fixed here —
  docs/findings/render.md "Narration-end -> fisherman-cutscene LOADING SCREEN"); flagging the fork point for
  a follow-up. The render-side garbage bug is now fixed regardless of which exec fork runs (it's a pure
  render-read staleness issue, not a logic-ownership one).

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
4. ✅ **DONE — the reward/tally-window actor SM cluster owned native**
   (`ActorReward`, game/object/actor_sm_reward.{h,cpp}): `FUN_80049A60`/`FUN_80049E54`/`FUN_8004A3D4`/
   `FUN_8004B150`/`FUN_8004B208` — 5 contiguous per-object SM steps (scroll/fade sub-SM, a numeric tally-
   counter tick, a large item/announcer EVENT dispatcher, and two blink-gate inits). Sole caller is the
   still-substrate `FUN_8004AAC4` actor "process" dispatcher (out of scope; not one of these 5). Wired via
   BOTH `shard_set_override` (redirects the substrate's DIRECT `func_<addr>(c)` call — the mechanism that
   actually matters here, since EngineOverrides alone is never consulted by a substrate-originated call)
   and `EngineOverrides::register_` (for native-code callers reaching these via `rec_dispatch` + tracing).
   Every `ov_*` trampoline gates on `c->game->psx_fallback` so SBS core B still runs the exact recompiled
   body — required because `g_override[]` is a single PROCESS-GLOBAL table shared by every Core, unlike
   EngineOverrides which is per-Game. Gate: `PSXPORT_SBS_MODE=full` autonav, 8000+ frames, 0 `sbs-div`,
   0 VIOLATION — **but 0 dispatch hits observed** (verified via a temporary stderr counter, since removed):
   autonav never reaches whatever NPC/scene owns this actor type, so the 0-diff is a "doesn't break
   anything else" result, NOT an empirical confirm of the port's correctness. Flagged as limited coverage;
   re-verify with the counter re-added if/when a scene reaching this actor is identified.

5. **RE'd, drafted, UNWIRED — ready to wire when frontier arrives (2026-07-08, band
   0x80020000-0x8002FFFF).** `ActorTomba::stepModeInteract`/`type8Interact`/`type7Interact`/
   `growthYSnap` (game/player/actor_tomba.{h,cpp}) — faithful native drafts of guest
   `FUN_80020364`/`FUN_800205CC`/`FUN_800235A0`/`FUN_80022C78`, the 4 leaf handlers
   `ActorTomba::postInteractWalk` already documents by name (its `LEAF_TYPE_0F_14_56` /
   `LEAF_TYPE_8` / `LEAF_TYPE_7` / (postFrameWaterCheck's) `LEAF_WATER_SPLASH` rec_dispatch
   call sites). All 4 mirror their guest-stack frames (40/32/32/0-byte, s0..s3+ra spills) per
   generated/shard_*.c ground truth (Ghidra's decompile of the first was subtly wrong on 2 points
   — a 32-bit heading word misread as 16-bit, and a cascading-result-value ladder — fixed against
   the raw recompiled C). UNWIRED: postInteractWalk's own rec_dispatch(c, LEAF_TYPE_*) call sites
   were deliberately left untouched — these are dead private methods until an EngineOverrides/
   shard_set_override wiring pass + SBS-full 0-diff gate lands them. Region census: 184 `func_
   8002xxxx` total, 13 already owned pre-pass, 171 unowned remain (only this coherent 4-function
   cluster drafted this pass — the rest is un-RE'd frontier, not yet triaged for value). Compiles
   clean in an isolated build2.

6. **RE'd, drafted, UNWIRED — ready to wire when frontier arrives (2026-07-08, band
   0x80070000-0x8007FFFF).** `ActorReward::update`/`resolvePosition`/`approachTargetX`
   (game/object/actor_sm_reward.{h,cpp}, EXTENDING the already-owned #4 cluster above) — faithful
   native drafts of guest `FUN_80070018`/`FUN_800702C0`/`FUN_80070650`: the reward/score-gem
   actor's top-level per-frame dispatcher + its two position-solve helpers, which directly call
   the already-owned `smTallyTick`/`smEventDispatch` and (via `rec_dispatch`) the already-owned
   `Spawn::dropScoreGem`/`GraphicsBind::renderUpdateBody`/`Animation::advanceLinkChain`. Guest
   frames MIRRORED (0x20/0x28/0-byte descents, s-reg + ra spills) per generated/shard_0.c,1.c,2.c
   ground truth; `resolvePosition`'s case-3 preserves a round-toward-zero shift
   (`(v-(v>>31))>>13`) that differs from the shared tail's plain `>>12` — a real distinction the
   Ghidra decompile's casts obscure, caught by cross-checking the raw recompiled arithmetic.
   UNWIRED: not in `registerOverrides()`, no SBS run. Region census: 171 `func_8007xxxx` unowned
   coming in (79 pre-owned incl. Animation/Cull/Trig/Spawn clusters); 142 Ghidra-decompiled to
   `scratch/decomp/region_8007.c`, only this 3-function cluster drafted. Two large clusters
   SURVEYED-not-drafted for a future pass: a dialog/text-box byte-stream renderer
   (0x8007C0D0-0x8007D5xx) and a pause/quit-menu widget builder (0x8007EAE4-0x8007FDB0, confirmed
   via literal "Options"/"Load data"/"Quit game" string pointers). Full survey in `docs/
   engine_re.md`'s "Wide-RE survey: 0x80070000-0x8007FFFF" section. Compiles clean in build2.

   **UPDATE (2026-07-08, wide-RE follow-up, worktree agent-a53f252288693983d):** the dialog/text-box
   cluster's cursor-advance core is now DRAFTED (UNWIRED/UNVERIFIED, compiles) —
   `DialogTextStream::advanceByte`/`applyRenderMode` (FUN_8007C0D0/FUN_8007D0D0) in
   `game/ui/dialog_text_stream.{h,cpp}`. Found and documented a recompiler limitation along the way
   (an 0xF8/0xF9 "dispatch" that's actually a local jump table masquerading as a real indirect call —
   docs/findings/ui.md). The box's own state machine (`FUN_8007D594`), position/layout
   (`FUN_8007D208`/`FUN_8007D14C`), and glyph-blit walker (`FUN_8007C940` + its callees) remain
   mapped-not-drafted. The pause/quit-menu cluster was surveyed further (family extended to the
   Save-confirm + Options sub-page builders) but still NOT drafted — its 3 shared layout helpers
   (`FUN_80079374`/`FUN_800793C4`/`FUN_8007E998`) need their own RE pass first. Full detail in
   docs/findings/ui.md.

# OPEN / BLOCKED (not on the critical path)
- **Band 0x800A0000-0x800BFFFF has no ownable dispatch-target code — flagged for reassignment (2026-07-08).**
  Assigned as an "audio/SFX/sequencer tables + mid-engine leaves" ownership band; RE'd via static census
  (`generated/rec_decls.h`: only 7 declared funcs in-range, all recompiler garbage/misdecoded-data with
  zero callers), live `recdep`/`recdep-all` (zero hits across walk/attack/100k+ frame sessions), and Ghidra
  headless auto-analysis on a free-roam RAM dump (0 functions discovered in the whole range). The band is
  the rodata/data tail of the loaded EXE image (`text 0xAE800` @ `0x80010000` → end `0x800BE800`); its
  content (voice-mixer struct, fade/pause state bytes, libcd dir-cache) is DATA already read by code that
  lives — and is owned — in OTHER bands. See `docs/findings/audio.md` "Ownership sweep of
  0x800A0000-0x800BFFFF…" for the full evidence. No code ported from this assignment.
- **ov_game_s4c verification** needs an in-game AREA transition (sm[0x4a]==2). Free-roam IS reachable headless
  (AUTO_SKIP=500 + AUTO_WALK) but the seaside exit isn't a plain walk/jump (hut door blocked by a barrel) —
  needs visual steering or RE of the exit trigger in GAME.BIN handler `0x801088d8`. (NB: there is NO pause menu
  — the reliable `PSXPORT_DEBUG=state` task-slot probe confirms 0 menu tasks; an earlier "pause menu" claim was
  a broken probe.) Controls (which button = jump) are not yet identified — leave it; it reveals itself in play.
- **Band 0x80030000-0x8003BFFF RE survey (fleet agent, 2026-07-08, RE-ahead-of-frontier — UNWIRED).**
  122-function Ghidra decompile + shard cross-check (docs/engine_re.md "RE survey" section has the full
  breakdown). NOT a single subsystem: (1) 5 particle-burst object state machines (`0x800300D8` family) —
  NAMED, not drafted (needs `0x80084520`/`0x80084250`/`0x80051794` RE'd first); (2) thin particle-spawn
  wrappers + list-tail helpers (`0x800310F4`-`0x80031780`) — named, not drafted; (3) the "compose object
  transform into scratchpad CR0-8" family, 9 functions (`0x800317CC`, `0x800318A0`-`0x8003265C`,
  `0x80032AB4`/`CBC`) — this IS the band's "GTE projection path" mandate; architecturally understood (it's
  the guest-side origin of `game/render/projection.cpp`'s `projComposeCore` formula) but NOT drafted —
  4 callee leaves (`80084520`/`80084250`/`80084A80`/`80051794`) need their own RE pass first, no in-band
  caller to cross-check a guess against; (4) item/status-menu 2D UI draw calls (`0x800328BC`-`0x800368D0`)
  — out of scope for this band, flagged for a UI-band pass; (5) `0x80036DFC`-`0x8003BF00` mostly already
  owned. DRAFTED (compile-only, unwired, unverified): `FUN_8003B054` (quad-corner rotate/swizzle) +
  `FUN_8003B320` (the RTPT+RTPS+OT "per-quad submitter" `submit.cpp` already references by address) in
  new `game/render/quad_rtpt_submit.{h,cpp}` — mirrors the already-owned `OverlayGt3Gt4::gt3/gt4` idiom.
  Also noted: `tools/codemap.py --addr 0x8003B220` reports "NO native owner" despite `game/player/
  hitbox.cpp` owning it — a codemap detection gap, not fixed this session (flagged for the operator).
- **Band 0x80010000-0x8001FFFF RE survey (fleet agent, 2026-07-08, RE-ahead-of-frontier — UNWIRED).**
  Ghidra headless full-analysis found only 33 real function starts in this 64 KB band, vs 519
  recompiler `func_8001xxxx` addresses — the gap is the finding. `docs/engine_re.md` "WIDE-RE survey:
  0x80010000-0x8001FFFF" has the full breakdown. NOT portable game logic for most of the band:
  (1) 0x80010000-0x800109FF — kernel exception/trap chain (`rec_break` + `cop0_mfc`), BIOS, leave
  substrate; (2) 0x80017930-0x8001CAC0 (~19 KB) — confirmed a BIOS jump table + embedded debug
  strings MISDECODED AS CODE by the recompiler's linear scan (hundreds of 2-instruction "trampoline"
  functions spaced 4 bytes apart; one callee decompiled to raw ASCII string bytes, not instructions)
  — NOT individually meaningful, do not RE/port any `func_8001793x..8001CAxx` address as game logic.
  (3) 0x8001CB00-0x80020000 — REAL game/AI code, two clusters: an init+FSM cluster
  (0x8001CB00-0x8001DD90, SCOPED not RE'd to completion — pad-calibration-shaped slot reset +
  cooperative wait-loop chain + an area-id-keyed 22-way dispatcher, all flagged for follow-up) and a
  9-function melee-proximity/cone-arbitration family (0x8001EC3C-0x8001FF7C) using the SAME actor
  struct as the already-drafted `ActorMeleeEngage` (0x80112188) and its same math callees
  (`FUN_80084080` sqrt-via-GTE-LZCS, decompiled this session; `Trig::ratan2`, already native).
  DRAFTED (compile-only, unwired, unverified): `FUN_8001F9DC` -> `MeleeProximity::isAtApproachAnchor`
  in new `game/ai/melee_proximity.{h,cpp}` — the simplest family member, guest 40-byte frame mirrored
  in `isAtApproachAnchorFramed()`. The other 8 family members are named/mapped, not drafted
  (side-effect fields not confidently disentangled in this session's window).
- **Band 0x80050000-0x8005FFFF RE survey (fleet agent, 2026-07-08, RE-ahead-of-frontier — UNWIRED).**
  145-function Ghidra decompile (`scratch/decomp/region_8005.c`) + shard cross-check; 112 of the
  region's 152 `gen_func_8005xxxx` symbols were unowned (PcScheduler + NodeXform's core cluster were
  already owned — skipped per the region assignment). Full breakdown in docs/engine_re.md "Wide-RE
  survey of 0x80050000-0x8005FFFF". DRAFTED (compile-only, unwired, unverified, no SBS run): 5 new
  methods on the already-owned `NodeXform`/`GraphicsBind` classes — `NodeXform::copyMatrixBlock`
  (0x80051B34), `NodeXform::buildFromChild` (0x80051614 — Ghidra mislabeled its parent-table read;
  ground-truth generated C shows it reads `ActorTomba::G_ADDR + tableIdx*4 + 0xC0`, one of Tomba's
  own child-record slots, not a separate global table), `NodeXform::worldPosFromLocal` /
  `worldPosFromComposed` (0x80051D90/0x80051D20 — both route the still-unowned libgte leaf
  `0x800844C0` via `rec_dispatch`), and `GraphicsBind::recordArrayInit` (0x800519E0, batch sibling
  of the already-owned `recordInit`). MAPPED-ONLY (not drafted, refused per "quality > quantity"):
  (a) ~15 controller-vibration/analog-config + XA-audio-cue-queue leaves (0x80052144-0x800527C8);
  (b) ~90-function ActorTomba "G-block" physics/AI FSM (0x800527C8-0x8005FB54) — confirmed via
  field-offset cross-reference against the already-owned `game/player/actor_tomba.cpp` fields, but
  far too large to RE to byte-exact fidelity in one pass; flagged as its own dedicated follow-up
  wave, not a tail end of this one.
- **Band 0x80090000-0x8009FFFF RE survey (fleet agent, 2026-07-08, RE-ahead-of-frontier — UNWIRED,
  NOTHING DRAFTED).** 209-function Ghidra decompile (`scratch/decomp/region_8009.c`) of the 194
  `func_8009xxxx` symbols still on the substrate. Verdict: this band is **entirely PSY-Q SDK LIBRARY
  code** — `libsnd` SEQ music sequencer (`SsSeqOpen`/`Play`/`Stop`/`SetVol`/`Called` tick, per-track
  0xB0 control blocks), the raw `libspu` SPU-register driver (`Spu_Init`/transfer, hardware-register
  offsets confirmed via literal `"SPU T.O.: %s"` timeout strings), `libmcrd` memory-card sector I/O,
  and `libmdec` DCT sync (already correctly HLE'd as `sync_ok` in `sync_overrides.cpp`) — consistent
  with that file's own comment marking `[0x80080000,0x8009E000)` as the resident SCEI library window.
  **No `game/` port is appropriate here** (flagged, not forced, per the wide-RE brief's hardware-driver
  carve-out) — the existing HLE taps + `PSXPORT_DEBUG=seqtick/keyon/septrace/SEQDBG` probes are the
  correct treatment. Separately: `func_8009EB78/EBFC/EC80/EF18` (the only symbols past 0x8009D06C) are
  a **recompiler misdecode of non-code bytes**, not real functions — decompiled bodies are full of
  `UNHANDLED op` garbage and jump to addresses outside valid PSX RAM; Ghidra's own analyzer also finds
  zero functions there. Flagged as a recompiler-coverage bug, not RE'd further. Full writeup:
  docs/engine_re.md "Wide-RE survey: 0x80090000-0x8009FFFF".

**SESSION 2026-07-08 (wide-RE) — 0x80126000-0x8013FFFF (A00 gameplay overlay, middle band):** ~210
`ov_a00_gen_*` symbols in range; 55 already owned pre-session, 5 DRAFTED this session (compile-only,
unwired, unverified, no SBS run), 153 still MAPPED-ONLY. DRAFTED (`game/ai/beh_toy_spawn_family.cpp`):
`beh_arm_countdown_if_linked_ready_80127420`, `beh_distance_band_predicate_801274bc`,
`beh_spawn_toy_child_type5_80127720`/`type4_8012763c`/`type2_80127510` — a per-object "toy/child
spawner" family using the legacy `FUN_80072DDC` allocator (distinct from `Spawn::dispatch`). 14
sibling functions in the same cluster (0x80126040-0x80127450, the top-level dispatcher + physics
integrators) were traced but NOT drafted — they read/write the shared `GBASE=0x800BF870` global blob
`beh_lift_platform.cpp` already references, and need a RAM dump to confirm field roles before porting
byte-exact. Full per-function notes + the complete 153-address UNOWNED table in docs/engine_re.md
"Wide-RE survey: 0x80126000-0x8013FFFF". Flagged two HIGH-VALUE next targets: the case-handler bodies
of already-owned `beh_substate_edge_orchestrator` (6 unowned leaves, 0x8012E8A8-0x80130524) and
`beh_cull_substate_orchestrator` (6 unowned leaves, 0x8013272C-0x80133184) — the orchestrators
themselves are LIVE but dispatch out to substrate for every case body.
- **Band 0x800527C8-0x8005FB54 follow-up wave (fleet agent, 2026-07-08, RE-ahead-of-frontier —
  UNWIRED).** Dispatcher-first pass per the CLAUDE.md guidance. Full breakdown in docs/engine_re.md
  "Region 0x800527C8-0x8005FB54 follow-up wave". DRAFTED (compile-only, unwired, unverified, no SBS
  run, no static caller found — reached only via an indirect fn-ptr "think" slot): `0x800527C8` ->
  `beh_actor_tomba_proximity_combat` (new `game/ai/beh_actor_tomba_proximity_combat.{h,cpp}`) — an
  enemy-vs-Tomba proximity-combat state machine (2 parallel 5-state jump tables keyed on `obj+5`,
  selected by `obj+3`), mechanically transliterated 1:1 from `generated/shard_3.c:13494`
  (register-scratch-preserving style, per `actor_melee_engage.cpp`'s own "don't risk a
  mis-restructure under time pressure" precedent for dense DAGs). KEY FINDING (not drafted, flagged
  as the real next target): `0x8005950C` is Tomba's per-frame G-block state-machine driver, called
  directly from the ALREADY-NATIVE `Engine::frameStartTick`/`frameStartTickFaithful` — most of the
  remaining ~99 functions in the region cascade from it, so it should be RE'd FIRST in the next
  wave rather than treating the region as ~99 independent leaves. The rest of the region (~99
  functions) is triaged into 15 named call-graph families (outer action-selector, target-slot,
  walkStart/gStateMutate-adjacent enter/exit helpers, 3-4 near-identical "variant A/B/C(/D)"
  template triplets, a lift/ride cross-reference with `game/ai/beh_lift_platform.cpp`, etc.) — see
  docs/engine_re.md for the full family breakdown and next-step priority order.
- **`0x8005950C` follow-up wave (fleet agent, 2026-07-08, RE-ahead-of-frontier — UNWIRED).** RE'd +
  drafted the per-frame G-block driver flagged above as the region's real frontier, plus its full
  immediate sub-tree of small/medium callees. New methods on `ActorTomba` (`game/player/
  actor_tomba.{h,cpp}`), all faithful 1:1 ports from `generated/shard_*.c` ground truth (Ghidra
  headless cross-check in `scratch/decomp/g_8005950C.c`), all UNWIRED (compile-only, no SBS run):
  `frameTick` (`0x8005950C`), `turnBiasCompute` (`0x80055C9C`), `outerTransitionGate`
  (`0x80053E50`), `outerTransitionCommit` (`0x80053FDC` — caught + documented a real Ghidra
  decompiler error in this one, see the `.cpp` banner), `assetReady` (`0x80045580`),
  `resetLoadGate` (`0x80042310`). Still-substrate/mapped-only (too large for this pass, each its
  own dedicated-pass candidate): `0x80058648` (case-0 init driver), `0x80058918`/`0x80058F5C` (the
  two ~50-function-deep mode-N dispatch tables `frameTick` cases 1/4 route into), `0x800597AC`
  (matrix-compose loop over an attached-item list). Full writeup in docs/engine_re.md's "0x8005950C
  — Tomba's per-frame G-block driver" section. Next: wire `frameTick` in
  (`Engine::frameStartTick`'s dispatch site + EngineOverrides/`shard_set_override`) + SBS-gate,
  then RE `0x80058648` and one of the two mode-N tables.
- **Case-handler leaves of `beh_substate_edge_orchestrator` + `beh_cull_substate_orchestrator`
  (wide-RE agent, 2026-07-08, UNWIRED).** Followed up on the HIGH-VALUE targets flagged by the
  session above. Of the 12 leaves the two orchestrators dispatch out to, 7 DRAFTED (compile-only,
  unwired, unverified, no SBS run) and 5 MAPPED (call-graph + field-shape RE only, not transcribed —
  too large/uncertain to draft with confidence in one pass). New files
  `game/ai/beh_substate_edge_leaves.cpp` (0x8012E8A8/0x8012F494/0x80130524) and
  `game/ai/beh_cull_substate_leaves.cpp` (0x8013272C/0x80132954/0x80132D58/0x80133184), added to
  `cmake/tomba2_port.cmake`; builds clean (`tomba2_port` links). MAPPED-ONLY (RE'd for call-graph but
  not drafted): `0x8012ED84` (401 gen-C ln, calls `0x8012E8A8` directly + 6 more substrate leaves),
  `0x8012F5B4` (428 ln), `0x8012FD88` (406 ln), `0x80132A88` (162 ln, the cull orchestrator's common
  tail), `0x80132EDC` (146 ln). Full per-leaf field/branch notes in docs/engine_re.md "Case-handler
  leaves of the two substate orchestrators (0x8012E8A8-0x80133184)". Confidence is UNEVEN across the
  7 drafts — 0x8012F494/0x80130524/0x8013272C/0x80132954/0x80132D58/0x80133184 preserve ground-truth
  control flow closely but several field roles (obj[+0x3C..0x7A]-ish counters/flags) are inferred
  from operand shape only, not confirmed against a live RAM dump; 0x8012E8A8 is the highest-confidence
  draft (a clean NodeXform::propagate-family sibling, cross-checked against `game/render/
  node_xform.cpp`'s existing 3 propagate variants). Per fleet-workflow.md §9, a wiring pass MUST
  re-diff every line against `generated/ov_a00_shard_{0,1}.c` before registering + SBS-gating — these
  drafts almost certainly contain bugs per that section's own track record. None of these 7 leaves is
  reachable from intro-area SBS autonav per the orchestrators' own header comments (they sit behind
  the idle/active field path), so wiring will need broader scene coverage to actually gate them.
- **Follow-up: the 5 mapped-only substate leaves, dedicated band (wide-RE agent, 2026-07-10,
  UNWIRED, isolated worktree).** Targeted the exact 5 leaves the 2026-07-08 session above left
  MAPPED-ONLY. Drafted the 2 smaller ones with full confidence: `0x80132A88` (162 gen-C ln, the cull
  orchestrator's common tail — appended to `game/ai/beh_cull_substate_leaves.cpp`) and `0x80132EDC`
  (146 ln, cull orchestrator node[5]==3 — same file). Both transliterated goto/label 1:1 against
  ground truth (not restructured into if/else) specifically to dodge §9's branch-polarity risk; 2
  real bugs were caught and fixed mid-trace before the code was written (an obj[118] literal-vs-
  increment mixup, and two "reset to defaults" branches that jump straight past the shared tail
  rather than falling through it — both would have been silent, hard-to-spot divergences at wiring
  time). Build verified: `tomba2_port` links clean, no warnings on the touched file.
  The remaining 3 (`0x8012ED84` 401 ln, `0x8012F5B4` 428 ln, `0x8012FD88` 406 ln — the edge-
  orchestrator's node[5]==0/1/2 states) were DEMOTED back to MAPPED-ONLY rather than drafted:
  ~2.5-3x the gen-C length of the two just-drafted leaves, wider register frames (r16-r23+r30 vs
  r16-r18), and the same hand-tracing effort that caught 2 bugs in 300 combined lines would not
  scale confidently to 1235 combined lines in one pass. `0x8012ED84`'s opening 5-entry constant-
  table lookup was traced and its call-graph note in docs/engine_re.md corrected (was previously
  documented as "6-entry", confirmed 5 via the loop's own `<5` guard) — an honest map beats a buggy
  draft per fleet-workflow.md §9. Also cross-checked `obj[78]`/`obj[80]`/`obj[64]` field roles
  across the now-6 drafted cull leaves — no contradictions found, but the differing sign
  conventions and the discovery of a new `obj[74]` countdown-source field are recorded in
  docs/engine_re.md so a future reader doesn't mistake them for bugs. Regenerated docs/code-map.md
  (616 natives, 30 ORPHAN — the 2 new drafts show as ORPHAN/unwired, as expected).
- **Two hottest unowned leaves + per-frame input fence (wide-RE agent, 2026-07-09, UNWIRED).**
  Band: `0x80079528` (4235 dispatches/600 free-roam frames), `0x80079374` (4235), `0x800788AC`
  (627, ~1/frame). All 3 DRAFTED (compile+link clean, unwired, unverified, no SBS run):
  `Str::length` (game/core/str.h/.cpp, `strlen`), `Font::drawText` (game/ui/font.h/.cpp, arg-pack
  wrapper around the still-unowned `FUN_80078CA8` font/glyph emitter), `Engine::padEdgeFenceDraft`
  (game/input/pad_edge_fence.cpp, per-frame input-edge fence — computes `DAT_800E7E68`
  pressed/`DAT_800F23A4` released, tail-calls `FUN_8005229C`; does NOT replace the live
  `rec_dispatch(c, 0x800788ACu)` call site in `Engine::frameUpdate()`). Added to
  `cmake/tomba2_port.cmake`. Full RE + confidence notes: docs/engine_re.md "Wide-RE wave
  2026-07-09". `0x80079528` is trivial to wire (no deps). `0x80079374`/`0x800788AC` still
  depend on un-owned callees (`0x80078CA8`, `0x800524B4`, `0x8005229C`) reached via
  `rec_dispatch` — wiring those two first needs a re-diff pass per fleet-workflow.md §9.

- **libsnd/BIOS cluster 0x80086000-0x8009AFFF (wide-RE agent, 2026-07-09, UNWIRED).** Free-roam
  dispatch-count-ordered band: 0x80086288(1254) 0x80090BD0(1254) 0x800909C0(1254) 0x8008913C(627)
  0x80099490(581) 0x800998E4(579) 0x8009A420(521) — all confirmed unowned via `codemap.py --addr`,
  right at the psyq libc/libsnd boundary (`rand`=0x8009A450). 3 DRAFTED (compile+link-only, unwired,
  unverified, no SBS run): `Timing::vsyncCallbackDispatch()` (0x80086288, BIOS VSyncCallback chain
  invoker, runtime/recomp/timing.cpp), `Sequencer::frameTick()` (0x800909C0, libsnd per-VBlank tick
  wrapper, new game/audio/sequencer.h/.cpp, wired onto `Engine::sequencer`), `Core::guestMemset()`
  (0x8009A420, confirmed psyq libc `memset`, runtime/recomp/mem.cpp — has an existing still-substrate
  call site in `game/world/pool.cpp` `Pool::resetControlBlock`/`init` ready for a follow-up direct-
  call swap). 4 MAPPED-ONLY (too deep / semantically unconfirmed for this pass): `0x80090BD0`
  (`SsSeqCalled` — the sequencer engine itself, gated on RE'ing 7 more unowned per-channel leaves
  first), `0x80099490`/`0x800998E4` (both already have LIVE native callers in
  `AreaSlots::updateTail()` via `rec_dispatch` — SPU voice-attenuation/state-buffer builders,
  shape-confirmed but constant tables not fully walked), `0x8008913C` (trivial 3-instruction table
  selector, mechanically easy but semantic role unconfirmed — left unnamed rather than guessed).
  Full per-function RE + reasoning in docs/engine_re.md "Wide-RE wave 2026-07-09 — libsnd/BIOS
  cluster". Also fixed an out-of-band build-blocker discovered while verifying the link: vendor fork
  `spu.c` was missing `SPU_PokeRAM` (declared in spu_state.h, used by verify_harness.cpp) — added
  the symmetric write-back next to the existing `SPU_PeekRAM`; this had been failing EVERY fresh
  checkout's link, unrelated to this band.
- **libgpu "GPU sys" jump-table cluster, 0x80080000-0x80085000 band (wide-RE agent, 2026-07-09,
  UNWIRED).** Task band was the top-12 free-roam-dispatch-count unowned leaves in this range
  (0x80081218 dropped — already owned by Asset::uploadImage). Identified the WHOLE band as libgpu's
  GPU-sys jump table + OT-DMA-send status block (guest 0x800A5998 table, 0x800A5A80-ish status
  region shared with the already-owned `gpu_timeout_arm`/`gpu_timeout_chk`, runtime/recomp/
  sync_overrides.cpp) — cross-confirms and extends the per-frame-loop RE already in docs/engine_re.md.
  6 DRAFTED (compile-only, unwired, unverified, no SBS run): `func_80080F6C`=DrawSync,
  `func_80081458`=ClearOTagR, `func_80082C68`=GPU-DMA status-block reset, `func_80083DE0`=draw-mode/
  texwin packet-header builder, `func_800847B0`=vertex-header repack (LOW confidence on semantic
  role), `func_80084250`=GTE 3-vertex rotate-and-pack (sits right after the owned Math cluster in
  gte_math.cpp — MEDIUM confidence, register-flow exact, source-struct field roles inferred). New
  files `game/render/wide_re_libgpu_leaves.cpp`, `game/math/wide_re_gte_transform3.cpp`, added to
  `cmake/tomba2_port.cmake`; both compile clean with zero warnings (final `tomba2_port` link is
  currently blocked by an UNRELATED pre-existing gap — `SPU_PokeRAM` has no definition anywhere in
  the tree, reproduced identically on baseline `main` with these two files removed — not caused by
  this session's changes, out of this band's scope to fix).
  5 MAPPED-ONLY (too large/uncertain to draft with confidence): **0x800815D0 = PutDrawEnv**
  (CONFIRMED identity, already the doc's own "next widescreen lever" target — calls `func_80081FB0`
  + 5 more unowned leaves, needs those RE'd first) and the **0x80082D04 GPU-DMA completion-callback
  queue cluster** (0x80082D04 itself is the single HIGHEST dispatch-count target in the band at 824
  hits/frame — 0x80082FB4/0x80083364/0x80082424/0x80082734, a 64-slot mutually-recursive ring buffer
  at 0x80100C30, gated by the same timeout status block). Full field-level RE + confidence notes in
  docs/engine_re.md "Wide-RE wave: libgpu GPU sys jump-table cluster". Recommended next: RE the
  0x80082D04 queue cluster properly (highest dispatch count in the band by far) before drafting it —
  getting an interrupt-callback-queue's field semantics wrong is the exact failure mode
  fleet-workflow.md §9 warns about.- **SsSeqCalled cluster follow-up, 0x80090BD0 + its 7 per-channel leaves (wide-RE agent,
  2026-07-09, UNWIRED).** Picked up the prior wave's "MAPPED, NOT drafted" gap. RE'd (and
  CORRECTED — the prior pass's globals were off by an 0x8000/0x8xxx transcription slip;
  reentrancy flag/active-mask/pointer-array/counts are actually 0x80104C24/28/30 and
  0x801054B0/B2, re-derived fresh from `generated/shard_3.c`'s current gen body, not
  0x8010CCxx/0x80109Exx) and DRAFTED `Sequencer::seqChannelDispatch()` (0x80090BD0 SsSeqCalled
  itself — the reentrancy-guarded 7-seq x 15-chan double loop + all 8 per-channel flag-bit
  tests) plus 3 of its 7 leaves: `channelPitchSelectDispatch` (0x800910F0, thin wrapper),
  `channelReleaseClear` (0x80091050), `channelStopFlagSet` (0x80091910, true leaf). The other
  4 leaf call sites (0x80090E40 x2, 0x80092080 x2, 0x80091970 x1) stay `rec_dispatch(c, addr)`
  call-outs inside the now-native double loop — same pattern `frameTick()` already used for
  SsSeqCalled itself; those 3 leaves are deep ADSR/pitch-slide/note-init state machines with
  2+ further unowned callees each (0x80091120, 0x80095A9C, 0x80095530, 0x80095B90, 0x800931A0)
  — control flow fully RE'd (generated/shard_4.c:15017 / shard_1.c:17775 / shard_4.c:15144) but
  field semantics beyond the flags/counter fields are inferred, not confirmed; deprioritized
  under the wide-RE effort budget. All new code in `game/audio/sequencer.{h,cpp}` (extends the
  existing `Sequencer` class rather than a new file). Full per-function RE + bit-to-leaf map in
  docs/engine_re.md's sequencer.h-referenced comment block. Note: 0x800931C0 (the "prep call"
  fired once before SsSeqCalled's seq loop) was ALREADY drafted by an earlier pass as
  `input_dispatch_931c0` (game/input/input.cpp, static free fn, unwired) — left as-is, called
  via `rec_dispatch` from `seqChannelDispatch()` since it's a different TU's static symbol.
  fleet-workflow.md §9 warns about.

- **Band 0x8010A000-0x8010CFFF + 0x80106AC4, SOP-intro-cutscene sub-tick cluster (wide-RE agent,
  2026-07-10, UNWIRED).** Task band was the 8 hottest free-roam-dispatch-count unowned leaves in this
  range. 6 of 8 DRAFTED (compile-only, unwired, unverified, no SBS run), + 2 confirmed dependencies
  pulled in for completeness: `sopBeatAdvanceWalk`/`sopBeatAdvanceNarration` (0x8010AF60/0x8010B078,
  HIGH confidence on the transcription — clean SCENE_BEAT timer sequencers — but the TRIGGER is
  INFERRED: zero static call-site xrefs, both addresses are raw data in a keyframe/anim-event-shaped
  table at 0x8010CA60-0x8010CAAC), `sopIntroEffectTick`/`sopIntroEffectSpawn`/`sopOrbitPathStep`
  (0x8010B2D4/0x8010B44C/0x8010B11C, HIGH confidence, xref-confirmed spawn+dispatch chain: a 3rd SOP
  model (id 0xC) spawned+ticked via the same node+0x1C mechanism as `beh_sop_overlay_shadow`),
  `sopLiftedSubtick` (0x8010B588, HIGH confidence — closes the "lifted actor's own deeper sub-tick"
  frontier note from the earlier sop_overlay_shadow pass; docs/port-progress.md's own prior "left for
  its own pass" pointer), `Demo::s3SubMachine` (0x80106AC4, HIGH confidence — the main-menu title
  cursor sub-machine `Demo::s3()`/`demo_frame_s3()` already document as "mirror of 0x8010696C" but
  still rec_dispatch). New file `game/ai/sop_intro_events.{h,cpp}`, `Demo::s3SubMachine` added to
  game/scene/demo.{h,cpp}; both added to `cmake/tomba2_port.cmake`; full `tomba2_port` build+link
  verified clean (zero warnings) after `git submodule update --init --recursive` (vendor/beetle-psx +
  its libchdr sub-submodule needed a fresh checkout in this worktree).
  1 MAPPED-ONLY (context uncertain, not a transcription gap): `beh_orbit_spark_effect` (0x8010BEAC) —
  state-machine transcription is HIGH confidence but its only xref is a raw DATA reference from a
  MAIN.EXE-resident per-type table (0x800A22B8), not SOP-local — likely a GENERIC reusable particle
  type, spawner not traced this pass.
  1 DEFERRED BY DESIGN (not an RE gap): `0x8010C26C`/`0x8010C79C` — already correctly flagged
  "DEFERRED to render rebuild" above (raw GP0 packet emitters; CLAUDE.md's "REBUILD, don't transcribe"
  render rule bans a byte-exact draft here). Re-confirmed this pass: neither decompiles cleanly from
  `ram_game.bin` (Ghidra hits bad-instruction-data at both — that dump doesn't have GAME.BIN resident
  at those addresses); a future render-rebuild pass needs a fresh capture with the field's parallax/
  end-of-area-text actually on screen. Full field-level RE + confidence notes in docs/engine_re.md
  "Band 0x8010A000-0x8010CFFF wide-RE wave".
- **libgpu GPU-DMA completion-callback queue cluster (wide-RE agent, 2026-07-10, UNWIRED, dedicated
  deep pass).** Prior wave (2026-07-09) explicitly deferred this cluster citing fleet-workflow.md §9's
  "9 bugs in one function" risk (~380 gen-C lines, mutual recursion). This session RE'd the whole call
  graph from `generated/shard_*.c` `gen_func_<addr>` bodies line-by-line (every branch polarity
  checked twice against the gen-C) and DRAFTED 4 of the 5 targeted addresses:
  `0x80082D04`=GpuDmaQueueEnqueue (824 dispatch hits, the single highest-value target in the whole
  band), `0x80082FB4`=GpuDmaQueueDrain (also the GPU-DMA interrupt-handler body), `0x80083364`=
  GpuDmaQueueSync, `0x80082424`=GpuDmaSend (the DMA kick). New file
  `game/render/wide_re_gpu_dma_queue.cpp`, added to `cmake/tomba2_port.cmake`; `tomba2_port` build+
  link verified clean. **Corrected a wrong field-map guess from the prior wave**: ring head/tail
  counters are at `0x800A5AC8`/`0x800A5ACC`, not `0x800A5A88`/`0x800A5A8C`. Full field map + call
  graph in the new file's header comment and `docs/engine_re.md`'s "Wide-RE wave 2026-07-10" section.
  1 MAPPED, NOT drafted: `0x80082734` — turned out on RE to be a separate, larger (48-byte frame, 6
  spills) libgpu `LoadImage()`-style chunked GP0-FIFO pixel streamer, NOT part of this cluster's
  mutual recursion (shares only the busy-wait idiom on the same status-block globals). HIGH confidence
  on role, MEDIUM-LOW on the rect-clip/chunking arithmetic — left for its own dedicated RE pass.

- **Band 0x800506D0/0x800420AC/0x80042090/0x80042E10/0x80043108/0x80041468/0x80040FA0, scheduler
  sleep-countdown + ScriptInterp opcode-table cluster (wide-RE agent, 2026-07-10, UNWIRED).** Task
  band was the 9 hottest free-roam-dispatch-count unowned leaves in the range; 2 of the original 9
  (0x800518FC `Engine::objMatrixCompose`, 0x8004190C `Engine::animTick`) were already owned and
  skipped. 5 of 7 DRAFTED (compile-only, unwired, unverified, no SBS run): `PcScheduler::
  tickSleepCountdown` (0x800506D0, `game/core/pc_scheduler.{h,cpp}` — the long-referenced "sleep
  countdown / re-arm 1->2" sweep, never previously drafted as a body); `ScriptInterp::
  op05WaitFrames`/`op06TestSceneFlag`/`op34ClaimGate`/`advanceStep` (0x80042090/0x800420AC/
  0x80042E10/0x80040FA0, `game/scene/script_interp.{h,cpp}`) — 4 of the 63-entry script-opcode
  handler table at guest 0x800A3B78 (positively identified by reading the table directly out of
  MAIN.EXE .rodata, not inferred), plus the "advance sub-machine" wrapper `advanceEntry` currently
  only `rec_dispatch`es to. TWO REAL BUGS caught and fixed mid-draft before landing (op06's
  exact-match arm was truncating argC to a byte instead of sign-extend-comparing; op34 was checking
  the "no-wait" sign bit even when the guest skips that check entirely) — see docs/engine_re.md for
  the full write-ups; a reminder that even "simple" opcode handlers need line-by-line care.
  2 MAPPED-ONLY, refused for drafting (dense fixed-point-math DAGs — same risk class the melee wave
  got wrong 6 times in one function, docs/fleet-workflow.md §9): 0x80043108 (opcode 36, 95 dispatch
  hits — squared-distance/sqrt/div movement solver with guest div-by-zero `rec_break` traps) and
  0x80041468 (opcode 31, 11 hits — ratan2-based turn-toward-target). Both need a dedicated Ghidra
  cross-check pass, not a from-scratch manual MIPS transcription. Full field-level RE in
  docs/engine_re.md "Wide-RE pass 2026-07-10 — scheduler sleep-countdown + 4 ScriptInterp opcode
  handlers".

- **Dedicated follow-up: op36/op31 movement-script family (wide-RE agent, 2026-07-10, UNWIRED),
  closing the two functions the prior wave refused.** Both `0x80043108` (op36, "move toward a
  script-literal target position") and `0x80041468` (op31, "turn self-or-designated actor toward a
  computed angle") DRAFTED, plus their still-unowned callees `0x80041438` (`ScriptInterp::
  turnFacing`/`turnFacingFramed`) and `0x80042EA4` (`ScriptInterp::stepEventPulse`/
  `stepEventPulseFramed`), plus a bonus leaf `0x8004139C` (`ScriptInterp::stepAngleToward`, the
  actual angle-nudge primitive `turnFacing` delegates to) — all in `game/scene/script_interp.{h,
  cpp}`. Method: raw `generated/shard_*.c` (instruction-exact ground truth) transcribed near-
  literally, THEN independently cross-checked against a fresh Ghidra headless decompile (`tools/
  decomp.sh`, `scratch/decomp/op36_op31_band.c`) function-by-function. The cross-check caught ONE
  real Ghidra-vs-raw-C divergence (Ghidra misplaced a `trap(0x1c00)` div-by-zero check into the
  wrong spot in op36's step-schedule solver — the raw recompiled C has no trap there; ground truth
  wins per CLAUDE.md) and this session self-caught one of its OWN transcription slips on a second
  hand-trace before writing any code (op31's sign-bit actor-selection direction was initially
  backwards). All guest frames mirrored per CLAUDE.md ("MIRROR THE GUEST STACK") even though this
  tier stays unwired/ungated (sp descent + LIVE-value spills at the RE'd offsets, matching game/
  world/object_table.cpp's style). Corrected the ORIGINAL mapped-only guess that op36's obj+108
  pointer was a separate "target object" struct — it is in fact the SAME `OBJ_SCRIPT_PTR` field
  the interpreter itself uses (obj+0x6C), and op36 reads the target position directly out of the
  CURRENT script entry's argA/B/C (Z/Y/X) plus its extended-block halfword, not from any secondary
  object. Similarly corrected op31's "0x8064*10000-ish constant table" guess to a single scratchpad
  pointer slot (0x1F800214), not a table. Builds and links clean (`cmake --build build2 --target
  tomba2_port`); confirmed via `tools/codemap.py --addr` that all 5 addresses now resolve LIVE with
  zero remaining unowned callees in this cluster. Full field-level RE in docs/engine_re.md "Wide-RE
  follow-up 2026-07-10 — op36/op31 movement-script family (drafted)". UNVERIFIED — no override
  registration, no SBS run; a future frontier-tier session must wire + gate before trusting the
  behavior in-game.

- **SsSeqCalled cluster, remaining bit4/5/6/7/2 leaves (wide-RE agent, 2026-07-10, UNWIRED).** Closes
  the prior wave's "MAPPED, NOT drafted" trio: `channelPitchSlideTick` (0x80090E40, bit4/5,
  portamento ramp — MEDIUM confidence, register-literal goto/label transcription per multiple
  re-converging branches), `channelEnvelopeRampTick` (0x80092080, bit6/7, ADSR ramp — MEDIUM
  confidence, true leaf/no stack frame, same goto/label style), `channelNoteInit` (0x80091970, bit2,
  note retrigger — MEDIUM confidence, mostly linear, structured C++). Also drafted their own small
  callees: `channelVolumeSnapshot` (0x80095A9C, HIGH confidence, true leaf), `channelKeyEventScan`
  (0x80095B90, LOW-MEDIUM confidence — role of the hw-voice-bitmask scan not independently confirmed,
  inherited from the prior wave's note; control flow exact), `channelKeyRegisterMerge` (0x80094B50,
  MEDIUM confidence, true leaf, builds a KON-style register pair). `seqChannelDispatch()`'s 4
  remaining `rec_dispatch` call-outs for these addresses swapped to direct native calls (still fully
  UNWIRED overall — nothing in this chain is registered anywhere). One callee left MAPPED, not
  drafted: 0x80095530 (the SPU voice-register write leaf `channelPitchSlideTick` calls — ~320 lines,
  a KON/pan-table write loop, too large for this pass). All new code in `game/audio/sequencer.cpp`
  (extends the existing class); full `tomba2_port` build+link verified clean. Full field-level RE +
  the corrected offset math (several of this wave's clamp masks required re-deriving from the exact
  gen decimal immediates rather than trusting the prior wave's prose summary) in sequencer.h's header
  comment and docs/engine_re.md.

- **SPU voice-register write leaf + font/glyph emitter (wide-RE agent, 2026-07-10, 2nd disjoint
  band, UNWIRED).** Closes the prior sequencer wave's own deferred gap plus a separately-deferred
  font leaf:
  - `Sequencer::channelVoiceRegisterWrite()` (0x80095530, `game/audio/sequencer.cpp`) — the ~320-line
    "SPU voice-register write leaf" `channelPitchSlideTick()` still `rec_dispatch()`es to. Drafted
    register-literal/goto-label, LOW-MEDIUM confidence (dense fixed-point pan/volume compute over
    the same stride-56 voice-record array `channelKeyEventScan()` reads; field ROLES beyond what's
    needed to name the method are inferred, not confirmed). Its own callee `channelVoiceSelectPrep()`
    (0x800962B0, true leaf) drafted alongside it.
  - `Font::glyphEmit()` (0x80078CA8, `game/ui/font.cpp`/`.h`) — the font/glyph emitter
    `Font::drawText()` tail-calls, previously filed by a 2026-07-09 wave as "large, separate scope,
    not drafted" (403 gen-C lines). Re-read: gen-C lines 211-402 are confirmed UNREACHABLE dead code
    (a real `return` at line 210 with no label past it, same shard-grouping artifact seen elsewhere)
    — the LIVE body is only ~180 lines, tractable. Drafted register-literal/goto-label: null-
    terminated string walk over a fixed scratch struct at guest `0x800C0000` (corrects an earlier doc
    note that misplaced "cursor state" at scratchpad `0x1F800000..0x1F80001F`), per-byte dispatch
    (space/`\n`/control bytes 1-4/default-glyph), glyph arm prepends a GP0 packet at the shared
    packet pool (`PKT_POOL_PTR` 0x800BF544, same pool every other render leaf uses). Calls the
    already-owned `func_80083DE0` (game/render/wide_re_libgpu_leaves.cpp) via `rec_dispatch`, and
    leaves the still-unowned `FUN_80078988` (box/rule primitive, 4 call sites) `rec_dispatch`'d —
    out of band, not drafted. LOW-MEDIUM confidence — control flow + the scratch-struct base-address
    correction are solid (direct re-read), individual field roles beyond that are inferred.
  Both new methods build+link clean in a fresh `build2`; UNWIRED (no override registration, no SBS
  run) per docs/fleet-workflow.md §6. A wiring pass MUST do the line-by-line gen-body re-verify §9
  requires before registering either as an override. Full RE writeup in sequencer.h/font.h header
  comments and docs/engine_re.md.
- **libgpu dedicated pass: LoadImage streamer + PutDrawEnv chain (wide-RE agent, 2026-07-10,
  UNWIRED).** The two libgpu targets both prior waves explicitly deferred, now drafted from raw gen-C:
  `func_80082734` (LoadImage-internal chunked GP0-FIFO pixel streamer, 48-byte frame / 6 spills
  mirrored, HIGH confidence on flow, MEDIUM-LOW on the async chunk-DMA handoff semantics) in
  `game/render/wide_re_gpu_loadimage_streamer.cpp`; `func_800815D0` (PutDrawEnv, s-regs kept LIVE
  across dispatches for callee-frame fidelity, HIGH confidence) plus 4 of its 5 unowned callees
  (0x80082240/0x800822D8 SetDrawArea TL/BR, 0x80082370 SetDrawingOffset, 0x80082220 DR_TPAGE,
  0x8008238C DR_TWIN — all HIGH confidence true leaves) in `game/render/wide_re_gpu_putdrawenv.cpp`.
  MAPPED not drafted: 0x80081FB0 (the DRAWENV packer — 147 gen-C lines; 6-word header path fully
  RE'd, FillRect tail's two scratch-word sources untraced — §9 bug-farm shape, deferred honestly);
  0x8009A3E0 (memcpy-like, out of band). ALSO FIXED two real bugs found in the already-committed
  `wide_re_libgpu_leaves.cpp` drafts (inverted boot-flag hook polarity in DrawSync AND ClearOTagR;
  ClearOTagR dummy-tail constants off by 0xC0) plus address corrections to prior prose (clip pair =
  0x800A59A4/A6, GP0 port ptr = 0x800A5AA4). Everything unwired; build+link verified clean. Full RE
  in the two file headers + docs/engine_re.md "Wide-RE wave 2026-07-10 — dedicated libgpu pass".