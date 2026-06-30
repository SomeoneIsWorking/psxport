# Porting the INTRO NARRATION cutscene to native PC (own it, don't patch around it)

**Why this doc exists.** The intro narration (the opening story before free-roam) has visible bugs the user
reported on the running game: (1) a DOUBLE fade-in, (2) the 3rd scene renders the SEA when it should be a
dark VOID, (3) the 4th scene (cliff/sea) looks CORRUPT. These cannot be diagnosed or fixed from outside,
because the narration's content runs as still-PSX recomp/overlay code that the engine merely `rec_dispatch`es
into. The directive (user, 2026-07-01, firm): **OWN the narration — RE each piece and reimplement it
natively** so we control (and can see) the fades, backgrounds, scenes, and text. Stop patching the substrate.

There is **NO oracle** (the reference emulator/diff tooling was removed). Verification is **USER EYEBALL**:
make a principled native change, build, capture `scratch/screenshots/n_*.png`, and have the user confirm
against their memory of the original. The agent CANNOT self-verify the look.

## What the narration actually is (RE, later-275/276)
- It runs in the GAME stage, **SOP field mode** (`sm[0x4a]==0`), driven by `ov_sop_field_mode`
  (engine/sop.cpp) — a per-area state machine `sm[0x50]`: 0=LOAD(fade black)→1=FADE-IN→2=GAMEPLAY→
  3=FADE-OUT→4=RESET(next). State 0 sets a 30-frame startup delay `sm[0x60]=0x1e`.
- It is **ONE SOP area** (area index `*0x800bf870` stays 0 the whole narration — the scenes are NOT separate
  area reloads). It spawns **3 scene objects** in `ov_sop_field_mode` state 0 (sop.cpp:253-261) from table
  **`0x8010c98c`** (stride 12: +0/2/4 = node pos SVECTOR, +6 = flags 0x0303, +8 = per-scene HANDLER ptr):
  - scene 0: pos(0x0FD2,0xFA24,0x4602) handler **0x8010ACFC**
  - scene 1: pos(0x122A,0xFA56,0x44D6) handler **0x8010B798**
  - scene 2: pos(0x0FA0,0xEC78,0x4650) handler **0x8010B990**
  (these handlers are stamped into each spawned node's +0x1c; they are the per-scene CONTENT — backgrounds,
  characters, choreography. The on-screen sequence the user sees is field → letter/characters → void → cliff,
  so a single handler likely drives several beats via its own sub-state + the text scroller.)
- Per-frame content the SOP narration still DISPATCHES (the ownership work-list — reimplement each native):
  - `0x8010a0e0` entity-update loop (a0=0x800f2418)        — sop.cpp:195
  - `0x8007b008` Tomba update                              — sop.cpp:196
  - `0x8010bffc` parallax BG draw (a0=0x800ed018)          — sop.cpp:206  ← background
  - `0x8010c26c` BG tile scroller (a0=0x800ed018)          — sop.cpp:219  ← background
  - `0x8010c79c` end-of-area TEXT scroller (the narration text) — sop.cpp:226
  - the 3 scene handlers 0x8010ACFC / 0x8010B798 / 0x8010B990 (node+0x1c)  ← scenes/backgrounds
  - `ov_bg_scene_transition_sm` (engine/bg_scene_transition_sm.cpp, FUN_8002655C) — ALREADY native; the
    scene-to-scene fade/transition machine (state `*(0x80100404)`, struct P=0x80100400).
  NB `0x8010c98c` etc. are SOP-OVERLAY addresses — `tools/disas.py` only reads MAIN.EXE, so read these from
  the recompiled bodies `generated/ov_sop_*` or from live RAM (`r <addr>` in the REPL during the narration).

## BUG 1 — the double fade-in (ROOT CAUSE FOUND, fix is a design call)
Traced with a fade-trace on `gpu_set_fade` + addr2line on the call sites. At the narration start the screen
fades in TWICE, in sequence:
1. `ov_sop_field_mode` state-1 FADE-IN (sop.cpp:277) runs r=255→8 **during the 30-frame startup delay**.
   During that delay `ov_sop_field_update` early-returns (no render walk), but `ov_scene_native` still draws
   the entity lists, so what fades in is the partial/stale scene (the user: "only the text fades in").
2. Then `sm[0x50]`→2 and `ov_bg_scene_transition_sm` runs its **state-0 UNCONDITIONAL full black**
   (bg_scene_transition_sm.cpp:83 `fade_rect(0xffffff)`) then **state-1 fade-in** (line 90) — the user's
   "everything turns black again, then the whole scene fades in."
Both are faithful native ports of separate PSX fade sources, but our `engine_fade_set`/`gpu_set_fade` is a
SINGLE fade state (last-writer-wins), and the two machines fire at different times, so the scene visibly
appears twice. THE FIX (design call, then user-verify): the narration scene should fade in ONCE. Most likely
the `ov_bg_scene_transition_sm` state-0→1 is the intended "scene appears" fade (it runs after the scene is
built), and the SOP `sm[0x50]` state-1 fade during the pre-build startup delay is the spurious one — so hold
black through the startup delay and let the bg-transition own the single fade-in. MUST be reimplemented
coherently (don't surgically null one `engine_fade_set` — the SOP fade is shared with other SOP areas; verify
free-roam/SOP areas still fade correctly). Verify by eyeball.

## BUG 2 — scene 3 renders SEA, should be a dark VOID
Not yet root-caused. The background is drawn by `0x8010bffc` (parallax) + `0x8010c26c` (tile scroller) +
the master BG drawer `0x8003df04` (16-state jump table @0x80014fc0 keyed on `*0x800bf870`, gated by
`*0x800bf873==0` — see engine_render_walk.cpp ov_scene_native backdrop). The "void" scene should draw NO
background (black) but draws the sea layer. Lead: RE the scene-2 handler 0x8010B990 (and what it sets for the
background select / the bg-transition `dir`), and the BG drawer state for that scene. Decompile via the
recompiled `ov_sop_gen_8010B990` body (generated/ov_sop_shard_*.c) or Ghidra-headless (skill `decomp-port`).

## BUG 3 — scene 4 (cliff/sea) looks corrupt
Not yet root-caused. The sea/water rendering in the cliff scene looks wrong (striped/garbled in the captured
n_560/n_860 frames). Likely the still-PSX water/background submit for that scene. RE the relevant scene
handler + the water draw; own it natively (PC-native water, real depth) per the render directives.

## What is ALREADY done (this session, later-275, committed + pushed)
- Narration recomp-MISS `0x8010BF54` FIXED: overlay-ID re-validation in `resident_overlay()` (the cache was
  mis-identifying the resident MODE-slot overlay) + a dangling-render-pointer guard `rec_addr_has_entry()`
  (skip a render node whose fn isn't a real entry of the resident overlay). overlay_router.cpp + emit.py
  (RecOverlay.idx, RECOMP_VERSION 2026-07-01.1).
- Narration→field TRANSITION CRASH FIXED (render-queue overflow): `ov_scene_native` was drawing the new
  area's objects on the field-area-init frame (sm[0x4a]==1,sm[0x4e]==0) before model-attach → garbage
  geomblk. Suppressed the field draw on that init frame (engine_render_walk.cpp), gated on the persistent
  GAME state machine. So the full narration now PLAYS THROUGH without crashing.
- TDD: `PSXPORT_SELFTEST=narration` (runtime/recomp/selftest.cpp) drives the native shipping path through the
  un-skipped narration and asserts no overflow + the GAME loop keeps running. RED→GREEN. Extend it as the
  port lands the fade/background fixes (it is STATE-based; the LOOK still needs eyeball).

## How to work this (the loop)
1. Reach the narration headless: `printf 'newgame\nrun N\n...' | PSXPORT_REPL=1 PSXPORT_VK_HEADLESS=1 ./scratch/bin/tomba2_port`.
   Narration is GAME sm[0x4a]==0, sm[0x50] 0→2, frames ~25..~900. Capture frames with `shot <path.ppm>`;
   convert with `convert`/PIL; the user eyeballs. (Reference frames captured this session: the 4 scenes are
   field → "letter/Tabby" characters → "was she kidnapped?" → "Tomba jumps... And then..." cliff.)
2. Own ONE dispatched fn at a time (RE its recompiled body → reimplement native in a narration-owned file →
   route the SOP dispatch to it). Keep the recomp body as the behavioral reference.
3. Gate progress on the USER eyeball (no oracle) + keep `PSXPORT_SELFTEST=narration` green for the no-crash/
   progression invariants.

## STATUS UPDATE (later-281, oracle-verified) — bugs 2 & 3 FIXED
The software-GPU ORACLE (docs/oracle.md) proved the intro narration is a 2D-COMPOSITED cutscene rendered
ENTIRELY by the dispatched PSX SOP GP0 stream (void = full-screen black fill + semi-transparent textured
swirl EFFECT quads + Tomba sprite + text; cliff = full scene incl. clean sea tiles). The native port had
treated it as the walkable FIELD (ov_scene_native + g_ot_2d_only=1), which drew a stale field SEA in the
void and dropped the cutscene's own fills/effect. FIX (engine/game_tomba2.cpp): detect the narration by the
SOP overlay signature *(0x80109450)==0x3C021F80 (NOT sm[0x4a]); walk the FULL guest OT; run ov_scene_native
only for the 3D-world beats (skip the void, scene byte 0x800bf9b4==5). Native now matches the oracle for
every beat; free-roam unaffected. Bug 1 (double fade) was already fixed (later-277). See
docs/findings/render.md "Intro-narration cutscene rendered wrong".
