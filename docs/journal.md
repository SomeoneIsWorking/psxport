# Debug / progress journal

## 2026-06-17 — chan4 area music "loops ~18 s early" FIXED (spurious interleaved EOF)
User report: the gameplay area music after the fisherman cutscene (chan4 XA, clip [84515..97979]
loop=1, = 89.76 s) loops noticeably earlier than it should (~90 s expected). RE'd via offline +
runtime diagnostics:
- **Data/loop point is CORRECT.** Standalone `tools/xa_wavdump` (no game boot; opens the CHD and
  decodes any chan/[start..end] with the same xa_decode_sector as the port) renders the chan4 song:
  89.76 s to end 97979, and the user confirmed ~90 s is right. So NOT an end_lba/data error.
- **Steady-state audio is realtime** (PSXPORT_AUDIO_RATE meter: ~44100 samples/s, no sustained queue
  drops). So NOT a global playback-rate drift either.
- **ROOT CAUSE (runtime):** `xa_decode_next_sector` terminated the stream on ANY EOF submode bit
  (raw[18] & 0x80), including EOF on a NON-matching interleaved sector. The [84515..97979] range has
  a non-audio sector at **LBA 95338** carrying EOF (an unrelated file/channel's end-of-file). That
  killed the chan4 music ~18 s early (95338-84515 = 10823 of 13464 sectors ~= 72 s), and the
  dialog-coord resume (`cd_override.c` `xa_dialog_coord`) restarted it from the top -> "loops early".
  Caught live: a repeating `[xa] EOF (non-audio) @ LBA 95338` -> `xa_active=0` -> `[xa] PLAY clip chan=4`
  cycle in the field.
- **FIX (`xa_stream.c`):** a BOUNDED clip (`s_end_lba != 0`) ends strictly at `end_lba` (already the
  `s_lba > s_end_lba` loop) - EOF markers from other interleaved channels inside the range are ignored.
  EOF still terminates an OPEN-ENDED stream (`s_end_lba == 0`); `xa_stream_start` now clears
  `s_end_lba`/`s_loop` so a stale clip end can't make an open-ended stream look bounded.
- **VERIFIED:** runtime now logs `[xa] LOOP chan=4 ... span=13464 sectors` (full clip), no 95338 stop;
  a captured field SPU WAV autocorrelates to a **90.02 s** loop period (was ~72 s).
- **Tooling added (reusable):** `tools/xa_wavdump.c` (+`build_xa_wavdump.sh`) offline XA extractor;
  `PSXPORT_AUTO_GAMEPLAY=1` state-gated navigator in native_boot.c (pulses **Start** - the fisherman
  dialog cutscene advances on Start edges and otherwise hard-stalls headless at sm[4e=9]; releases
  input once chan4 area music loops continuously = field) + `pad_repl_release()`; `[xa] LOOP` log;
  `PSXPORT_AUDIO_RATE` production-rate meter.

## 2026-06-17 (later-88) — HW renderer M0–M2 + M2b: Vulkan present + GPU VRAM image + triangle rasterizer; finding: Tomba2 is texture-dominated.
Built the Vulkan/MoltenVK renderer foundation (approved plan; runtime/recomp/gpu_vk.c, PSXPORT_VK=1):
- **M0**: SDL_Vulkan swapchain + fullscreen-quad present (SW still rasterizes s_vram, VK presents it).
- **M1**: VRAM as a device image (R16_UINT 1024x512 = 1555); present samples it + unpacks in-shader.
- **M2**: GPU triangle pipeline (flat/gouraud) drawing into the VRAM image; readback. Self-test
  PSXPORT_VK_TRITEST=1 PASSES (flat fill 0x001f exact, gouraud interpolated, clear correct).
- **M2b**: tee UNTEXTURED polys from gp0_exec into gpu_vk_draw_tri (absolute VRAM coords) + a per-frame
  VK-vs-SW diff (PSXPORT_VK_DIFF=frame: upload SW VRAM as bg, draw VK tris on top, readback, count
  mismatches, dump scratch/screenshots/vk_diff.ppm).
- **FINDING:** gameplay frames have **zero untextured polys** (f2500/f4500 both "no untextured tris").
  Tomba2 draws virtually everything TEXTURED. So M2 coverage on this game is ~nil — the untextured
  rasterizer is a validated foundation, but **M3 (textured tris + CLUT-in-shader + semi-transparency)
  is the milestone that actually renders Tomba2**. Pivot the renderer effort there next.
- Verified: builds/link with -lvulkan; headless unaffected (VK only with a window); windowed VK up
  (1280x720, RADV); present + self-test + diff-harness all run clean. SW path + default untouched.

## 2026-06-16 (later-87) — ROOT-CAUSED the WIDE60 "tripled weapon icon": the re-rasterized in-between is LOSSY. Interim fix: present the real frame when not interpolating.
User: the tripling happens ONLY with PSXPORT_WIDE60=1 (the port), and is the only thing broken.
Diagnosed via the offline A/in-between/B dump at the field scene (f4720, spiky-ball enemies in water):
- **A (prev real) = 1 ball, B (cur real) = 1 ball, but the in-between (s_interp) = 3 balls.** Prim count
  ~1325 = one frame (NOT multi-frame accumulation). Ball-region prims are all `obj=0` (untagged, per
  later-86), so the synth snaps everything — yet the in-between still differs from VRAM.
- **ROOT CAUSE:** `wide60_synthesize` rebuilds the in-between by re-rasterizing only the captured GP0
  poly/sprite/line subset into a cleared buffer. It does NOT reproduce occluders the real frame uses
  (semi-transparent water over the balls, fills, uncaptured ops, exact blend/draw-order/mask), so
  objects that are hidden/dimmed in the true frame REAPPEAR in the in-between → spurious extra copies.
  The "capture display list + re-rasterize" approach is fundamentally lossy. (Generalizes the later-81
  "uncaptured prim types" caveat.) The correct fix is the native renderer (own every draw + occlusion).
- **INTERIM FIX (engine/wide60.c):** `wide60_synthesize` returns the #prims actually interpolated;
  `wide60_present` blits the LOSSY s_interp only when that's >0, else presents the REAL current frame
  (back buffer A, then real front B). Since interpolation is blocked (obj tags unavailable → moved=0
  always), WIDE60 now shows real frames = NO tripling, at the cost of no true in-between until the
  native renderer supplies object-tagged draws. Headless unaffected (faithful branch). NEEDS live check.

## 2026-06-16 (later-86) — Phase 4 object interpolation: reworked to node-pointer identity; BLOCKED by decoupled render pass (key finding). Falls back to safe all-snap.
Replaced wide60's GTE-fingerprint matcher with **per-object 2D screen-translation keyed by the entity's
pool-slot pointer** (the stable identity from later-84): each captured poly tagged with `Prim.obj`
(= `g_current_object`), per-object screen centroid matched across frames by pointer, prims translated to
the midpoint (`engine/wide60.c` ocen_* + native-walk `call_handler` sets g_current_object=node around
each handler). The translation math is correct and can't smear/dupe (rigid per-object move, gated).
- **BLOCKED (measured, decisive):** `rtp/frame ≈ 3268` but **`rtp_with_obj = 0`** — the render-time GTE
  RTPS fire in a **separate pass with NO object context**; neither the cull dispatcher (LOD math only,
  no RTPS) nor the `FUN_8007a904` handler walk brackets the drawn geometry. So `Prim.obj` stays 0 →
  everything snaps. (Generalizes later-82's "g_current_object stays 0".) Current behavior: clean
  all-snap (no smear/flicker/dupes; 60fps cadence shows each real frame ~twice = judder, not true
  in-between). Opt-in (PSXPORT_WIDE60); default faithful path untouched.
- **UNBLOCK paths:** (a) RE the render pass and set object context where per-object RTPS happens; or
  (b) **get object-tagged draws from the planned native (VK/MoltenVK) renderer** — it knows which entity
  each draw belongs to, so interpolation falls out. (b) is cleaner and aligns with the next track.
- grid_get sentinel fixed (was returning 0xFFFFFFFF for obj-0 cells, which masked this in the old
  approach — "join 92-99%" was joining everything to one blob). Now returns 0 → honest snap.

## 2026-06-16 (later-85) — Phase 1: native entity-list walk LANDED (FUN_8007a904), default-on, oracle bit-identical.
First native engine-layer function: `engine/engine_tomba2.c` reimplements the per-frame object driver
`FUN_8007a904` in native C — walks both entity lists (heads 0x800fb168/0x800f2624) via next@+0x24,
clears render_flag@+1, calls each node's handler@+0x1c via `rec_dispatch` (gameplay STAYS PSX). `next`
read before the handler runs (handler may unlink the node) and held in a host local. Second list head
re-read fresh after list 1 (matches the recomp reload).
- **Verified faithful:** VRAM bit-identical (1 MB `cmp` PASS) native-walk vs recomp body at frames 4000
  AND 4720 of real gameplay; default(native)==recomp-fallback at f4000. Visits 110-157 nodes/frame.
- **Default-on** (the native engine owns the walk); `PSXPORT_RECOMP_OBJWALK=1` restores the recomp body
  as the oracle. `PSXPORT_ENGINE_DBG=1` logs node counts. This is the seam the user wanted: native
  engine driving PSX gameplay handlers in guest memory. Next: snapshot per-node pos for interpolation
  (the obj-pointer-keyed approach from later-84), then native cull/render submission.

## 2026-06-16 (later-84) — DIRECTION: Tomba2Engine native-engine port. Entity list RE'd + runtime-validated. Repo reorg + 72 GB cleanup.
User redirected the project: reimplement Tomba!2's **engine layer** in native C (gameplay logic STAYS
recompiled PSX in guest memory; the native engine reads entity structs from guest RAM). Repo reframed
as **Tomba2Engine**; N64Recomp-style framework/game split, boundary-first in-tree (engine/ = game,
runtime/recomp/ = common PSX platform → future psxport submodule). See CLAUDE.md, docs/engine_re.md,
plan <local-notes>/plans/fancy-tinkering-kite.md, memory [[tomba2engine-native-engine-pivot]].

### Engine RE — the entity system (Phase 0 done)
- **Entity list = two doubly-linked lists** of 0xD0 pool nodes; heads `DAT_800fb168`/`DAT_800f2624`.
  Node: render_flag@+1, state@+4, type@+0xc, **handler fn-ptr@+0x1c**, prev@+0x20, **next@+0x24**,
  pos@+0x2e/32/36. Walk = **`FUN_8007a904`**: per list, follow next@+0x24, clear +1, call handler@+0x1c(node).
  Found by searching gameplay RAM dumps for the handler address bytes (handlers are per-object fn ptrs,
  not a type-indexed table — that's why static xref found nothing).
- **Runtime-validated** (`PSXPORT_OBJLOG=1`, field scene): the cull/object path FIRES in gameplay
  (212k calls/4700 frames) — **CORRECTS the later-82 claim "cull doesn't fire"**, which was DEMO-only.
  Objects carry real 3D world positions keyed by their pool-slot pointer (type 02/03,
  e.g. obj=800fc5c0 pos=(4750,-1500,5000)). ≥2 pools observed (strides 0x88 and 0xD0).
- **Interpolation insight (supersedes the GTE-fingerprint approach):** the **object pointer is a stable
  cross-frame identity** (pool slot) with a real position — snapshot per-object pos each logic frame and
  interpolate that, instead of fingerprinting GTE transforms. This is the Phase-4 path once the native
  entity walk (Phase 1) owns the list. (wide60.c per-prim gating fix `b2de253` stays meanwhile.)

### Cleanup
tools/clean.sh (allowlist of regenerable dumps; preserves RE assets/state/bios/bin/obj) — freed ~72 GB.
Dropped a stray tracked 16 MB Beetle savestate (`hold`).

## 2026-06-16 (later-83) — wide60 reprojection: FIXED user-reported "terrible" live output (smearing + flicker + TRIPLED weapon HUD icon). Root cause = global screen-XY remap with no per-prim object gating. Tooling-proven.
User ran `PSXPORT_WIDE60=1 ./run.sh` on the later-82 reprojection code: "terrible" — stretched/smeared
polys, flicker, and the bottom weapon-icon HUD drawn DOUBLED/TRIPLED. (Diagnosed via tooling per user
directive, not by eyeballing — see the new sdbg counters.)

### ROOT CAUSE (confirmed, `PSXPORT_WIDE60_SDBG` per-frame counters in wide60_synthesize)
`wide60_build_remap` builds a **global old-SXY → interp-SXY hash table**; `wide60_synthesize` remapped
**any** captured prim vertex whose packet (x,y) hit a key — with NO notion of whether that prim is a
3D/GTE object. PSX sprites/rects/lines + CPU-projected polys are screen-space (never RTP'd), but their
coords can COLLIDE with a real 3D vertex's SXY in the table, so they got dragged to interpolated
positions. Measured on gameplay (f471+, every frame): `sprite_remap` 1–4/frame (= the tripled HUD icon),
`line_remap` 1–2, and **poly partial-remap 4–103/frame, varying wildly** (= stretched tris = smearing;
the frame-to-frame variation = flicker). The old per-prim object join (`Prim.obj` via the grid) was
computed at capture but UNUSED by the transform-based synth — that's the regression.

### FIX (wide60.c wide60_synthesize) — ALL-OR-NOTHING per primitive
- Sprites (nv==1) + lines (nv==2): **always snap**, never consult the remap (they're always 2D).
- Polygons: remap only if **EVERY** vertex resolved in the table (whole prim belongs to one matched+
  interpolated object); else snap the WHOLE prim. No mixed/partial vertices → no stretch. A CPU-projected
  2D poly won't have all corners coincide with distinct 3D SXYs → snaps. Snapped prims = 30fps fallback.
- VERIFIED by tooling: `sprite_remap=0 line_remap=0` across all 5943 frames of the headless run (was
  1–4/frame). Offline f2500 dump (`PSXPORT_WIDE60_SYNTH=2500`) unchanged-good: in-between complete (bg,
  rails, DEMO text, orbs), character at midpoint, no holes. Faithful path untouched (synth is WIDE60-only).
- **Kept as committed tooling:** `PSXPORT_WIDE60_SDBG=1` runs the synth every headless frame and prints
  per-frame remap-correctness counts (poly full/partial/none, sprite/line remaps, collisions-snapped).
- OPEN / next: user must eyeball LIVE (`PSXPORT_WIDE60=1 ./run.sh`) — the confirmed artifact sources are
  gone, but residual judder is possible since ~10–30% of 3D polys/frame fall back to snap (partial
  coverage: their object was gated by the TR-teleport gate, or they span objects). If too many 3D polys
  snap, investigate WHY coverage is partial (gate too tight? per-vertex object association needed?).

## 2026-06-16 (later-82) — wide60 60fps RE-ARCHITECTED to the per-game GTE-transform tier (user-directed; screen-space plane interp REJECTED). Native graphical-object capture + cross-frame matching VERIFIED.
**User rejected screen-space primitive interpolation** ("you are doing this blindly; reverse engineer the
game's camera and objects and interpolate those, not individual planes"; then "port the game's graphical
objects into PC native such as camera and models, then interpolate them"). Did the RE:

### RE findings (PSXPORT_W60_GTEDBG instrumentation, demo scene f2500)
- The **0x8007712C cull dispatcher does NOT fire** in these scenes — `g_current_object` stays 0. So the
  earlier "object join 92-99%" (later-57) was joining EVERYTHING to one sentinel = a single mega-blob —
  THE ROOT CAUSE of the smearing in the screen-space approach. **Falsifies the object-join premise.**
- The real object identity is the **GTE TRANSFORM**: each run of RTPS/RTPT sharing one (R = rotation
  matrix CR0-4, TR = translation CR5-7 = the model-view, camera baked in) is one object. ~34-121 distinct
  transforms/frame: identity-rotation+large-TR = world props/background; varied-rotation+small-TR clusters
  = the character (one transform per animated bone, 30-70 RTPS each); one 1535-RTPS group = the terrain.
- Camera has **identity rotation** here (2.5D); motion is in TR (e.g. TRZ 2624→2656 between f2500/f2501).
  Projection consts OFX/OFY/H constant. Local input verts (DR0/DR1) are **model-space, frame-invariant**.

### Architecture (the per-game tier, per CLAUDE.md)
Group geometry by GTE transform = native object. Cross-frame **identity = local-vertex fingerprint** (mesh
invariant; only transform moves). **Interpolate the transform** (R + TR) at t=0.5; **re-project** the local
verts through the real Beetle GTE → old-SXY→new-SXY map → remap B's captured prim vertices. Unmapped prims
(CPU-projected terrain edges / 2D HUD) snap = 30fps. Perspective-correct ⇒ no smears, no gaps.

### VERIFIED foundation (headless, gameplay f500-f5000) — `wide60.c` XObj capture
`wide60_rtp`→`xobj_rtp` groups RTPS by transform into native `XObj{R,TR,fingerprint,nrtps}`, double-buffered
A/B. **objects/frame = 24-121; cross-frame match by fingerprint = 87-94%; TR delta avg 70-940, max ~5700**
(interpolatable). 0-object frames = FMV/load (no interp). Faithful path unchanged.

### Next (this is the active work)
Reprojection: save GTE state → for each matched object write interp(R_A,R_B)/interp(TR_A,TR_B) → RTPS each
local vert → build SXY remap → restore GTE → render in-between by remapping prim verts. **The screen-space
synth (wide60_synthesize lerp, prim matcher, per-object decide, shape gate) is SUPERSEDED** — kept only
until reprojection lands, then removed.

## 2026-06-16 (later-81) — wide60 60fps: in-between SYNTHESIZER + live present, rebuilt to the user's mandated architecture. PLAYABLE windowed (awaiting user flicker check). Also: fit-to-screen 4:3 display scaling.
**User correction (a first cut that rendered the in-between INTO the game's back VRAM buffer was
rejected as "terrible").** The 60fps layer must NOT write VRAM — it sits ON TOP of the PC renderer.
Rebuilt per [[wide60-60fps-architecture]]:
- **Separate framebuffer.** Interp is rasterized into `s_interp` (gpu_native.c), NOT VRAM. A write-target
  redirect `s_fb_base` (used by `put_px_b`) points at s_interp only during synth; `sample_tex` always
  reads VRAM, so textures come from the live atlas. `gpu_w60_begin_interp` clears the target region +
  sets env; `gpu_w60_draw_poly/sprite` reuse the SAME `tri()`/`raster_sprite()` path (extracted a shared
  `raster_sprite` helper from gp0_exec). `gpu_present` split into `gpu_present_ex(do_blit)` so wide60 runs
  bookkeeping without the front blit.
- **Cadence = 1 frame behind.** Per 30fps logic frame present TWO frames: the PREVIOUS real frame (intact
  in the back VRAM buffer, blitted read-only) then the INTERP frame (s_interp). Current is held → becomes
  "previous" next frame. Stream: A, lerp(A,B), B, lerp(B,C), C… = 60fps. `wide60_present()` owns it; gated
  to windowed + animating (geom>0) + actually double-buffering (front_y flips), else one faithful present.
  Pacing: `gpu_pace_subframe(2)` twice (the shared accumulator advances one logic frame, so audio stays
  realtime); faithful path unchanged at `gpu_pace_subframe(1)`.
- **Interp = object-vertex lerp at t=0.5** (matched within a displacement gate, default 48px L1; else snap);
  UV/color/state kept from B (avoids UV-parity smear). Full list redrawn over a cleared region → background
  + HUD reproduced, no holes (verified via the offline dump).
- **Display scaling (`present_window`→`blit_src`):** fit output to the screen at fixed **4:3** with
  letterbox/pillarbox bars — never stretch 2D/FMV. Default borderless **fullscreen-desktop** ("adapt to
  screen"); `PSXPORT_WINDOWED=1` = resizable window; ESC/close quits; linear upscale filter.

### Verified
- **Faithful path byte-identical** with PSXPORT_WIDE60 off (VRAM f3000 `cmp` PASS) — the `put_px_b`→`fb()`
  and `raster_sprite` extraction are behavior-preserving.
- **Offline dump** (`PSXPORT_WIDE60_SYNTH=2500`): the interp frame in s_interp is COMPLETE (full bg, rails,
  DEMO text) with the character at the interpolated midpoint between A and B. scratch/screenshots/w60_*.png.
- Headless WIDE60 run completes (else-branch single present, no crash).

### OPEN / next (NEEDS THE USER's eyes — windowed)
- **Play + flicker check:** `PSXPORT_WIDE60=1 ./run.sh`. Watch for smoothness + ZERO flicker. Do not
  declare done on headless alone.
- Known approximations to revisit if artifacts show: per-prim clip not captured (uses full front-buffer
  clip); lines (0x40–0x5F) / fills (0x02) not captured → if a scene draws bg via those they'd be missing in
  the interp (clear-to-black shows through). The DEMO scene was complete with poly+sprite only.

## 2026-06-16 (later-80) — wide60 60fps: full primitive capture + cross-frame MATCHER built & displacement-verified (the handoff's stated next milestone). Foundation for the in-between synthesizer.
Continued the 60fps tier from later-57 (object→primitive join). Built milestones 3 (full capture) and
the matcher, all in `runtime/recomp/wide60.c` + read-only taps in `gpu_native.c gp0_exec` (gated PSXPORT_WIDE60).

### Mechanism
- **Full primitive capture (PrimFrame A/B).** Every completed GP0 poly (`wide60_cap_poly`) and
  sprite/rect (`wide60_cap_sprite`) is teed into the current frame's `Prim[]` buffer: op, nv, per-vertex
  (x,y packet-coords / u,v / rgb), E5 draw-offset, texpage, color-mode, CLUT, and the joined obj id
  (`grid_get` of the lead vertex; sprites/lines = obj 0 → snap). Double-buffered A(prev)/B(cur), swapped
  in `wide60_frame_commit`. PRIM_MAX=8192 (measured ≤~1400/frame, with headroom; overflow flagged).
- **Matcher (`wide60_match`, run per logic frame).** Key = **obj + op + texpage + color-mode + CLUT**;
  greedy "first unused A prim with equal key, in A draw order" → pairs the k-th same-key prim in B with
  the k-th in A. `s_match[i]` = A index or −1 (snap). obj 0 never matches.

### Verified (headless, gameplay f500–f5000)
- **poly-join 92–99%** (reconfirms later-57). **lerp match 49–96%** of all prims (the rest = sprites/HUD/
  terrain that snap by design + joined polys whose mesh genuinely changed). **Median cross-frame
  displacement of matched pairs = 2–9 px** — i.e. matches are genuinely the same primitive one logic
  frame apart, exactly what's interpolatable. (p90 saturates the 15px histogram ceiling = the fast-motion
  tail the synth's displacement gate will snap.)
- **KEY-CHOICE dead end (don't revisit):** dropping texpage/CLUT and keying on obj+op alone (relying on
  draw-order ordinal) is WORSE — median displacement jumps to 15+px because per-tri LOD/cull shifts the
  ordinals. The "CLUT high halves alternate by parity, don't key on them" warning applied to the OBJECT-LESS
  offline fingerprint matcher; with obj as the anchor, texpage/CLUT *refine* the match (verified by the
  displacement histogram). Richer key kept.
- **Faithful path safe:** VRAM at f3000 **byte-identical** with PSXPORT_WIDE60 ON vs OFF (`cmp` PASS) —
  capture/match only READS, never touches s_vram.

### Next (remaining, needs the USER's eyes for flicker)
The in-between SYNTHESIZER + present ordering: replay B's captured list rasterizing lerp(A,B,0.5) for
matched prims (snap the rest, plus a displacement gate), skip VRAM-upload ops on in-betweens, present
in-between then B, host-pace 60 Hz. No-flicker is TOP priority — do NOT declare done on headless alone.

## 2026-06-16 (later-79) — FIXED (user-verified): "ingame music plays over the dialog" in the prologue. Cause: the looping ingame-music XA (chan4) is started by the gameplay state machine, which on real HW only fires AFTER the CD-paced scene load — by then the dialog owns the audio. Our INSTANT CD reads fire it during the dialog, so it overlaps the dialog tone. Fix (per user directive — keep instant CD, mod the CD-speed-dependent behavior in PC code): suppress the looping ingame-music XA while a dialog tone is the current song, resume it after. Identifications corrected: **song 4-7 = sequenced dialog tones** (user-ID: 4=regular, 6=worry), **chan4 XA = ingame/area music**, **chan22 XA = dialog voice**, **chan7 XA = prologue narration**. FOLLOW-UPS FIXED in later-79b/79c: the resumed music starting at full volume (now fades in via the game's own CD-volume ramp) and a ~1-frame music "blip" at the dialog start (later-79b cut synchronously; later-79c found the fade-in still leaked one FULL-volume frame and fixed it by flushing the SPU CDVOL register on the snap — USER-VERIFIED PERFECT). STILL OPEN: fade-OUT entering indoors is abrupt (not yet captured — see later-79c).

### later-79 CORRECTION of the mid-investigation notes below
The measurement notes in this entry are accurate as DATA but two interpretations in them are WRONG and are corrected here:
1. "song 4 = field BGM that plays through the dialog" — FALSE. song 4 is the **regular dialog tone**; it's *supposed* to play during the dialog. The thing wrongly overlapping it is the **chan4 XA ingame music**, a different SPU path.
2. "holding DAT_801fe0e0 / the gate fix is refuted" — the gate IS the mechanism: the ingame music is started by FUN_8005f2f0/FUN_800624b4 (gameplay handlers) at the FUN_80074f24 call (ram_f1000_all.c:40267/41983), and those handlers are held by `if (DAT_801fe0e0 != 0) … return`. Instant loads let the handler reach the music-start during a gate gap. (We did NOT implement a raw gate-hold — see the FIX — but the gate was the right lever to reason about.)
3. The "model CD latency" fix was **rejected by the user**: instant CD reads are a required PC feature (PC disk speed, not CD speed); every CD-speed-dependent function gets MODDED in PC code instead.

### later-79 — oracle A/B of the prologue→fisherman timeline (the decisive measurement)
Drove BOTH cores to the prologue narration and sampled `0x800bed80` (current song) + `0x801fe0e0`
(voice task-2 gate) frame-by-frame. **Oracle menu nav fix:** REPL `tap` does NOT register at the
title menu, but **held press works** — `press Cross / run 30 / release Cross` selected NewGame then
StartGame (updates docs/diff-driver.md "known gap"). Reusable oracle state saved at the prologue:
`scratch/state/prologue.sav`.

ORACLE timeline (ground truth):
- prologue narration (chan7): gate `0x801fe0e0`=1 **continuously** ~f6360→~f8460 (~2100 frames), song=FFFF.
- narration ends → gate→0 at ~f8520, song still FFFF.
- **~300-frame pause, gate stays 0**, then song→**04** at ~f8820 (field BGM starts WHILE gate=0).
- fisherman "Aaaaahhh!" scream + dialog: gate **toggles** 1/0 per voice line from ~f9300 onward
  (f9300-9360 up, 9420-9660 down, 9720-10080 up, …) — and **song stays 04 the entire time**.

=> In the real game the sequenced field BGM (song 4) **correctly plays underneath the whole fisherman
dialog**; it is NOT gated off during the cutscene. song 4 even starts BEFORE the first dialog line, in
a gate-0 window after the narration. **This REFUTES later-78e's plan** (hold `DAT_801fe0e0` nonzero to
delay song 4 — that would make the port DIVERGE from the oracle). song 4 is not gated by the voice task.

PORT timeline (same scene, native frames): narration gate=2 ~f267→~f1360 (~1093f, ≈HALF the oracle's),
then at f1390 gate→0 and **within 4 frames** song4 + chan4-loop fire together (f1394), chan22 scream
~f1640. The oracle's ~300f post-narration pause is collapsed to ~4f, so song4 and the cutscene start
simultaneously — what the user hears as "song 4 during the cutscene."

### later-79 ROOT CAUSE (confirmed with PSXPORT_CD_VERBOSE)
At narration-end the game loads the fisherman scene: a burst of large `FUN_8001db8c`/`FUN_8001dc40`
loadfiles — 285096 + 188416 + 231424 + 448512 B … (~1.4 MB) — issued right at f1390. The port serves
these **synchronously and instantly** (cd_override.c `ov_cd_loadfile`/`ov_cd_async_read`), so the whole
load finishes in ~4 frames; the game then starts song4 the instant the load-done flag is set. In the
oracle the same ~1.4 MB takes real CD time (~5 s ≈ ~300 frames at 2× ≈ the measured f8520→f8820 gap).
**The game uses CD load latency as implicit pacing; the port's zero-latency CD I/O removes it.** This is
an architectural consequence of the "no CD emulation / synchronous reads" design, not a localized bug.
(The narration being ~2× too short is a related, separate pacing gap — likely XA/stream consumption rate
— and is NOT yet root-caused; the post-narration collapse above is the dominant audible effect.)

### later-79 — RE of the trigger + the implemented fix
- **What starts the ingame music:** `FUN_80074f24(DAT_800bf870)` (→ `FUN_800750d8`/`FUN_8001d364` → `FUN_8001d2a8(chan4,[84515..97979],loop=1)`), called from the gameplay handlers `FUN_8005f2f0` (case 6, :40267) and `FUN_800624b4` (:41983). Those handlers are held by `if (DAT_801fe0e0 != 0) { FUN_8001cf2c(); return; }`. The dialog routine `FUN_801464c0` (overlay, 0x80146xxx — INTERPRETED, not in the Ghidra decomp; disassembled live from a RAM dump with capstone) is just the **voice player** (line index → channel → `FUN_8001d2a8`); it does not stop the music.
- **Oracle ground truth (cold-boot WAV; savestate XA does NOT replay):** narration (chan7) → **~8 s silence** (real CD load) → dialog tone + voice, with **NO ingame music** (chan4 cross-corr ≈0.087 vs dialog-tone 0.31; the 8 s silence rules out a looping chan4). So the ingame music is correctly absent during the prologue dialog. `scratch/wav/oracle_scene.wav`, segments in `scratch/wav/music/`.
- **The mod (cd_override.c + xa_stream.c + native_boot.c):** a looping XA clip = ingame/area music; one-shot clips = voice/narration. While a dialog tone is the current song (`0x800bed80` in 4..7), `ov_voice_play` defers a looping clip (remembers it, doesn't start it / doesn't set the gate) and `xa_dialog_coord` (per-frame, from native_scheduler_step) stops any looping XA that's sounding; once the dialog tone clears and the XA stream is free it resumes the remembered ingame-music clip. Voice clips are untouched. USER-VERIFIED correct (dialog now = tone + voice only; ingame music returns after).
- **Music identification library** (for future audio bugs): `scratch/wav/music/` — `seq_00..13.wav` (sequenced songs, rendered via new REPL `bgm N` + `wav PATH`) and `xa_chan{4,7,22}_*.wav` (XA tracks, via new REPL `xadump chan lba PATH`). chan4=ingame music, song4=regular dialog, song6=worry dialog (user-identified).
- **OPEN (next):** resumed ingame music starts at full volume; the real game fades it in from 0 (another instant-CD timing casualty — the volume ramp). Investigate the XA/CD volume fade and mod it PC-side. → DONE in later-79b.

### later-79b — fade-in + the 1-frame "blip", both PC-side (register-verified)
Two follow-ups from later-79, fixed in the same instant-CD-mod spirit.

**The fade mechanism (RE'd):** the game fades XA loudness via the **CD-volume** SpuCommonAttr (mask
`0xc0` = CDVOLL/CDVOLR → SPU regs 0x1B0/0x1B2 → Beetle `spu.c` scales the XA by `CDVol>>15`, spu.c:1400).
Per-frame `FUN_80075824(&DAT_800be1f8)` ramps a fade **current** `DAT_800be224` toward a **target**
`DAT_800be222` by 0x100/frame (×2 if `DAT_1f800137==2`), scaled by master `DAT_800be220`; `FUN_80075cec`
sets the target (neg arg snaps both); the scene-transition fades are `FUN_80026470` (out, target 0) /
`FUN_800264bc` (in: snap to ~0, target 0x47ff). Verified live (PSXPORT_CDVOL_DBG, new gated log in spu.c):
in gameplay CDVol sits flat at 25484 (= master, current `DAT_800be224`=0x7fff), i.e. the ramp runs but is
already at target — so a (re)started clip plays at full, no fade. The dialog VOICE (chan22) is the SAME XA
stream and is ALSO scaled by CDVol, so the fade may only be applied when the music is the sole XA user.

**Fade-in fix (cd_override.c `music_fade_in`):** on ingame-music (re)start (`ov_voice_play` loop path +
`xa_dialog_coord` resume) snap the fade-current `DAT_800be224`=0 and leave the target; the game's own
per-frame ramp climbs it back (~128f ≈ 2.1 s) = a faithful fade-in using the vanilla mechanism/rate.
Verified: at chan4 start CDVol drops 25480→**198** (first ramp step from 0) and climbs.

**1-frame "blip" fix (native_boot.c `ov_bgm_start` → cd_override.c `xa_music_cut_if_dialog`):** the
per-frame `xa_dialog_coord` stops the looping music one frame AFTER the audio is mixed (frame loop:
`rc0(0x800788ac)` audio mix at line 378 runs BEFORE `native_scheduler_step`/`xa_dialog_coord` at 379-380),
so a dialog-tone start leaked ~1 frame of music. Now the BGM-start override (always-on; logging still
gated by PSXPORT_BGMDBG) cuts the looping XA **synchronously** the instant a dialog-tone song (4-7) is
written. Combined with the fade-in, any residual sub-frame leak is at ~0.6 % volume (inaudible).

**Not a regression:** the "house is on fire" (song 6) dialog stalls headless at `DAT_801fe0e0`=2 — but
**baseline (stashed) stalls identically**, so it's the pre-existing open BGM-timing bug (commit a42289d),
not from this change. The post-dialog resume couldn't be observed headless (dialog stall); the resume uses
the identical `music_fade_in` path. Pending: user ear-check via `./run.sh`.

### later-79c — later-79b fade-in was INCOMPLETE: it still leaked one FULL-volume frame (user ear-confirmed). Root cause found via frame-by-frame trace; fixed by flushing the SPU CDVOL register on the snap. Now USER-VERIFIED PERFECT.
later-79b snapped only the *guest* fade-current `DAT_800be224`=0. But the game's per-frame ramp
`FUN_80075824` writes the SPU CDVOL **register** (0x1F801DB0/DB2 → Beetle spu.c case 0x30/0x32, `CDVol[]`)
from `cur` exactly **once per frame**, and on the music-(re)start frame it had ALREADY run with the old
(full) `cur` *before* `music_fade_in` fired — so the started XA mixed at full for that one frame, then
dropped and climbed. That leading full frame = the user's "1-frame blip" and "starts loud then drops to
zero then climbs."
- **Diagnosis tooling (gated, kept):** `xa_audio_trace()` (cd_override.c, PSXPORT_XA_DBG) logs the fade
  vars + XA lifecycle per frame at `pre`/`post`/`coord` points (native_boot.c, around `rc0(0x800788ac)`);
  `[xamix]` (spu.c, PSXPORT_XAMIX_DBG, env cached) logs the CDVOL actually applied to live XA samples.
  The trace nailed it: at chan4 start `[xamix] CDVolL=25480` (full) for the start frame, then 198/198…
- **Fix (cd_override.c `music_fade_in`):** in addition to `DAT_800be224`=0, write the SPU CDVOL register
  to 0 directly — `mem_w16(0x1f801db0,0); mem_w16(0x1f801db2,0)` (routes io_write→spu_write→SPU_Write).
  This mutes the start frame's mix; next frame the game's ramp rewrites CDVOL from cur=0 and climbs.
  `ov_voice_play` runs mid-tick (after `FUN_80075824`), so the register write lands before that frame's
  SPU mix. **Verified:** start frame now mixes at `CDVolL=0` (was 25480), then 198/795/1591… climbing;
  **user ear-confirmed "absolutely perfect, to the last minute detail."**
- **STILL OPEN — fade-OUT (entering indoors) is abrupt.** Not yet captured: in both traces the music
  stayed at full through the transition and the game never set CDVOL target→0, then the stream was
  STOPped at full `cur` and immediately restarted (loop/scene re-trigger), so no fade-out path was
  exercised. Need a clean "enter indoors" capture to see whether the game arms `FUN_80026470` (target 0)
  and the stream is killed before the ramp reaches 0 (→ defer `xa_stream_stop` until cur≈0), or the game
  never fades here at all. Don't fix blind.

## 2026-06-16 (later 78) — FIXED later-77: in-game XA-ADPCM streaming implemented natively (prologue BGM + voice now audible, WAV-confirmed). Fisherman-scream "first note repeats / cutscene won't advance" ROOT-CAUSED to FUN_8001cfc8's faked GetlocL position wait; interim stopgap in place, native engine-port planned.
**New native subsystem `runtime/recomp/xa_stream.c`** (in build: added to run.sh + tools/build_port.sh).
Decodes CD-XA from the ReadS-streamed sectors and feeds the SPU CD-audio input via
`CDC_GetCDAudioSample()` (Beetle spu.c calls it per 44.1kHz sample, scaled by the game's `CDVol`,
gated on `SPUControl` bit0 — both game-set). The silence stub that used to live in spu_beetle.c is
gone; xa_stream.c owns that symbol now. Decode is **pull-driven** (decode-on-consumption → self-paces
to realtime, advances the disc LBA exactly as audio is consumed). Resamples XA 37800/18900 → 44100 with
a fractional phase accumulator + linear interp. `xa_decode_sector()` (mednafen-parity) is reused from
native_fmv.c. Debug: `PSXPORT_XA_DBG=1` (events) / `=2` (per-sector). `PSXPORT_CDCMD_DBG=1` logs both
CD-command wrappers.

### How the game drives in-game XA (observed via PSXPORT_CDCMD_DBG, both wrappers)
TWO CD-command wrappers; cutscene XA uses the engine-streaming one:
- libcd `CdControl` = `FUN_8008AC34` → `ov_cd_command`. Boot/menu (Setmode 0x80/0xC0, a short clip).
- **engine streaming = `FUN_8001CE90` → `ov_cd_cmd_stream`. Cutscene voice/BGM goes HERE:**
  Setmode **0xC8** (Speed|ADPCM|SF-filter), Setfilter file=1/chan=N, Setloc→LBA, ReadS. Both wrappers
  now route Setmode/Setfilter/Setloc/ReadS/Pause to xa_stream. Prologue narration = file1/chan7 @ LBA
  66515; fisherman-scene voice clips = file1/chan4 @ 84515, chan22 @ 12771, etc. (interleaved, every
  8th sector is the selected channel). VERIFIED: prologue WAV has real BGM+voice from ~t=8s (RMS
  1700-4400) where it was silent before; xa_stream decodes continuously, LBA advances, no loop in the
  engine itself.

### Fisherman "AAAGH repeats / cutscene stuck" — ROOT CAUSE (RE'd FUN_8001cfc8, the engine streaming reader, task slot 2)
`FUN_8001cfc8` plays a clip [start..end] on file1/chan(task+0x66): Setloc start, ReadS, then **polls
GetlocL (cmd 0x10) and yields each frame WHILE head <= task+0x58 (clip end)**; when head > end it pauses
(`FUN_8001ce90(9)`) + `DAT_800be0e4=0` + ends the task (advancing the cutscene), OR if task+0x67==1 loops
the clip. Our `ov_cd_cmd_stream` faked GetlocL as a STATIC position (clip start) → `head <= end` always
true → the wait never ends → cutscene never advances, clip runs on / re-triggers ("first note over and
over"). The user nailed it: that voice line gates cutscene advancement.
- Task params (RE'd): `FUN_8001d2a8(chan, start_lba, end_lba, flags)` writes them DIRECTLY into the
  slot-2 task struct — `DAT_801fe134/138/146/147` ARE task2 (`0x801fe0e0`) `+0x54`(start)/`+0x58`(end)/
  `+0x66`(chan)/`+0x67`(loop). Helpers: `FUN_8008a00c` LBA→BCD MSF (msf=lba+150); `FUN_8008a110` BCD
  MSF→LBA(−150); `FUN_8001ceb0(0xC8,..)` ensures Setmode 0xC8; `FUN_8001cf00(0/1)` CD-event enable.
- **Interim stopgap (committed):** `ov_cd_cmd_stream` GetlocL now reports `xa_stream_play_lba()` (the
  native engine's advancing read head) while streaming → the wait terminates, clips play once and the
  scene advances. Marked `// STOPGAP` in cd_override.c.

### later-78e — XA audio ORACLE-VERIFIED; OPEN: gameplay BGM starts during the fisherman cutscene
XA in-game audio now works AND is oracle-verified: drove the oracle (cold boot, Start to skip FMVs,
Cross to select StartGame) to the prologue narration ("Tomba is living peacefully…") and captured its
audio (scratch/wav/oracle_cold.wav, narration ~263-291s). Extracted both narration segments and ran
tools/audio_differ/compare.py diff vs the port (scratch/wav/port_cdfix.wav, narration ~15-23s):
**corr=0.954, RMS ratio 0.894** — the music+VO content MATCHES the oracle. (Two bugs fixed to get here:
mono-XA buffer overflow [later-78d], and the dropped CD->SPU enable [FUN_8001cf00(1)] that left
SPUControl bit0=0 so the decoded XA was discarded by spu.c.) User confirms: fisherman dialog + other
audio fixed.

OPEN (user report): the **sequenced gameplay BGM starts too early — during the fisherman cutscene**,
should wait until it ends. Observed in port: `FUN_80074BF8(idx=4)` fires at ~f1554 (0x800bed80→4) while
the cutscene's XA (chan4 loop music + chan22 voice lines) is still playing. The gameplay-vs-cutscene
gate is `DAT_801fe0e0 != 0` (voice-task-alive): handlers FUN_8005f2f0 (case1 @40208) and FUN_800624b4
(case2 @41958) do `if (DAT_801fe0e0 != 0) { FUN_8001cf2c(); return; }` — i.e. don't run gameplay while a
voice clip plays. Our native voice port (later-78c) sets DAT_801fe0e0=2 only WHILE a clip is mid-play and
0 otherwise, so between clips / before the first clip the gameplay slips through and starts song 4.
User's framing: "cutscenes stop gameplay, maybe that stop didn't work."
NEXT (needs oracle A/B): watch when the oracle writes 0x800bed80 (song 4) relative to the cutscene, and
what keeps gameplay blocked across the WHOLE cutscene in the real game (candidate: the chan4 XA loop keeps
task slot 2 alive continuously; our single-ring player drops it when a chan22 voice line replaces chan4
and doesn't resume → DAT_801fe0e0 gaps). Likely fix: hold the cutscene "voice/stream active" state for the
whole cutscene (don't 0 it between lines), or resume the looped chan4 music after each voice line. Confirm
the exact gating against the oracle before changing — do NOT guess (user rule: verify vs oracle).

### later-78d — BUG: mono XA overflowed the decode buffers (voice was silent/garbled)
User: no voice in the WAV / "sounds the same as when XA was broken". Per-sector log showed the
chan22 voice = **mono 18900Hz → n=4032 frames/sector** (mono puts ALL units on one channel: 18*8*28),
but the decode buffers were sized for 2016 STEREO frames: `xa_decode_sector`'s internal
`ch[2][2016+8]` and the callers' `pcm[2016*2]` / `xa_pcm[2016*2]`. The 4032-frame mono sector
overflowed them → corrupted state (seen as `wr=678508593` garbage right after a mono sector) → no
coherent voice. FIX: size all three for the mono max (4032): `ch[2][4032+8]`, `pcm[4032*2]`,
`xa_pcm[4032*2]` (native_fmv.c + xa_stream.c). After: wr increments 4032,8064,12096… cleanly.
(Stereo 37800 FMV audio never hit this; mono voice did.)

### later-78c — FIXED: voice-streaming engine ported native (drop the FUN_8001cfc8 task)
Implemented the native port. Verified interactively (drive like a player → prologue → hands-off):
voice lines now ADVANCE one-by-one (chan22 clips 12771→15875+, each new line starts fresh) and
task-2 state tracks clip completion, instead of sticking on the first clip forever.
- ROOT CAUSE confirmed live: read task-2 fields mid-stall — they said clip = LBA 13923..14243 chan22,
  but xa_stream was still playing the OLD clip (12771). The dialog re-registered slot 2 with a new
  clip, but the native scheduler resumed the STALE FUN_8001cfc8 coroutine (it keys fresh-vs-resume on
  g_task_started, not on the entry/re-register), so the new clip's Setloc/ReadS never fired → old
  stream's head never reached the new end (14243) → `while (DAT_801fe0e0 != 0)` hung → "AAAGH repeats".
- FIX (engine→PC): override `FUN_8001d2a8` (the voice/BGM clip player all by-index APIs funnel through)
  → `xa_stream_play(chan,start,end,loop)` (idempotent per clip → re-issuing the same line can't reset
  the ring); override `FUN_8001cf2c` → `xa_stream_stop`. We OWN task slot 2: `native_scheduler_step`
  skips the now-unused FUN_8001cfc8 coroutine and writes the task-2 state byte (2 while
  `xa_stream_voice_busy()`, 0 when the clip's head passes its end) so the cutscene wait advances.
  Clip end/loop handled in xa_decode_next_sector. GetlocL stopgap reverted (that poll no longer runs).
- Slot 2 is exclusively FUN_8001cfc8, registered ONLY by FUN_8001d2a8 (verified) → safe to own.
- NOTE: XA is a single stream (one channel to SPU at a time) — BGM (chan4, loop) and voice (chan22)
  can't sound simultaneously; the game interleaves them by switching the clip, which our single-ring
  player matches. Pending user ear-check on the captured WAV (scratch/wav/fish_native_clean.wav).

### later-78b — the dialog/voice chain RE'd (stopgap did NOT fix fisherman; user confirms)
The voice line gates cutscene advancement via the **voice TASK**, not a flag:
- Dialog/cutscene script (recomp, e.g. FUN_80043a40) plays a voice line by index via the engine
  voice APIs: **FUN_8001d71c / FUN_8001d364 / FUN_8001d41c / FUN_8001d0e0** → all funnel to
  **FUN_8001d2a8(chan, start_lba, end_lba, flags)** which sets task-2 fields + `DAT_800be0e4` and
  (re)registers task slot 2 (FUN_80051f14 → entry FUN_8001cfc8, state=2). Clip channel/LBA come from
  tables PTR_DAT_8001005c.. + `_DAT_1f800220/224`.
- **The cutscene waits `while (DAT_801fe0e0 != 0)`** (ram_f1000_all.c:24287, 40208, 41958).
  `DAT_801fe0e0` = task slot 2's state byte (0x801fe000 + 2*0x70). It advances ONLY when the voice
  task ENDS (state→0). FUN_8001cf2c = stop voice (DAT_800be0e4=0 + CD pause).
- Suspect (not yet reproduced headless): our native scheduler keys fresh-vs-resume on `g_task_started`,
  NOT on the task ENTRY changing. FUN_80051f14 re-registers slot 2 (state=2, NEW entry) on each
  voice-play; if the slot's previous coroutine context is still "started", the scheduler RESUMES the old
  pc instead of entering the new entry → wrong clip / never-ends → cutscene stuck / "first note repeats".
- REPRO GAP: fast-forward nav (tap x / tap start) manually advances the dialog and MASKS the bug; only 4
  XA STARTs seen headless, clips progress. Need to reach the fisherman scream and let it AUTO-advance
  (no input). Ask the user how to reliably trigger it, or compare vs oracle at that exact beat.

### NEXT — port the voice-streaming engine to native C (engine→PC; gameplay/script stays on recomp)
Port FUN_8001d2a8 (+ the by-index APIs) and FUN_8001cf2c to drive xa_stream DIRECTLY and DROP the task-2
coroutine FUN_8001cfc8 entirely (no scheduler interaction → no re-register fragility, no GetlocL fake):
- xa_stream: add play(chan,start,end,loop) [idempotent if same clip already active → no ring reset/
  repeat] + busy() [1 while play_lba<=end & active].
- Override FUN_8001d2a8 → xa_stream_play; FUN_8001cf2c → xa_stream_stop. DROP spawning task slot 2.
- The cutscene polls `DAT_801fe0e0 != 0` (task-2 state): since we no longer run task 2, MAINTAIN that
  byte natively = (xa_stream_busy() ? running : 0) so the existing recomp wait still works. (Set it on
  play, clear it when the clip finishes.) Verify clip plays once + scene auto-advances; compare vs oracle.
Old single-function plan (kept for reference):
Per the locked architecture (engine on PC, gameplay on recomp/interp), the streaming reader is ENGINE
and should be native, eliminating the faked GetlocL poll. Plan:
1. Add a **native task-stepper** path to `native_scheduler_step` (native_boot.c): when a slot's entry
   is a known native engine task (0x8001cfc8), call a native stepper instead of `rec_coro_run`; treat
   its return as YIELD (set task state 1, re-armed by FUN_800506d0) or DONE (state 0).
2. Native `cd_stream_task` state machine using xa_stream: read chan/start/end/loop from the task
   struct; setfilter(1,chan)+setloc(start)+start; each frame check abort (`DAT_800be0e4 & 0x10`) and
   `xa play_lba > end` → done (pause, `DAT_800be0e4=0`, end) or loop (restart). Add a by-LBA
   `xa_stream_setloc` variant (we have the LBA, not BCD). Skip the PSX CD-event plumbing (FUN_8001cf00
   etc.) — xa_stream is self-contained — but replicate `task+0x6f=2` and the DAT_800be0e4 side effects.
3. Verify parity vs the stopgap build (clip plays once, scene advances) and vs the oracle.

## 2026-06-16 (later 77) — ROOT CAUSE (oracle-confirmed): the port has NO in-game XA-ADPCM (CD-streamed) audio. That's BOTH the opening-cutscene BGM AND the missing voice acting. Sequenced BGM (gameplay) works; XA does not.
Got the oracle to the SAME prologue scene (cold boot + tap Start/Cross navigates New Game; the stale
title.sav does NOT take input — its frontio/SIO state is stale, cold boot is the reliable path) and
diffed: at the prologue narration the **oracle ALSO has song=0xFF / no active libsnd slot** (tools/bgm.py
on scratch/bin/orc_prologue.bin). So the prologue's audio is NOT sequenced → it's **XA-ADPCM CD audio**.
- `runtime/recomp/cdc_native.c`: CD `SetFilter`(0x0D)/`Play`(0x03)/`Setmode`(0x0E) just ACK; `ReadN/ReadS`
  only `load_sector()` data files. There is **no XA-ADPCM decode → SPU CD-audio input** path at all.
  `disc.c` even says "(XA/STR) is a later front-end concern." FMVs get XA via native_fmv.c, but in-game
  streamed audio (cutscene BGM + dialog voice) is unimplemented.
- So the user's two symptoms — opening-cutscene "Tomba is living peacefully" BGM missing AND dialog voice
  acting missing — are the SAME gap: in-game XA streaming.

### Oracle navigation FIXED (for the tooling): cold boot, not title.sav
Injected input DOES reach the emulated pad (verified: PSXPORT_INPUTDBG in vendor input.c logs
`player0 type=1 buttons=…`). The stale title.sav just ignores it. Cold-boot + tap Start (skip FMVs) +
Cross navigates to New Game reliably. main.cpp watchdog disarmed under -repl so the oracle idles at the
prompt. tools/drive.py drives it.

### NEXT — implement in-game XA-ADPCM streaming (the fix)
Decode XA-ADPCM (Mode2/Form2, 2324B, subheader submode/coding at raw[18/19] — framing already noted in
disc.c) from the CD sectors the game ReadS-streams with the XA filter set (CdlSetfilter + Setmode XA
bit), and mix into the SPU's CD-audio input (SPU CD volume / the mednafen spu.c CD input path used for
CDDA/XA). Gate on the SetFilter/file+channel. Verify vs the oracle at the prologue (audible BGM + voice).

## 2026-06-16 (later 76) — Missing BGM PINNED to the OPENING PROLOGUE cutscene (post-New-Game narration). Its BGM-start never fires in our port (zero writes to the song byte 0x800bed80 across the whole prologue); the oracle holds song 4 throughout. Title=correctly silent; gameplay BGM (song 2/3) works. Built interactive driver + BGM inspector tools; oracle now drivable (watchdog fix).
User ground truth (corrected this session): the **title/menu** has NO BGM and that's CORRECT (the user
had confused it with the unimplemented memory-card/Load page). The **gameplay** field BGM (song 2/3,
FUN_80025588 via stage handlers) WORKS. The bug is the **opening prologue cutscene** that plays right
after New Game (narration: "Tomba is living peacefully…", "Tabby disappeared", "Is she safe?", over
village→cliff→fisherman scenes). It must have BGM and ours is silent.

### Evidence (interactive, clean navigation — NOT forced-Start spam)
- Drove the native port like a player via tools/drive.py (tap x to confirm NewGame→StartGame, then
  hands-off). Watched 0x800bed78 (sound-cmd queue count) + 0x800bed80 (current song): across the whole
  prologue, **0x800bed80 is NEVER written** (stays 0xFF). Only volume bytes 0x800bed7c-7f and the
  0x800bed82 flag are touched. So the prologue's BGM-start (FUN_80074BF8) is never reached.
- Oracle reference = scratch/state/newgame.sav (the fisherman cliff scene, part of the same prologue):
  0x800bed80 = **4**, held steady 600+ frames with NO re-writes → the oracle starts song 4 once at
  prologue start and sustains it. (tools/drive.py oracle + tools/bgm.py.)
- Earlier FORCE_BUTTONS=FFF7 runs DID fire song 4 at f166 — that was an artifact of Start-spam advancing
  scene state differently (Start in gameplay = pause = stops BGM). Disregard those; clean nav is the
  truth: prologue BGM never triggers.

### Mechanism recap (still true, later-75)
BGM machinery works: FUN_80074BF8(idx) sets 0x800bed80 + SsSeqPlay; SsSeqCalled ticks; SEQ→SPU produces
audio. So this is purely a TRIGGER problem: the prologue cutscene never issues its "play song 4" command.

### CORRECTION (user): the narration cards are a DISTINCT silent scene — song 4 is a SEPARATE scene
My earlier "song 4 starts late" was WRONG (user corrected). What happened: I let the silent narration
("Tomba is living peacefully" / "kidnapped?" cards) run ~39 s; it then ADVANCED into the *separate*
fisherman/Tomba-in-tree scene, which correctly plays song 4. Those are different scenes. The narration
cards are their OWN scene and must have their OWN BGM (NOT song 4) — and it's missing/silent. Do not
conflate the two: the fisherman BGM (song 4) working says nothing about the narration cards' BGM.
- The song-4 trigger FUN_80074590(0x72) lives at 0x8011a120 — but that overlay is NOT loaded during the
  narration (0x8011a118 = zeros in scratch/bin/native_prologue.bin); it loads with the game scene at f1174.
- So either (a) the real game starts the narration's BGM via a DIFFERENT (earlier) trigger we don't fire,
  or (b) our narration phase is far too long / stalled (~39 s) and runs before the map+BGM load, whereas
  in the oracle the map/BGM is up during the (shorter) narration. Need the oracle AT the narration cards
  to decide — and that's blocked on oracle menu navigation (below).

### BLOCKER to resolve first: oracle menu navigation
Injected input (REPL tap/press, g_repl_buttons set & confirmed) does NOT reach the game in the oracle:
the title menu cursor (0x801fe068) doesn't move and the candidate pad buffer (0x800bf4f8) stays 0xFFFF.
Likely the OpenBIOS pad-poll path isn't seeing InputStateCb (or the game's real pad buffer is elsewhere).
Until fixed, can't drive the oracle from title→NewGame to the narration for a clean diff. Fixing this is
the highest-leverage next step (unblocks "always compare vs oracle"). Workaround refs: newgame.sav is
POST-narration only.

### NEXT — find + port the prologue/narration BGM trigger (compare vs oracle)
The prologue scene loads (narration renders) but its BGM-start isn't issued. Candidate: the prologue
map/scene-load BGM selector or the cutscene/event script's music cue runs in the oracle but is skipped
by our native boot/scheduler (native_boot.c hand-codes the frame loop + replaces FUN_80051e60). Find in
the oracle exactly what calls FUN_80074BF8 for song 4 at prologue start (need the oracle AT prologue
start — blocked: oracle title-menu input via REPL tap does NOT navigate the menu, cause TBD; cold-boot
through FMVs is slow). Then check whether our native scheduler runs that code path / its precondition.

### Tooling built/fixed this session (use these; extend as needed)
- **tools/drive.py** — persistent INTERACTIVE driver for native|oracle via a FIFO (O_RDWR keeps it open).
  `start [native|oracle] [headed] [ENV=val…]`, `send "cmd" …`, `out [N]`, `stop`. Drive like a player,
  observe between commands. Native: FMVs off + BGMDBG by default. `headed` opens a window (SDL; needs a
  display — windowing in this sandbox didn't show for the user, headless is the norm).
- **tools/bgm.py** — BGM/libsnd state inspector for ANY 2MB RAM dump (oracle snapshots OR our dumps):
  `dump`/`slots`/`callers`. The reliable way to diff BGM state across cores/scenes.
- **native REPL**: added `dumpram <path>`; fixed `shot`/`dumpram` path truncation (was %31s).
- **PSXPORT_FORCE_STOP_AT=N** (pad_input.c): cease forced input at frame N (reach a scene via Start, then
  go hands-off so Start-pause doesn't poison the capture).
- **Oracle watchdog fix** (main.cpp): with -repl, the global idle SIGALRM watchdog killed the process at
  the blocking prompt — now disarmed for -repl (run/tap self-arm). The oracle is now interactively drivable.
- Gotcha: oracle REPL tap/press does NOT move the title menu (input not reaching it); root cause TBD.

## 2026-06-16 (later 75) — BGM: later-74 FALSIFIED. The libsnd sequencer ACTIVATES BGM, the read pointer ADVANCES, and it PRODUCES AUDIO (WAV-confirmed). Not "frozen/never activated." The real issue is SCENE-SPECIFIC: some scenes' BGM is started+sustained (fisherman intro = audible), others (menu / steady gameplay) are not started or get stopped. Bug moved upstream to scene/stage BGM-trigger logic, NOT the SPU/sequencer.
**later-74's whole premise is wrong** and so is memory [[track-game-state-not-output]]'s "songs OPEN but never ACTIVATED." Evidence, all from the native port (PSXPORT_BGMDBG diag in native_boot.c: overrides on FUN_80074BF8/FUN_80074E48 + a per-frame slot-tick probe):

### How BGM actually works here (RE'd this session)
- **FUN_80074BF8(idx)** = "play BGM #idx": stores idx to the current-song byte **0x800bed80**, looks up
  seq table @0x800b6e68 (idx*8 → {seq_no, loopcount}), calls **SsSeqPlay** (public 0x80090560 → worker
  0x800905e0; sets the per-seq struct ptrs + **+0x98 bit0**). idx 0xFF = "no song".
- **FUN_80074E48** = "stop current BGM": if 0x800bed80 != -1, SsSeqStop then sets 0x800bed80 = 0xFFFF.
  FUN_80074BC4 (callers all overlay/stage code) is "stop-all-sound" → calls it on scene transitions.
- **FUN_80075A80** = per-frame sound-command service (callers = stage handlers 0x80106480..0x80108d4c):
  processes a command queue; a request byte @0x800be22a drives start/stop of BGM.
- SsSeqCalled (0x80090BD0) advances any slot with +0x98 bit0 set. Slot struct = (lw 0x80104c30) +
  i*0xB0; flag @ +0x98; read ptr @ +0x00, SEQ base @ +0x04. Active BGM slot for the newgame scene = **slot 4**.

### Proof the chain works (native port, PSXPORT_NO_FMV=1, FORCE_BUTTONS=FFF7)
- f166 BGM_START(idx=4) fires (ra=0x80074608). **slot 4 read ptr ADVANCES** every frame: base
  0x8018245C → +84 over f166-225 (logged by the [bgmtick] probe). f260 BGM_START(idx=2): slot 2 read
  ptr advances +167. So sequences DO activate and tick.
- **WAV capture (scratch/wav/bgm_nofmv.wav, PSXPORT_WAV, FMVs OFF) has real signal**: RMS 3000-4700 at
  t≈5.5-7.5s (= f166-225, the idx-4 BGM) and t≈9s (idx-2). So the SEQ→SPU→mix path produces audio.
  (Reaching scenes via FORCE_BUTTONS=FFF7 (pulse Start) — and disabling intro FMVs with PSXPORT_NO_FMV=1
   so the capture/frame-timing isn't the FMV player. Without NO_FMV you're capturing the FMVs, not the game.)
- Oracle reference (newgame.sav, fisherman scene): 0x800bed80 = **4**, stable 1500+ frames, no rewrites
  → the oracle holds BGM 4 at that steady scene. Native at f166 ALSO starts BGM 4 (matches).

### What's actually wrong (the narrowed target)
The intro/fisherman BGM plays in our port (audio confirmed) — consistent with the user hearing it. The
silent cases are **specific scenes**: the **menu** and **steady gameplay** (grassy field). There, either
the stage's BGM-start (FUN_80074BF8 via a stage handler) never fires, or a spurious stop/scene-state
divergence kills it. The 0/1 start/stop oscillation I first saw was an ARTIFACT of forced-Start spam
(Start = pause menu, which stops BGM); not a real bug. NEXT: reach the menu and steady gameplay with NO
spurious input (REPL, minimal taps, no pause), confirm whether their BGM-start fires, and if not, find the
stage-handler precondition our native scheduler/boot path doesn't satisfy. Compare the per-frame command
queue @0x800be22a / stage handler calls of FUN_80075A80 vs the oracle at the same scene.

### Gotchas / tooling this session
- **PSXPORT_NO_FMV=1** to skip intro FMVs (else screenshots/WAV/frame-timing are the native_fmv player).
- Oracle REPL `tap`/`press` did NOT advance the title menu from title.sav nor newgame.sav this session
  (input not reaching the menu, or those states idle waiting on stalled CD stream) — could not navigate
  the oracle live; used the RAM-dump snapshots (scratch/bin/tomba2/state/{demo,newgame,title}.bin) and
  newgame.sav idle instead. Native FORCE_BUTTONS=FFF7 DOES drive menus.
- PSXPORT_BGMDBG=1: logs every BGM start/stop with caller ra + the per-frame [bgmtick] read-ptr probe.

## 2026-06-16 (later 74) — BGM: USER-CONFIRMED it's the SPU music MIX (not the output path). Sequenced BGM songs are OPEN but never ACTIVATED — the per-sequence play flag (struct+0x98 bit0) is clear, so SsSeqCalled skips them (read ptr frozen). Jingle/SFX work. Built interactive REPL + watchpoints + screenshots to drive both cores.
Tooling built (user request): native-port REPL (PSXPORT_REPL, FIFO-driven, persistent) mirroring the
oracle's `-repl`; commands run/r/rw/w/w8/watch/unwatch/hits/press/release/tap/regs/seq/shot/stage;
runtime memory watchpoints (mem_set_watch, reports writer PC); `shot <path>` (gpu_native_shot → PPM);
tools/repl2.py drives native|oracle|both. Oracle gained `watch`/`unwatch`.

### Headless samples → USER GROUND TRUTH (scratch/wav/{menu,navigated}_sample.wav)
Generated headless SPU captures; user listened: **NO sequenced BGM** in menu or gameplay (only the
fisherman FMV music + a quest jingle + SFX/footsteps/pause-menu). So headless == windowed: the SPU
music MIX lacks BGM → it is NOT a windowed-output-path bug. The jingle + SFX DO play → the
sequencer/SPU CAN make sound; only the BGM SONGS are silent.

### Root-cause localization (via the REPL at the menu)
- The menu BGM sequence (struct 0 @0x800BE3D8) is OPEN with a real SEQ data ptr (0x80182380, in the
  verified-correct level-data region) and L/R volumes set — but its read pointers DO NOT ADVANCE over
  200 frames (frozen). 
- SsSeqCalled (0x80090BD0) only advances a sequence if **struct+0x98 bit0** (the active/play flag) is
  set. struct 0's flag @0x800BE470 = **0x00000000** (clear) → skipped → frozen → silent.
- So the BGM songs are allocated/opened (playmask 0xC3FF, 14 open — identical to the oracle) but never
  ACTIVATED: the per-sequence play flag is never set (SsSeqPlay/activation not firing for them), while
  the jingle/SFX sequences DO get activated.

### NEXT (the fix)
Find what SETS struct+0x98 bit0 (SsSeqPlay-equivalent) for a BGM song and why our port never calls it
for the menu/gameplay BGM (the jingle/SFX path works). Drive the oracle to a steady-BGM scene
(read structs 7-13 play flags too — slots 0-6 were clear at the pig scene), find which struct is the
active BGM there (flag bit0 set), then watch that flag's writer PC in the oracle (= the activator),
and check why our port's path doesn't reach it. Likely the scene/menu BGM-start code our boot path
skips, or an SsSeqPlay precondition. Tools ready: REPL `watch` on struct+0x98 in both cores.

## 2026-06-15 (later 73) — BGM: the libsnd sequencer INFRASTRUCTURE is correct (state matches the oracle); dialogue issue RETRACTED. Missing BGM is in finer per-sequence state, needs a GAME-stage sequence-table compare vs the oracle.
User refined the symptom: Menu (no BGM) → Story cutscene (no BGM) → **Fisherman (BGM plays)** →
Gameplay (no BGM). Only the fisherman has BGM = it's FMV/XA (native_fmv), everything else is the
libsnd SEQUENCER → so ALL sequenced BGM is silent, one common cause ("fix menu, fix the rest").
**Dialogue is a NON-ISSUE** (user retracted): the "missing dialogue" was the fisherman-cutscene
dialog; the user's spam-Start runs just got stuck on it; vanilla Start skips it. Drop it.

### Method (user directive): track GAME STATE, not output (no WAV/RMS — that's [[track-game-state-not-output]])
SsSeqCalled (0x80090BD0) reads the libsnd state: **0x801054B0** = open-seq count, **0x80104C28** =
playing bitmask, **0x80104C30** = sequence table, **0x800AC424** = tick mode, **0x800AC42C** =
SsSeqCalled ptr. Probe: PSXPORT_SEQDBG (native_boot.c). Reach GAME headless via
PSXPORT_FORCE_BUTTONS=FFF7 (pulse Start; active-low mask) — confirmed reaches GAME stage.

### Finding: sequencer infra is CORRECT — not the bug
- OURS over START→DEMO→GAME: open=14, playmask=0xC3FF, tickmode=5, seqfn=0x80090BD0 — STABLE the
  whole run (only changes at f0→f3 during boot).
- ORACLE @ green-field DEMO (oracle_ram_7000): **identical** — open=14, playmask=0xC3FF, tickmode=5,
  seqfn=0x80090BD0.
⟹ The sequencer is set up and "playing" identically to the oracle. BUT the DEMO is correctly SILENT
in both with this same 0xC3FF — so playmask is TOO COARSE to indicate audible BGM (14 slots stay
allocated; per-scene the game swaps the SEQ song / ramps per-slot volume). The missing BGM is in the
FINER state: which SEQ song / volume each slot plays, or whether the VAB (instrument samples) is in
SPU RAM. later-54's headless "music" (RMS) was likely these always-allocated slots, not real per-scene BGM.

### NEXT
RE the sequence struct (table @0x80104C30, stride from SsSeqCalled's loop) → per-slot {SEQ data ptr,
volume, current song}. Dump it from OURS at GAME (FORCE_BUTTONS=FFF7) AND the ORACLE at GAME
(-inputscript pulse-Start), diff → find which song/volume/VAB differs. Also verify the VAB reached SPU
RAM (the game's SsVabTransfer → SPU data-port/DMA). The native port has NO loadstate, so GAME must be
reached via input each time (oracle has -loadstate, but the oracle plays BGM correctly so it can't
show OUR bug — only serves as the reference state to diff against).

## 2026-06-15 (later 72) — GRAPHICS CORRUPTION FIXED (user-verified). Root cause: our GPU never handled VARIABLE-LENGTH POLY-LINES — it consumed a fixed 3/4 words, drifting the whole GP0 parse so a later data word decoded as an atlas-clobbering VRAM copy.
THE BUG WAS IN OUR PC GPU after all (gpu_native.c gp0_len / FIFO). gpu_differ misled me (later-59/71):
its captured INITIAL VRAM at f3265 was ALREADY clobbered by the prior frame, so both renderers
"reproduced" the garbage — it never tested a clean start.

### Root cause (offline OT analysis, scratch/otanalyze.py)
Walking the real ordering table from its root (0x800EC114) and parsing with CORRECT lengths: the drift
originates at OT node 0x800DDF00, command 0x5EF8F8F8 = **op 0x5E, a gouraud poly-line** (4 vertices =
9 words, = the node's count). PSX poly-lines (line group 0x40-0x5F with bit 0x08; 0x48-0x4F mono,
0x58-0x5F gouraud) are VARIABLE length — a vertex list terminated by a word with
(w & 0xF000F000)==0x50005000 (0x55555555). Our gp0_len returned a fixed 4 ("poly-line term not
modeled"), consuming 4 of 9 words → parse drifts by 5 → ~3000 words later a data word (0x8040333D)
lands at a command boundary and is executed as a GP0 0x80 VRAM->VRAM copy onto the atlas (later-69/70).

### Fix (gpu_native.c)
- gp0_len: 0x80 (VRAM->VRAM copy) is 4 words, not 3 (was grouped with A0/C0 headers — also a real bug).
- gpu_gp0: detect poly-lines at command start and ACCUMULATE words until the terminator (vertex-start
  slots: gouraud even idx>=4, mono idx>=3), then render. s_fifo grown 16->256 (poly-lines are long).
- gp0_exec line branch rewritten to draw N-1 segments over the full vertex list (single lines = 2
  verts = 1 segment, unchanged).
**USER-VERIFIED: graphics are fine now** (green-field/dungeon/gameplay all render correctly).

### Remaining issues (user ground-truth, next)
- Missing BGM almost everywhere (only the fisherman "pull Tomba from water" cutscene BGM plays); menu
  BGM also missing (earlier "menu has no BGM = vanilla" was WRONG).
- Missing dialogue at the start of gameplay (Tomba & Zippo dialogue doesn't play).
- User hypothesis: we may be initializing/running in DEMO mode (would explain missing dialogue, and
  possibly the BGM triggers). Investigate a DEMO-vs-GAME mode flag.

## 2026-06-15 (later 71) — CONFIRMED the clobber is a corrupted ORDERING-TABLE entry in RAM, NOT our GPU. gpu_differ at the ACTUAL clobber frame (f3265): replaying the same word stream through OUR renderer AND Beetle both clobber the atlas identically. So the bad VRAM-copy is in the display-list words from RAM; the fix is upstream (whatever builds/corrupts the OT).
gpu_differ (later-59) had only ever tested f3360/f5000, never the f3265 clobber. Tested it now:
- PSXPORT_GPUTRACE="3265:…" captures the 10815-word GP0 stream fed at f3265 (post-DMA-traversal).
- replay_ours vs `wide60rt -gpureplay` of that SAME stream: atlas (320,0) 92% same, (576,0) 100% same
  (the 8% is the known UV-round/dither residual, later-44). BOTH renderers execute the copy and clobber
  the atlas. ⟹ the bad 0x80 copy is genuinely in the word stream from RAM; our GPU/parser is faithful
  (re-confirmed). The clobber word 0x8040333D lives at 0x800D8FD4 as a DATA word (last word of a
  gouraud-tri, immediately followed by a 0x090D8044 OT header) — i.e. the OT stream is structured so a
  data word lands at a command boundary and is executed as a MoveImage.

### Where this leaves the fix
The display list (ordering table) our interpreted game-logic/HLE builds at ~f3265 is corrupted (a
node count / next-pointer / buffer is wrong, so the DMA feeds a misaligned stream that decodes a data
word as an atlas-clobbering VRAM copy). The OT lives in 0x800Cxxxx-0x800Exxxx, exactly the dynamic
region that diverges from the oracle (later-60). This is upstream of the GPU entirely.

### NEXT (unavoidable: from-boot OT-corruption hunt)
The transplant masks accumulation, so go from-boot. Find what writes the bad OT: PSXPORT_CW watchpoint
on the OT buffer around f3265 (filter to the offending node/words), or sequence-diff the OT-build path
(ClearOTagR @0x80081458 / AddPrim / the draw-enqueue) vs the oracle. Identify the routine that
mis-builds the list (a wrong word-count in a primitive, a stale double-buffer OT index DAT_1f800135,
or an HLE memcpy/alloc that overruns the OT) and PC-own/fix it. NO bandaid (do not just skip the copy).
Tooling: gpu_differ now builds again (replay_ours.c defines g_ram); PSXPORT_TEXWATCH logs node+words;
PSXPORT_CLOBBERDUMP dumps RAM at the clobber.

## 2026-06-15 (later 70) — the clobbering copy is a MALFORMED OT NODE (garbage words read as a VRAM copy). Bad words come from the OT in RAM, not our GPU parser. Root cause = a bad OT node our port builds and the oracle doesn't.
The f3265 atlas-clobbering 80copy (later-69) is node **0x800D4B9C**, raw words
**8040333D, 34383838, 005A0040, 38FF322E**. A real libgpu MoveImage cmd word is 0x80000000; this is
0x8040333D with ASCII-ish operands (0x34383838="8884", 0x2E=".", 0x3D="=") ⟹ these are DATA/garbage
words being read as a GP0 0x80 VRAM→VRAM copy (which lands on the atlas: dest (64,90) 558x255).
gpu_differ (later-59) already showed Beetle reproduces our garbage from our own captured GP0 WORDS, so
our GPU parser is fine — **the bad words are in the OT (game RAM)**, fed to GP0 by the DMA. So our port
builds (or corrupts) a bad OT node at ~f3265 that the oracle does not (the OT region 0x800Cxxxx-0x800Exxxx
is exactly where our RAM diverges from the oracle, later-60).

### Reconciles every prior result
- VRAM-only transplant clean (later-65): the clobber (f3265 ≈ present 3265) happened BEFORE the
  transplant (logic 1680 ≈ present 4267); the transplant replaced the clobbered atlas with clean and the
  one-time clobber didn't recur ⟹ stayed clean. RAM-only no effect: the VRAM damage was already done and
  RAM transplant doesn't repaint VRAM.
- Decompressor/load/upload byte-perfect (later-69): the atlas WAS correct (f2591 == oracle 100%); the
  clobber is the only corruption.

### The real bug, restated
A malformed OT node (0x800D4B9C) appears in our display list around f3265 and decodes as a spurious
VRAM copy that overwrites the atlas. It is upstream of the GPU — in the interpreted game logic / HLE
that builds or owns that OT buffer (a wrong word-count, a stale/garbage pointer, or a buffer the OT
node refs that got clobbered). This is the SAME class as the long-standing "dynamic region
0x800C-0x800E diverges from oracle."

### NEXT (the from-boot divergence hunt — the call-sequence harness the user approved)
The transplant masks accumulation, so go from-boot: find the FIRST frame the OT (or its source struct)
diverges from the oracle, via a sequence-keyed differential (log the OT-build / AddPrim / the function
that emits node 0x800D4B9C's command, sequence-numbered, in both engines; diff). Identify the
instruction/HLE that writes the bad word. Watch 0x800D4B9C-area stores around f3265 (PSXPORT_CW /
oracle PSXPORT_WATCHW). The owning routine, once found, gets PC-owned per the user directive.

## 2026-06-15 (later 69) — DECOMPRESSOR/LOAD/UPLOAD ALL CORRECT. The atlas is decompressed byte-perfect then CLOBBERED by a VRAM→VRAM copy (80copy dest=(64,90) 558x255 @f3265). The corruption is a spurious/mistimed VRAM copy, not the asset pipeline.
Big correction to later-66/67/68 (the reused-scratch made the input look wrong; it is not).

### The decompressor is FAITHFUL — atlas is correct right after unpack
- The oracle does the SAME load+unpack sequence we do (oracle calltrace via PSXPORT_CALLTRACE +
  psxport_hooks.cpp loader logging): same LBAs unpacked (6613/6625/6717/6774), 2020 loaded-not-unpacked
  — IDENTICAL to ours. So NOT a sequencing bug; 2020 is not the atlas.
- The decompressor offset table @0x800153C8 is byte-identical ours vs oracle.
- **Texpage (320,0) right after our 6625 unpack (present f2591) is 100.0% identical to the oracle**
  (oracle_vram_7000). So our decompress+upload of the atlas is byte-perfect. (later-66/67's "input
  52.9%/wrong" was the reused-scratch 0x18A000 ghost — the LIVE input matches the disc 100%.)

### …then it gets CLOBBERED
- Our (320,0) at present f4267 is only 4.7% == its f2591 self ⟹ overwritten between f2591 and f4267.
- PSXPORT_TEXWATCH="320,0,512,256": exactly ONE write hits the atlas in that window —
  **f3265 80copy src=(56,56) dest=(64,90) 558x255** (a VRAM→VRAM block copy). dest (64,90)+558x255
  spans x64-622 y90-345, swallowing the atlas texpages (320,0)-(512,256). This single copy accounts
  for the whole (320,0) change.

### So the real bug
A VRAM→VRAM copy overwrites the already-correct atlas. The decompressor/CD-read/upload are all
RULED OUT (byte-perfect). The copy either (a) shouldn't run / has wrong coords/size in our port, or
(b) is mistimed (ordering vs the atlas load — sync-vs-async CD), or (c) its source (56,56) holds
garbage in our port. The oracle's atlas survives at f7000, so the oracle either doesn't do this copy,
does it before the atlas load, or to different coords.

### NEXT
Capture the oracle's GP0 0x80 (VRAM→VRAM) copies (hook GPU in wide60rt, or gpudump) around the
green-field demo and compare to ours: same copy? same coords/size? same timing relative to the atlas
unpack? Then find what issues our f3265 copy (GP0 0x80 origin in the OT / a libgpu MoveImage) and why
it lands on the atlas. Tools added: psxport_hooks.cpp loader/unpack calltrace (oracle), the early/late
VRAM compare recipe (PSXPORT_VRAMDUMP at two frames), PSXPORT_TEXWATCH.

## 2026-06-15 (later 68) — mapping the loader/streaming dispatcher; two load paths found. Need the ORACLE's load/unpack LBA sequence to pin the correct atlas (reused-scratch ambiguity). User directive: PORT the loader+streaming to PC-native.
Caller (ra) logging on ov_cd_loadfile + ov_unpack_group: the load+unpack path that fills the atlas is
**FUN_80044f58** (loadfile call ra=0x80045008 i.e. the jal at 0x80045000→FUN_8001dc40; unpack
ra=0x8004501C i.e. jal 0x80044e84 @0x80045014, anchor 0x1fd000, then a 42-word descriptor copy to
0x800fb178/0x8010-4e90). Our run unpacks ONLY 6625→(320,0) and 6774→(576,0) via this path. The
LBA-2020 load comes from a DIFFERENT site (ra=0x80045430 = jal FUN_8001dc40 @0x80045428) and is
followed by 0x80045558/0x80045258 processing — NOT the FUN_80044E84 unpacker.

### Open ambiguity (must resolve before fixing)
"Oracle atlas = LBA 2020" (later-67) was inferred from oracle 0x18A000 @f7000 = LBA 2021, but 0x18A000
is REUSED scratch — at f7000 it may just hold the LAST load (2020), while the live atlas was
decompressed from an EARLIER load since overwritten. So I do NOT yet know the correct atlas source.
Two live hypotheses: (a) demo-SEQUENCE divergence — our port streams/unpacks DIFFERENT areas
(6625/6774) than the oracle by the same scene point (our sync CD vs async timing, or skipped-intro
demo state); (b) a later load+unpack the oracle does and we miss. Both are streaming-sequence bugs.

### Plan (user: port the loader+streaming to PC-native)
1. Instrument the ORACLE (wide60rt, psxport hooks) to log every loadfile (FUN_8001db8c/dc40) and
   unpack (FUN_80044E84) with LBA/dest/size, run to the green-field hut, and DIFF the sequence vs ours
   (scratch/logs/ra.log) → find the first divergence (which area/atlas, when).
2. Then PC-own the loader+unpack sequencing (it's asset-streaming infra, not game logic) so the right
   atlas is unpacked deterministically when its load completes — instead of relying on the async
   IRQ/state-machine our no-IRQ port desyncs. Verify: green-field renders clean with NO transplant.

## 2026-06-15 (later 67) — ROOT CAUSE: the correct atlas (LBA 2020) is LOADED but NEVER UNPACKED. Our texpages keep a PRIOR area's atlas. Load→unpack streaming desync (synchronous native CD, missing async completion trigger).
Nailed it (continuing later-66). The oracle's green-field atlas decompresses from **disc LBA 2021**
(discscan on oracle_ram_7000 @0x18A800 = 100% consecutive from LBA 2021; i.e. ov_loadfile of LBA 2020
table + 2021 data). We DO load LBA 2020 (cd.log: "loadfile 448512 B @ LBA 2020 -> 0x8018A000").

### The smoking gun (one run, CD_VERBOSE + UNPACKLOG + UPLOADLOG)
Across the WHOLE attract run there are exactly **two** atlas (count=10) unpacks, both from the WRONG
source, and the atlas texpages are uploaded exactly twice:
- LBA 6625 → unpack → upload (320,0) 192x256  (f2590)
- LBA 6774 → unpack → upload (576,0) 256x256  (f3054)
- **LBA 2020 (the correct atlas) is loaded (line ~2421) and then NO count=10 unpack ever follows it.**
So our VRAM atlas at (320,0)/(576,0) holds the decompressed LBA 6625/6774 textures (a PRIOR area),
never the hut atlas (LBA 2020). The oracle unpacks LBA 2020 by its frame 7000; we never do. Our
logic-frame 1681 ≈ oracle 7000 (same area, level data identical), so we SHOULD have unpacked it by
then. Same disc data (our LBA-2020 load == oracle's LBA-2021 input), same decompressor ⟹ if we
unpacked it we'd get the clean atlas.

### Root cause class: CD-streaming / load→unpack sequencing desync
The atlas unpack is triggered by game logic AFTER the load completes. Our native CD is SYNCHRONOUS
(ov_loadfile / ov_cd_read complete instantly) and we deliver NO preemptive IRQ; the real game streams
asynchronously and sequences load→unpack via completion callbacks / a state machine (cf. the CD
streaming contract notes, FUN_8001d7c4 per-sector IRQ callback that our no-IRQ runtime never fires).
The instant-vs-async timing desyncs that state machine, so the geometry advances to the hut scene
while its atlas is loaded-but-unpacked (or the unpack trigger is missed entirely). This is consistent
with the user's "accumulates as you play" — each new area whose atlas-unpack trigger is missed keeps a
stale/wrong atlas; effect sprites etc. degrade as more areas stream.

### NEXT (the fix)
Find what triggers the count=10 atlas unpack (FUN_80044E84) after a loadfile in the real game — the
event/callback/flag the game polls — and ensure it fires in our port when the corresponding load
completes (deliver the completion event, or drive the unpack from the load-complete path). Verify:
after the fix, a count=10 unpack from LBA 2020 should appear and upload to (320,0)/(576,0), and the
green-field render should match the oracle WITHOUT any transplant. Trace via the loadfile→unpack
caller chain (who calls FUN_80044E84, gated on which flag) and the LBA-2020 loadfile's caller.

## 2026-06-15 (later 66) — corruption traced to the ATLAS LOAD: decompressor input is the WRONG DATA for the scene, though our CD read is faithful-to-disc (consecutive 100%). The atlas load targets the wrong disc location.
Continuing later-65 (corruption = VRAM atlas content). Traced the atlas pipeline backward.

### The atlas is decompressed from 0x8018A000 by the unpacker (FUN_80044E84)
PSXPORT_UNPACKLOG/UNPACKDUMP (games_tomba2.c): the green-field atlas is a count=10 unpack group at
table 0x8018A000, data at +0x800, decompressed to the 0x1Exxxx scratch then uploaded to the texpages
(576,0 256x256, 320,0 192x256, …). 0x8018A000 is REUSED scratch (5 unpacks/run: c2,c10,c2,c10,c2,
filled by ov_loadfile from LBAs 6613/6625/6717/6774/2020 — so static LBA compares there are unreliable;
capture LIVE in the override instead).

### The decompressor INPUT is wrong, but our READ is faithful to the disc
- Same-scene RAM compare (ours logic-frame 1681 vs oracle frame 7000, same area — level data
  0x158000/0x182000 == oracle 100%): the compressed-atlas region **0x18A000 is only 52.9% == oracle.**
- BUT our LIVE unpacker input (captured in the override, scratch/raw/unpackdump/unpack_*_c10.bin)
  matches the disc **CONSECUTIVELY 99.99%/100%** (data@+0x800 == disc LBA 6626 / 6775; tool
  scratch/bin/discscan: finds the best first-sector LBA then measures consecutive match). ⟹ our
  ov_loadfile reads exactly the disc's consecutive bytes — **the CD read is faithful** (re-confirms
  later-60; submode 0x08 is just this disc's data convention, NOT XA interleave — the verified-good
  level load at LBA 1908 is also submode 0x08).

### So we load faithful-but-WRONG data
The data at LBA 6625/6775 is read correctly but it is NOT the atlas this scene needs: the oracle's
atlas (oracle_vram_7000) renders OUR geometry CLEANLY (later-65 VRAM-only transplant), and we are in
the SAME area (level data identical), so the correct atlas for our scene == the oracle's. We loaded a
different one. ⟹ the bug is the **LBA/file our atlas loadfile targets** (FUN_8001db8c a1=lba) — wrong
disc location for this area's atlas — OR a demo/sequence divergence selecting a different atlas. NOTE
the dynamic game-state region (0x800C0000+) diverges from the oracle (different demo MOMENT, same
area), so a sequence/timing divergence that changes WHICH atlas is live is plausible and must be
checked.

### NEXT
Instrument the oracle's ov_loadfile-equivalent (FUN_8001db8c @0x8001db8c) to log dest/lba/size, run to
the green-field hut scene, and diff the load sequence vs ours (scratch/logs/cd.log). Find where our
atlas LBA (or the area-load order) first diverges from the oracle's, then trace WHY (the LBA is
computed by interpreted game logic from a directory/manifest — a wrong directory entry, wrong file
handle, or an earlier state divergence). Tools added: PSXPORT_UNPACKLOG/UNPACKDUMP (games_tomba2.c),
scratch/bin/discscan (find a buffer's disc LBA + consecutive match).

## 2026-06-15 (later 65) — CORRUPTION ISOLATED TO VRAM TEXTURE CONTENT via a state-transplant harness. RAM + per-frame logic + per-frame CLUTs are all CORRECT; the scene-load atlas textures baked into VRAM are wrong.
Built a transplant harness (PSXPORT_TRANSPLANT="frame:ramfile:vramfile", native_boot.c +
gpu_native_load_vram) that drops the ORACLE's clean green-field state into our RUNNING port at a
logic-frame boundary and CONTINUES. The user predicted "transplant will mask the bug" — correct, and
that masking IS the diagnostic.

### Oracle dumps (green-field, frame 7000): scratch/raw/oracle_ram_7000.bin + oracle_vram_7000.bin
(oracle: PSXPORT_RAMDUMP=7000:.. + PSXPORT_VRAMDUMP=7000:.. ; wide60rt <disc> -bios scratch/bios -frames 7100)

### Results (transplant at our logic frame 1680; effect visible from present f4267 — boot turbo makes
the logic→present map nonlinear, so find the transplant point by first-diff vs baseline, not by math)
- **Full transplant (RAM+VRAM) → CLEAN.** Our per-frame logic renders a perfect green-field (Tomba on
  grass) and STAYS clean for 78+ frames. ⟹ the per-frame rendering/logic is CORRECT; it does not
  corrupt good state.
- **RAM-only transplant → byte-identical to the no-transplant baseline (ZERO render effect).** The
  game RAM state is NOT the differentiator (our logic re-derives the same scene; transplanted RAM
  doesn't rebuild the already-built VRAM). ⟹ game RAM state is fine.
- **VRAM-only transplant → CLEAN (same as full).** Replacing ONLY our VRAM with the oracle's fixes the
  render completely.
⟹ **The corruption is entirely in our VRAM TEXTURE CONTENT.** And since the VRAM-only run KEPT our RAM
yet the per-frame 16x1 CLUT re-uploads (5315/run, sourced from our RAM 0x801FCDC0) did NOT re-corrupt
the clean VRAM over 78 frames, **the per-frame CLUTs are also correct.** The wrong data is the
SCENE-LOAD ATLAS textures (the big 256x256/192x256/… uploads), baked into VRAM once at load.

### So the bug is in the scene-load texture pipeline: decompress → upload
Upload is faithful (later-63, native). So either the DECOMPRESSED output is wrong, or textures are
placed at wrong VRAM coords, or the descriptor that drives it is wrong. NOTE this re-opens the
decompressor: later-61 only proved native==recomp==interp (consistency across OUR engines), never
correctness vs the oracle — and the "clean-C is faithfulness-independent" argument only covers
load-delay (now ruled out), not other subtle bugs or wrong INPUT addressing. NEXT: diff our green-field
VRAM ATLAS region (camera-independent, loaded once) against the oracle's to see WHICH texpages are
corrupt, then trace which decompress/upload call produced them. (Atlas is scene-static so this compare
is alignment-SAFE, unlike the framebuffer.)

## 2026-06-15 (later 64) — LOAD-DELAY RULED OUT as the corruption cause (measured, not assumed). Zero genuine load-delay hazards in the executed rendering code. New tooling: PSXPORT_LDHAZARD detector + PSXPORT_VRAMDUMP.
The prime remaining hypothesis (interp.c:13 "no load-delay slot") was that the interpreter's
omission of the R3000 load-delay slot makes game-logic draw-param code compute wrong UVs/CLUT/geom.
**Tested it directly — it is NOT the cause.**

### Detector (PSXPORT_LDHAZARD, interp.c) — counts REAL divergences
A load-delay divergence is: instruction I-1 loads a GPR, and the very next executed instruction I
reads that GPR (so our no-delay model gives the new value, hardware/oracle gives the old). The
detector follows TRUE execution order, INCLUDING jump/branch delay slots (a load in a delay slot is
checked against the branch TARGET, not the memory-next word). It excludes the benign lwl/lwr
unaligned-merge idiom (our no-delay model already merges those correctly: lwl commits, lwr merges).
- **Runtime, full attract incl. green-field (f3360) AND dungeon (f5000), 2600 logic frames: 0
  genuine hazards.** The overlays (gameplay logic, rec_coro_run) have NONE.
- **Static scan of all of MAIN.EXE (scratch/ldscan.py): 44 raw hits, ALL in the DATA region**
  (addresses ≥0x800A0000; highest recompiled function is 0x8009D06C) — pointer/asset tables misread
  as code, not real instructions. Zero in actual code.
- Why: the SN/GCC toolchain fills every load-delay slot (nop or an independent op), so compiler
  output never reads a load target in the next slot. Only hand-asm would, and the lwl/lwr pairs
  (the only load→next-reads-target cases that DO occur) are handled correctly already.
⟹ Adding faithful load-delay would be effort + regression risk for a NON-cause. Do not implement it
to "fix" the corruption. (It's still legit CPU correctness if we go interpreter-only, but it won't
change a pixel here.) RULED OUT — do not re-chase.

### Also this session
- **PSXPORT_VRAMDUMP="frame:path"** added to gpu_native.c (raw 1024x512x16), matching the oracle's
  same-named knob (main.cpp). Cross-engine VRAM diff at green-field (ours f3360 vs oracle f7000):
  atlas region differs, but that is the alignment ghost (different demo moment) — ours actually has
  MORE nonzero than the oracle in the atlas; NO black/missing-upload cells (cellcmp.py). Inconclusive
  for the bug (the documented alignment block), but rules out "a texture is missing from VRAM".
- Confirmed (POLYDUMP f3360 + f3720): every textured poly's texpage lands inside an uploaded atlas
  region (x≥320) → texpage selection is CORRECT. Some per-frame CLUTs are real palettes (1008,255),
  some are uniform 0x1084 (800,198 / 496,488) — uniform-palette = suspect but not yet pinned.

### Where the bug stands (elimination)
Ruled out: rasterizer, CD-read, XA, unaligned-SWR, raw-load, decompressor, **upload library**
(later-63), **load-delay** (this entry). Texpages correct; atlas content correct by argument
(native decompressor is faithfulness-independent yet byte-identical to recomp/interp). Remaining:
wrong UVs / wrong CLUT-INDEX per poly / wrong per-frame CLUT CONTENT / wrong vertex geometry —
all computed by interpreted game logic — OR a wrong/missing HLE the render path relies on. The
decisive tool is still a scene-aligned oracle lockstep (find FIRST RAM divergence via a shared
logic-frame counter); cross-moment VRAM/RAM diff stays blocked.

## 2026-06-15 (later 63) — GPU UPLOAD LIBRARY now PC-native (user directive: "PC-native GPU, not faithful"). RULES OUT the upload library as the corruption source — fully native upload reproduces the SAME garbage byte-near-identically.
User directive (corrects later-62's "faithfully port the vtable"): the GPU library should be **PC-native,
not a faithful recomp**. So instead of byte-translating the libgs vtable, I replaced the upload entry
point with a native VRAM blit and tested whether the corruption changes.

### What was done
- **FUN_80081218 (0x80081218) → `ov_upload_image` (games_tomba2.c) + `gpu_native_load_image`
  (gpu_native.c).** Empirically RE'd (PSXPORT_UL_PROBE + the A0 UPLOADLOG, exact match): a0 =
  descriptor { x:s16@0, y:s16@2, w:s16@4, h:s16@6 }, a1 = source = w*h contiguous 16-bit pixels,
  row-major. The recomp body ENQUEUES into the GsSortObject ring @0x800A5AC8 (head/tail @5AC8/5ACC,
  mod 0x40), DMA'd later as a GP0 0xA0 packet. **It is the SINGLE chokepoint for BOTH the scene-load
  texture atlas (256x256/192x256/128x256… into the character texpages) AND every per-frame 16x1 CLUT
  — 5315 CLUT + ~25 atlas calls per attract run.** Native impl writes the rect straight to s_vram and
  does NOT enqueue (later ring flush/sync no-ops over the empty ring). A/B: PSXPORT_LZ_RECOMP=1 keeps
  the recomp upload library. The unpacker (ov_unpack_group) routes its uploads here too → the whole
  decompress→upload asset path is now PC-owned.

### RESULT — upload library RULED OUT as the corruption source
A/B over 5187 presented frames (native vs PSXPORT_LZ_RECOMP=1), `cmp` each PPM:
- **5038 byte-identical; 149 differ, all in the green-field demo f3270–3775, by ~0.4% (≈300px/76800).**
- The differing pixels are a CLUT upload-TIMING offset (native writes immediately at the enqueue call;
  recomp defers to the ring flush — likely a 1-frame CLUT lag, so native is plausibly *more* correct).
- **The corruption is UNCHANGED.** f03360 (striped grass/black blocks/garbled char) and f03720
  (magenta+RGB-noise striped pillars) are visually identical native vs recomp; f03720 is byte-identical.
  ⟹ a fully PC-native upload reproduces the same garbage ⟹ **the upload library is not the cause.**

### So where is the bug now (narrowed)
Source texture data in RAM is correct (later-60: ==oracle 100%); decompressor byte-perfect
(later-61); upload faithful (this entry); rasterizer faithful (later-59). The garbage is
"uninitialized VRAM sampled as texture" (magenta + RGB noise). With correct textures + correct
upload + correct raster, that leaves the **DRAW STREAM PARAMETERS** (UVs / texpage / CLUT-index /
geometry) computed by the **interpreted gameplay overlay** — i.e. either wrong draw params, or
MISSING uploads (the overlay logic that decides what/where to upload computed wrong). Prime suspect
remains the interpreter's faithful-first quirks (no load-delay slot, add==addu — interp.c:13) that
both our engines share but the oracle does not; the gameplay overlays run THROUGH this flat
interpreter (interp.c:3). NOTE the decompressor ran clean through that same interpreter, so the quirk
is path-specific (geometry/UV code may have load-delay deps the codec does not). NEXT: determine
whether our VRAM atlas is correct vs oracle at the green-field scene (static, scene-alignable) — if
yes, the bug is in draw params (interpreter arithmetic), if no, in upload placement.

## 2026-06-15 (later 62) — porting non-gameplay subsystems to native (user goal). Decompressor + texture-unpacker now PC-owned & verified byte-identical. Next subsystem mapped: the libgs-style gfx/upload vtable.
Per the user's directive ("have anything EXCEPT gameplay logic PC-owned"), porting the asset/gfx
library out of recomp+interp into native C, one subsystem at a time, A/B-verified vs the recomp body
(PSXPORT_LZ_RECOMP=1) until the gameplay 2D-sprite corruption surfaces/resolves.

### Done this session (committed, each byte-identical to recomp over the full attract run)
- **LZ image decompressor** FUN_80044D8C → `ov_lz_decompress`/`lz_decompress` (games_tomba2.c).
- **Texture-group unpacker** FUN_80044E84 → `ov_unpack_group`. Reads descriptor table (count +
  12-byte entries {stride@+4, field@+6, srclen@+8}; packed source 0x800 past the table); per entry
  dst = anchor − 2*stride*field, native-decompress, then FUN_80081218 (upload) + FUN_80080f6c.
  NOTE: both ports are behaviour-neutral so far (frames identical) — they don't fix the corruption;
  they're steps toward full PC ownership. The recomp is faithful, so the divergence is elsewhere.

### NEXT subsystem mapped: the gfx/upload library (libgs/GsSortObject-style vtable)
- The gfx context object is the STATIC struct at **0x800A5958** (pointer cached at 0x800A5998; state
  byte at 0x800A59A2, =0/1/2 selects handlers). Its function-pointer fields (read from a RAM dump):
  - obj+0x08 = **0x80082D04** ← the actual upload method (FUN_80081218 calls this with
    a0=mem_r32(obj+0x20)=0x80082734, a1=descriptor, a2=8, a3=dst).
  - obj+0x0C = 0x80082504, obj+0x1C = 0x80082970, obj+0x20 = 0x80082734, obj+0x3C = 0x80083364
    (used by FUN_80080f6c).
- FUN_80081218 also calls FUN_80080fd4(template=0x8001BF2C, descriptor) first — a 3-way state
  machine on 0x800A59A2 → handlers 0x80081010 / 0x8008109c / 0x800810e0.
- To PC-own the upload path: port FUN_80081218 + FUN_80080fd4 + FUN_80080f6c and the method
  0x80082D04 (and whatever it dispatches). This is the prime remaining non-gameplay subsystem and the
  most likely home of the corruption (or an HLE/recomp quirk it relies on). Tooling: PSXPORT_TEXWATCH
  (VRAM-rect upload trace), PSXPORT_CW (RAM store watchpoint), PSXPORT_UPLOADLOG.

### Obstacle to differential debugging (documented, do not re-fight)
Cross-engine RAM/VRAM compare is blocked by frame-alignment: our native-boot frame numbering ≠ the
oracle's, and the attract demo's dynamic CLUT/area cycling means our f3340 ≠ oracle f7000 for dynamic
state (static level data DOES align 100%, but front-buffer/CLUT do not → 0% at arbitrary frames).
So verify ports by A/B (native vs recomp, same engine) + visual, not by oracle RAM diff.

## 2026-06-15 (later 61) — DECOMPRESSOR RULED OUT (recomp==interp==native byte-identical); 0x801FCDC0 is transient scratch not the live CLUT; loaded data is disc-correct. New direction (user): port ALL non-gameplay-logic to native.
Chased the per-frame CLUT at 0x801FCDC0. Findings, including a falsified hypothesis (kept honest):

### What the corruption looks like (user ground-truth, windowed + headless repro)
2D effect sprites corrupt and ACCUMULATE: gems → water splashes → eventually "busted" (magenta
polys + RGB-noise stripes = uninitialized memory sampled as texture). 3D chars/terrain often clean;
it's the 2D sprite/CLUT path. Reproduces headless: f5000 dungeon demo = character is a rainbow blob,
2D counter garbled (`scratch/frames/view_05000.png`). "Corrupted differently each time" windowed =
OS-threading races (user: don't chase determinism; headless IS deterministic).

### FALSIFIED hypothesis (do NOT repeat): "decompressor recomp-vs-interp divergence"
`gen_func_80044D8C` is the LZ image decompressor (2D predictors: offset[i]=base+2*factor*stride from
the static table @0x800153C8; ctrl byte → len=ctrl>>3, mode=ctrl&7; mode0=literal, else back-ref).
I caught it (PSXPORT_CW host-backtrace store watchpoint, added to mem.c) writing ZEROS to 0x801FCDC0
via the flat interpreter, while the recompiled path wrote the correct CLUT — looked like a
recomp-vs-interp divergence. **It is NOT.** Rendered frames are BYTE-IDENTICAL across the entire
5947-frame attract run whether the decompressor runs native, recompiled, or flat-interpreted
(`scratch/frames/{after,before}` cmp: 5947 same / 0 differ). The "zeros at 0x1FCDC0" is a TRANSIENT
decompression-scratch leftover — the unpacker `gen_func_80044E84` sets dst = anchor(0x1FD000) −
2*stride*field, decompresses each texture into that (intentionally overlapping) scratch, then
immediately uploads it to VRAM via `0x80081218` (vtable dispatch through the gfx object @0x800A5998).
The LIVE CLUTs/textures are in VRAM, not RAM — comparing 0x1FCDC0 in a RAM dump was an alignment ghost.

### Ruled out / verified this session
- **Decompressor** (recomp==interp==native, byte-identical over the whole run). NOT the cause.
- **Loaded compressed source is disc-correct**: scratch 0x8018A000 at f3340 == disc LBA 2636 100%
  (tool `scratch/bin/srccmp` — links disc.c, compares a RAM-dump region to disc sectors).
- 0x80158000 / 0x80182000 static level data == oracle 100% (scene base aligns). 0x8018A000 is a
  REUSED per-area scratch (loaded many times from different LBAs) → cross-frame RAM compare there is
  meaningless (dynamic; our demo-moment ≠ oracle's for the dynamic CLUT cycling).

### Changes kept (verified, aligned with the new goal)
- **LZ decompressor is now PC-owned** (`ov_lz_decompress` in games_tomba2.c; A/B via
  PSXPORT_LZ_RECOMP=1). Verified byte-identical to recomp over the full run. First step of the goal,
  but does NOT fix the corruption by itself.
- **interp.c rec_coro_run**: tail-`j` (op 0x02) now routes through coro_native_call, so native
  overrides / BIOS vectors fire on tail-jumps too (were bypassed — only jal/jalr/jr checked).

### Direction (user): "have anything EXCEPT gameplay logic PC-owned"
Port the texture/gfx/upload library to native C and keep going until the corruption surfaces or
resolves. NEXT native targets: unpacker 0x80044E84, upload 0x80081218, the gfx object @0x800A5998
(libgs/libgpu-style; method table at obj+8/+1c/+20, obj+0x3c). Root cause still OPEN — likely in the
gfx/upload library, or a shared recomp+interp faithful-first quirk (no load-delay slot, add==addu)
that both engines share but the oracle doesn't.

## 2026-06-15 (later 60) — corruption RULED-OUT list grows: NOT unaligned-SWR (latent SWR bug found+fixed but never hit), NOT CD-read, NOT XA-interleave. Corruption is in the gameplay texture UPLOAD/VRAM-content path; data loads correctly.
Continued from later-59. The corruption is in the VRAM texture atlas content (gpu_differ replays from
the trace's INITIAL VRAM and both renderers reproduce the garbage ⇒ it's baked into VRAM by prior
uploads, not per-frame rasterization).

### Found + fixed a REAL but LATENT bug: `mem_swr` preserve-mask (mem.c)
`mem_swr`'s keep-mask was `0x00FFFFFFu >> (32-sh)` — wrong; it zeroed the low bytes SWR must PRESERVE
on every unaligned store (verified vs canonical LE tables, `scratch/bin/uatest`: k=1/2/3 gave
e.g. `ADBEEF00` vs correct `ADBEEF88`). Fixed to `0xFFFFFFFFu >> (32-sh)`. LWL/LWR/SWL verified
CORRECT. BUT: **instrumenting (PSXPORT_UASWR_DBG) shows unaligned SWR is hit ZERO times in the demo**,
and post-fix demo frames are **byte-identical** to pre-fix (`cmp` f3345/f3386/f5000/f4600) — so this
fix does NOT change the corruption. Kept anyway (real correctness fix; a future unaligned SWR would
corrupt). NOT the cause.

### Ruled out (do not re-chase)
- **Rasterizer** (later-59: gpu_differ front-buffer IDENTICAL to Beetle on f3360+f5000).
- **CD data read**: reads happen (big level loads 188K/333K/448K → staging 0x8018A000/82000/58000;
  single-sector stream → 0x800EF478). Sectors at the gameplay LBAs are all **Mode2 Form1 DATA**
  (submode 0x08), consecutive — so consecutive-sector reading is correct; loaded bytes = the disc
  bytes the oracle also reads. Tool: `scratch/bin/secprobe` (links disc.c, prints submode per LBA).
- **XA interleaving** of the texture region: falsified (no Form2 audio sectors interleaved there).
- **Unaligned SWR**: never hit (above).
Staging RAM at a gameplay frame looks like real data (89–94% nonzero, high uniqueness) — but proving
it correct needs a SAME-SCENE compare vs the oracle (level data is static once loaded, so that
compare is alignment-insensitive — the decisive next test).

### RESULT of the same-scene RAM compare: LOADED DATA IS BYTE-PERFECT (load/decompress ruled out)
Dumped recomp RAM at a green-field gameplay frame and oracle RAM at its green-field frame (7000), then
compared. The texture/level-data staging regions **0x80158000 / 0x8018A000 / 0x80182000 match the
oracle at 100.0%** (768 KB byte-identical across two different engines ⇒ same scene AND correct data).
**So the loaded+decompressed texture data in RAM is correct; load/decompress is NOT the bug.** Tools:
recomp `PSXPORT_RAMDUMP_FRAME=N PSXPORT_RAMDUMP=path`; oracle `PSXPORT_RAMDUMP=frame:path`.

Full 2 MB aligned diff: only the **dynamic object/game-state region 0x800C0000–0x800E0000 (~68–77%)**
and low kernel RAM (0x0–0x10000, 22%) diverge — but that is almost certainly **frame-WITHIN-scene**
difference (the demo animates; I can align to the scene via static data, not to the exact frame), not
corruption. So the RAM diff is inconclusive for the corruption itself.

### Upload path mapped (new tooling: PSXPORT_UPLOADLOG in gpu_native.c — logs A0 dest/dims + DMA src)
- Intro/title (CLEAN): per-frame **full-frame 320×240 CPU→VRAM uploads to (0,0)** from 0x8001FB9C
  (pre-rendered frames — that's why the intro path is clean; no per-poly texturing).
- Green-field gameplay: **textures uploaded ONCE at scene-load** (f3054) to the atlas texpages the
  character samples — e.g. dest (576,0) 256×256, (896,0) 128×256, (832,0) 64×256 — streamed from a
  **high-RAM DMA staging area 0x801Fxxxx** (0x801FC800/FC600/FC580…). Then **CLUTs (16×1) re-uploaded
  EVERY gameplay frame** from **0x801FCDC0** to the exact CLUT slots the character uses
  ((1008,227),(1008,255),(800,190),(816,205)…).
- CRUCIAL: the texture/CLUT DMA source (0x801Fxxxx) is a **DIFFERENT region** from the raw level-data
  staging (0x80158000/8A000/82000) that matched the oracle 100%. So the raw load is correct, but the
  **processing step that fills the 0x801Fxxxx DMA buffer (decompress / CLUT build)** is unverified and
  is now the prime suspect — it runs in the interpreted gameplay overlay (DEMO.BIN/GAME.BIN).

### Next (root cause — still open)
Verify the 0x801Fxxxx DMA-staging contents (texture pixels + the per-frame CLUT at 0x801FCDC0) against
the oracle. Needs EXACT-frame alignment (this buffer is rebuilt per frame / per load), not just
scene-alignment — find the game's logic-frame counter (a RAM word incrementing once/logic-frame,
identical in both engines until divergence) and dump both at the same value, OR break on the
fill routine. Then diff the staged texture/CLUT bytes: a mismatch pins the mistranslated
processing instruction. (mem_swr fix is committed but is NOT the cause; rasterizer, CD-read,
XA-interleave, unaligned-SWR, and raw-load all ruled out.)

## 2026-06-15 (later 59) — GAMEPLAY CORRUPTION ISOLATED: it is NOT the rasterizer — the recompiled CPU/HLE produces a PROGRESSIVELY-CORRUPT GP0 stream (bad RAM-sourced texture/CLUT data). Beetle reproduces our garbage byte-near-identically; the oracle running the real game is clean.
User (windowed, ground truth): in-game graphics "grow more and more garbage as you play" — garbage
blocks, missing sprites, melted geometry; fishing-rod teleports in the non-FMV intro cutscene. Also
asked to **sync native vs oracle at attract** and observe. (Audio: attract has NO BGM in BOTH native
and oracle = normal; only the real game has gameplay BGM — that's the SEPARATE streamed-CD-audio gap,
later-54, untouched here. Menu-cursor tempo (later-58) USER-CONFIRMED correct.)

### Repro (headless attract — corrects the handoff's "attract renders coherently")
The attract **gameplay DEMO** reproduces it; the early intro (SCEA, character cutscenes, TOMBA 2
title) is coherent, but once the DEMO starts the scene is corrupt and **worsens over frames**.
- Native recomp port (`scratch/bin/tomba2_port`), headless 2500 logic frames → ~5087 presents:
  green-field demo lightly wrong by f~3345 (striped ground, garbled character, a black block),
  dungeon demo HEAVILY garbled by f5000 (character = vertical magenta/yellow rainbow bar).
  `scratch/logs/native_progression.png`, `native_fishing.png`, `native2_late.png`,
  `native2_f05000_full.png`.
- Oracle (`wide60rt`, full Beetle interpreter on the same disc) renders the SAME demo scene **clean**
  — smooth grass, clean sprites, no stripes/blocks: `scratch/logs/oracle_007000.png`. So the real
  game's draw stream is correct; ours is not.

### Decisive isolation (gpu_differ on captured GP0 traces — `tools/gpu_differ/`)
Captured our live GP0 stream at f3360 and f5000 (`PSXPORT_GPUTRACE`), replayed each through BOTH our
rasterizer (`replay_ours`) and Beetle (`wide60rt -gpureplay`), diffed VRAM:
- f3360: front buffer **IDENTICAL**, back buffer 165 px small-Δ (UV-round/dither residual, later-44).
- f5000 (heavily corrupted live): front buffer **IDENTICAL**, back buffer 248 px small-Δ only.
- Rendering both replays to PNG vs the LIVE frame: **all three identical, INCLUDING the garbled
  character** (`scratch/logs/replay_vs_live_5000.png`). Beetle faithfully reproduces our garbage from
  our own GP0 words.
⟹ **The rasterizer is correct.** The corruption is in the GP0 word stream itself, which is built from
main RAM (`gpu_dma2_*` read via `mem_r32`). The draw commands are structurally fine (POLYDUMP f5000:
textured gouraud tris, sane texpage/CLUT/geometry for the character) → the corruption is in the
**texture/CLUT DATA in VRAM**, uploaded from RAM. It **degrades over time** ⟹ progressive **RAM
corruption by the recompiled MAIN.EXE / native HLE** (a recompiler mistranslation or a missing/wrong
HLE the game relies on for memory/texture management). This is upstream of the GPU entirely.

### Tooling fixed
`tools/gpu_differ/replay_ours.c`: added stubs for `wide60_join_poly`/`ws_sx_dump` (added to
gpu_native.c by later-55/57) so the standalone differ links again. `tools/gpu_differ/build.sh` now
builds clean.

### Refinement (user ground-truth, 2026-06-15 — corrects "starts clean")
The gameplay demo is **corrupt from its FIRST frame** (f3345 already striped/garbled/black-block), not
"starts clean then degrades" — it's corrupt the instant gameplay textures come into use, then worsens.
Dividing line confirmed by capture: **SCEA screen, character close-up cutscenes, and the TOMBA 2 title
all render CLEAN**; only the **3D gameplay demos** corrupt (green-field AND dungeon both corrupt from
their first frames — `scratch/logs/transition.png` shows clean title f4150–4400 → already-corrupt
dungeon f4450+). User also reports the corruption hits **everything roughly together** (player +
terrain + sprites), not one sprite-load path first ⟹ **wholesale** corruption of a shared
texture/VRAM region, not a single sprite pipeline. Clean 2D screens use big flat sprites / few
textures; corrupt scenes are the 3D gameplay path (many textured polys + streamed sprite uploads),
which runs largely from the recompiled **DEMO.BIN/GAME.BIN overlays** — a prime suspect (overlay
recompilation or the gameplay texture-upload path). Whether corruption resets on scene load = TBD.

### Next (root cause — NOT yet found)
Need a **differential RAM trace** vs the oracle to find the FIRST divergence (frame + address), then
map to the mistranslated instruction / missing HLE. No lockstep recomp-vs-oracle RAM harness exists
yet; the hard part is determinism/alignment (different engines: recomp C vs mednafen interpreter —
timers/RNG/uninit RAM differ benignly). Build that next. Do NOT re-chase the rasterizer — it is ruled
out, bit-near-exact vs Beetle on real captured gameplay (f3360 + f5000).

## 2026-06-15 (later 58) — AUDIO TEMPO FIX: per-vblank work was running 1× per logic frame instead of quota× → audio at HALF real-time tempo windowed (user heard the menu-cursor tick too slow). later-54's "tick once" reasoning corrected.
User (windowed, ground truth): "menu cursor movement audio is playing at wrong tick … should be
faster tick." Diagnosis confirmed it's a general half-tempo bug, not menu-specific.

### Root cause (corrects later-54)
`spu_audio_frame()` advances the SPU by exactly ONE 1/60 s field (one vblank). later-54 ticked the
libsnd sequencer ONCE per `ov_frame_update` to "match" that one field. But **one `ov_frame_update` is
one LOGIC frame**, which spans `DAT_1f800235` (=quota=2 for Tomba2's 30 fps) vblanks of wall time. So
the per-vblank work — BOTH the sequencer tick AND the SPU field advance — was running 1× when it must
run quota× to hold the hardware 60 Hz rate in real time. Result: windowed (paced to quota/60 s per
frame) the SPU was fed at half rate and the sequencer ticked at 30 Hz → **everything at half tempo**.
The headless WAV HID this: its timeline is field-count, not wall-clock, so 1 tick : 1 field is still
60:60 = correct-sounding (why later-54's WAV check passed and the user OK'd that capture).

### Fix (`games_tomba2.c` ov_frame_update)
Run the per-vblank work `quota` times per logic frame: `for (v=0; v<quota; v++) { seq_tick;
spu_audio_frame(); }`. Adaptive — a true-60fps scene (quota=1) ticks once. Keeps the tick:field ratio
(hence BGM tempo) unchanged; fixes real-time playback. PSXPORT_T2_NOSEQTICK still opts out the tick.

### Verified (headless)
- WAV for 3200 frames is now **106.67 s** (was 53.33 s, later-54) = 2 fields/frame for quota=2 — the
  SPU now advances at real-time rate. tick:field ratio unchanged ⇒ BGM tempo preserved.
- **VRAM at f3000 byte-identical** to the pre-change baseline — the change is audio-isolated, no
  rendering regression. (Sequencer ticked 2× changes only audio RAM/SPU state, not the GPU path.)
- **Tempo correctness in real-time needs the USER's ears (windowed).** The headless WAV cannot certify
  wall-clock tempo (that's exactly what masked the bug). User to confirm the cursor tick now sounds right.

## 2026-06-15 (later 57) — 60fps: OBJECT→PRIMITIVE JOIN validated on the native path (92–99% of gameplay polys tag to an object). The design's #1 open risk is retired.
Built milestone 2 of the 60fps tier: the object-identity join the matcher is founded on.

### Mechanism (all in `wide60.c` + thin taps, gated PSXPORT_WIDE60)
- `games_tomba2.c` `ov_object_cull` overrides the per-object cull/LOD dispatcher **0x8007712C**
  (a0=object*, once/logic-frame per live drawable): sets `g_current_object = a0`, super-calls
  `gen_func_8007712C` unchanged, restores. Registered only when wide60 is on.
- `gte_beetle.c` `gte_op` RTP tap → `wide60_rtp(op)`: for each SXY the GTE just pushed (DR14 for
  RTPS, DR12/13/14 for RTPT) it stamps an **SXY→object grid** (epoch-stamped 1024×512, no per-frame
  memset) with `g_current_object`. So every projected vertex carries the object whose cull-subtree
  projected it.
- `gpu_native.c` `gp0_exec` polygon tap → `wide60_join_poly(v0.x,v0.y)`: joins each drawn poly's
  lead vertex to a captured SXY (±2px). Packet vertex coords are pre-draw-offset = the same space as
  the SXY-FIFO (confirmed), so the join is direct.

### Result (headless gameplay to f5000, `scratch/logs/wide60_join.log`)
**poly-join = 92–99% across gameplay** (f1000 98.1%, f2500 99.0%, f5000 98.1%; dips to ~77% on a
menu/transition frame). I.e. nearly all drawn 3D geometry is object-matchable; the unjoined remainder
is CPU-projected terrain / 2D HUD that snaps by design. This is the native-path confirmation the doc
asked for (emulator-era was 97%-within-2px). Caveat: a high join RATE proves viability, not that each
join picks the *correct* object vs a coincidental nearby SXY — within-object disambiguation
(draw-order + prim type + texpage + CLUT/uv-low) is the matcher's job (next milestone).

### Faithful-path safety
PSXPORT_WIDE60 unset → cull override not registered, all taps no-op; VRAM at f3000 byte-identical to
the pre-wide60 baseline (`cmp` PASS).

### Next (60fps, remaining)
Per-frame primitive capture (full display list A/B), the matcher (object-id key → token key → snap),
the transform-lerp+reproject (or screen-XY-lerp fallback) synthesizer, `gpu_replay_list` for the
in-between, and 60 Hz host present. No-flicker rule: unmatched geometry held at frame A.

## 2026-06-15 (later 56) — 60fps tier started: MEASURED Tomba2 = 30 fps (was only "believed"). Rate detector live in the port, gated, faithful path byte-identical.
Pivoted from widescreen (blocked, later-55) to the 60fps interpolation tier
(`docs/wide60_recomp_60fps.md`). First milestone = actually measure the logic rate.

### New file `runtime/recomp/wide60.c` (gated PSXPORT_WIDE60; additive)
Owns the wide60 capture/rate-detector/(future)matcher+synthesizer. Milestone-1 content:
- `wide60_geom_xy()` — folds the GTE's projected SXY (DR12/13/14 after RTPS/RTPT, tapped in
  `gte_beetle.c` `gte_op`) into a per-frame FNV-1a fingerprint. SXY output is **parity-invariant**
  (depends only on logic inputs, not the double-buffer draw target) — the right cadence signal.
- `wide60_frame_commit()` — per-logic-frame fence, called from `games_tomba2.c` `ov_frame_update`
  (the proven once-per-logic-iteration chokepoint). Feeds the lrate_proto detector: counts
  consecutive `ov_frame_update` calls with an identical projected-geometry set; modal run-length =
  content-change period. A frame with zero GTE ops (FMV/menu) folds a constant = HOLD (no spurious change).

### Measured (headless to f5500, gameplay; `scratch/logs/wide60_rate.log`)
**modal-period = 1 call/change, engine vblank quota (DAT_1f800235) = 2 vbl/frame → content ~30.0 fps.**
Votes unanimously period-1 (p1=3601, p2=p3=p4=0) — every logic iteration produces a fresh projected
set. So Tomba2's logic IS 30 fps and each frame is intended for 2 vblanks. **For 60 fps: synthesize
exactly ONE in-between per logic frame** (lerp transforms at t=0.5, reproject). This retires the
long-standing "Tomba2 framerate unverified — measure, don't assume" (CLAUDE.md).

### Faithful-path safety
With PSXPORT_WIDE60 unset, all taps are no-ops; VRAM at f3000 is **byte-identical** to the
pre-change baseline (`cmp` PASS). The 0.000% raster / audio fix are untouched.

### Next (60fps, remaining)
Per-object transform capture (tag RTP ops via the `0x8007712C` dispatcher → object id), primitive
capture/match by object id in `gpu_native.c`, the lerp+reproject synthesizer, and `gpu_replay_list`
to rasterize in-betweens. Then host-pace 60 Hz present. No-flicker rule: unmatched geometry held at
frame A, never half-interpolated.

## 2026-06-15 (later 55) — WIDESCREEN blocker found: VRAM is packed (textures abut the 320-wide FB) → the design doc's "wider buffer, square pixels, no stretch" is architecturally impossible. Measured the GTE projects ~14% of verts past the 4:3 edges.
Started the wide60 widescreen tier (handoff: do widescreen first). Built two RE tools and ran them
against real gameplay; the result overturns the central premise of `docs/wide60_recomp_widescreen.md`.

### Tools built (permanent)
- `tools/vram_png.py` — dump a raw 1024×512 16bpp VRAM (`PSXPORT_VRAMDUMP_AT`) to PNG, optional
  magenta outline of the displayed region. Used to read the live VRAM layout.
- `gte_beetle.c` `ws_sx_record`/`ws_sx_dump` (gated `PSXPORT_WS_SXHIST`) — histogram the screen-X the
  GTE writes to SXY-FIFO per RTPS/RTPT, dumped every 500 frames from `gpu_present`. Measures how much
  geometry the GTE projects outside the 320-wide window.

### What the GTE projects (gameplay f3000–5500, `scratch/logs/sxhist.log`)
Display window is [0,320). Of all projected vertices: **~10–15% land at SX<0, ~24–36% at SX≥320.**
Per-bucket (f5000): near-left band `[-64,0)` ≈ 3%, near-right band `[320,384)` ≈ 11%. So the GTE
DOES project substantial world geometry just past the 4:3 edges (with spikes at the saturation
extremes = behind-camera / off-screen rejects). Wide geometry is *computed* — the open question was
only whether it's drawable.

### The blocker (decisive — `scratch/screenshots/vram_f5000.png`)
The VRAM is **fully packed**: two 320-wide framebuffers stacked at (0,0) and (0,256) (~224 tall;
display flips between them — vramscan), and the **texture/CLUT atlas fills x≥320 immediately to the
right of the FBs**, across all rows. The only free VRAM is the thin vertical bands rows 224–255 /
480–511 — horizontal, not usable to widen a FB. Therefore:
- **You cannot widen the framebuffer to 427 in place** — x∈[320,427) is the live texture atlas;
  drawing world geometry there corrupts the textures it then samples.
- **The doc §5 "present a WS_WIDTH-wide region centered on the display origin" is wrong** — copying
  VRAM [s_disp_x−53 .. +374] copies the texture atlas as the right third of the screen (garbage).
- **The doc §1 IR1×0.75 multiply is ALSO wrong for a square present** — it squishes content 0.75×
  horizontally; presented 1:1 that's a thin/squished world (confirmed by first-principles + the
  prototype's own numbers). The 0.75 multiply only makes sense paired with a downstream 1.333×
  STRETCH (the Beetle hack) — which the doc explicitly set out to avoid.

### What real hor+ widescreen actually requires here (none are an autonomous win)
1. **Squish-stretch (Beetle hack, the only one that fits packed VRAM):** keep the 320 FB, apply the
   IR1×0.75 multiply (more world squished into 320), present-STRETCH 320→16:9. Fits VRAM, standard
   PSX-widescreen technique. Quality = native-h-res stretched (acceptable, not "better-than-Beetle").
   STILL needs the cull-cone widen to populate the new edge bands.
2. **Separate wide FB backing store (what the doc imagined "owning the rasterizer" buys):** render FB
   draws into our own 640-wide off-screen buffer at recentered coords while textures stay in s_vram.
   Feasible (we own gpu_native.c) but doubles rasterization and must mirror FB read-backs; and STILL
   needs the cull-cone widen.
3. **Relocate the texture atlas to free FB columns:** rewrite every texpage/CLUT coord the game
   programs — deep, fragile per-game RE.

**Common unavoidable dependency: the per-game CULL-CONE widen.** The six `slti` sites (§2 of the doc)
gate draw-enqueue in *world/camera* space, so the squish/recenter does NOT bring culled edge geometry
back — without widening them the new edge bands stay empty regardless of present strategy. And cull
widening is the exact change that previously caused walk-through ghosts (needs the visibility/activation
split RE **and the user's eyes** to validate — not completable while the user is away).

### Decision
Widescreen is NOT an autonomous-completable win: it forces the squish-stretch approach (overturning the
doc) and is hard-blocked on a risky per-game cull RE that needs user validation. Recorded here; the doc
is corrected. **Pivoting this session to the 60 fps interpolation tier** (`wide60_recomp_60fps.md`),
which renders into the existing FB (no VRAM-packing wall), is gated/additive (faithful path stays
byte-identical), and has a proven RE basis — the tractable, verifiable deliverable. First 60fps
milestone: actually MEASURE Tomba2's logic rate (a long-standing project unknown) via the rate detector.

## 2026-06-15 (later 54) — AUDIO FIXED: native libsnd sequencer tick → port plays SPU-voice music (was all-zeros)
Implemented the later-53 fix (user picked "native sequencer tick"). The port now produces music.

### What it was (one more RE step past later-53)
- The libsnd setup is `FUN_80090750 = SsSetTickMode`; runtime globals (read from a loaded state via
  the REPL): tick mode `DAT_800ac424 = 5`, sequencer pointer `DAT_800ac42c = 0x80090BD0`
  (`SsSeqCalled`), user-cb `DAT_800ac430 = 0x80086288`, `DAT_800ac434 = 0`. The VBlank IRQ runs the
  tick **wrapper `FUN_800909c0`** = (optional user cb) + `(*SsSeqCalled)()`.
- **Neither `0x800909c0` nor `0x80090bd0` is emitted by the static recompiler** — they're only ever
  reached through the IRQ callback pointer, never a direct `jal`, so the indirect-call discovery
  never saw them (classic recomp coverage gap). They run fine through the hybrid interpreter.

### Fix (games_tomba2.c `ov_frame_update`)
Call `rec_dispatch(c, 0x800909C0)` once per `ov_frame_update` (→ interpreter, bit-identical, runs
the wrapper to its `jr ra`). This is "port the HW interrupt work to PC" (the busy-wait-porting
rule), NOT IRQ simulation. Guarded on the sequencer pointer `mem_r32(0x800AC42C)` being a sane code
address so we never call through null before `SsStart`. Opt out (A/B): `PSXPORT_T2_NOSEQTICK`.
- **Tick rate = ONCE per `ov_frame_update`, NOT VBLANK_QUOTA(=2) times.** `spu_audio_frame()`
  advances the SPU exactly one 1/60 s field per call (3200 frames = 53.33 s audio = 60 fields/s),
  and on hardware the sequencer ticks once per VBlank (60 Hz realtime) while the SPU plays
  realtime — so the faithful ratio is 1 tick per SPU field. The quota=2 is the game's 30 fps
  *display* pacing and is irrelevant to audio tempo vs SPU time; ticking twice plays music 2x fast
  (first cut did this — fixed).

### Verified (headless, `scratch/logs/ours_seqtick1x.log`, `scratch/wav/ours_seqtick1x.wav`)
- Port `PSXPORT_SPU_DBG`: per-note KON went from **~10 (init only) → thousands** (val=2000,1000,00CC,
  0010,0020,0003,…); `spu_render peak` 0 → 4691…21270 every frame.
- WAV: was **all-zeros**; now 53.33 s, **RMS ~4267, 94% non-silent, peak 21270** — same loudness
  band as the oracle's KON menu music (~3000, later-53). Reaches frame 3200 / gameplay, no crash.
- TEMPO is reasoned-correct (1 tick / SPU field = hardware 60 Hz). **User should confirm audibly**
  (windowed run, no `PSXPORT_NOAUDIO`) — the only thing a headless RMS check can't certify is the
  exact musical tempo/pitch.
- **No rendering regression:** full 1 MB VRAM dumped (`PSXPORT_VRAMDUMP_AT`) at f600 (menu) and
  f3000 (gameplay grass) is **byte-identical** with the sequencer tick ON vs `PSXPORT_T2_NOSEQTICK`
  — the fix is isolated to audio; geometry/raster (the 0.000% later-51/52 result) is untouched.

### Tooling hygiene
`vendor/beetle-psx spu.c` oracle SPU trace is now gated on `#ifdef __LIBRETRO__` (oracle-only) — the
same spu.c links into the port too, which has its own `spu_beetle.c` log; this stops double-logging
and keeps the port build free of the oracle-only `PSX_CPU` reference.

### Still open (smaller, separate)
FMV/menu CD audio through the SPU (`CDC_GetCDAudioSample` stub) is still silent on the SPU path, but
FMVs play via `native_fmv.c` (own device). If a scene wants CD-DA track 2 / XA mixed through the SPU
during gameplay, that remains a separate, smaller gap (later-53 candidate #1) — not the music bug.

## 2026-06-15 (later 53) — AUDIO root cause PINNED via oracle: music sequencer is TIMER-IRQ-driven; the port delivers no preemptive IRQ → silent. (corrects later-52's candidate order)
Built the missing oracle-audio tooling and used it to pin the silent-port root cause. The
answer corrects later-52's prioritisation: it is **candidate #3 (an IRQ-driven sequencer the
port never ticks)**, NOT candidate #1 (streamed CD music). Evidence, all cited below, gathered
with the new tooling.

### New tooling (committed)
- **Oracle WAV capture — `PSXPORT_WAV=path` in `runtime/main.cpp`** (`AudioBatchCb`): dumps the
  FULL-Beetle (`wide60rt`) mixed 44100 Hz stereo to WAV, headless, independent of `-play`/SDL.
  Same format the port's `spu_audio.c` writes, so `tools/audio_differ/compare.py` diffs them
  directly. This is the ground-truth reference the handoff said was needed.
- **Oracle SPU trace — `PSXPORT_SPU_DBG=1` in `vendor/beetle-psx/mednafen/psx/spu.c`** (fork):
  logs KON (with the interrupted CPU `BACKED_PC` of the writer), SPUCNT (enable/cdaudio/irq/xfer
  on change), and a per-10s `CDmix` counter (how many output samples actually had CD-audio
  enabled + nonzero). Mirrors the port's `PSXPORT_SPU_DBG`. Env-gated, harmless when unset.
- Port-side `PSXPORT_SPU_DBG` extended (`spu_beetle.c`): now also counts voice-region/KOFF/CDVol
  writes + periodic SUMMARY, and logs the SPUCNT CD-audio bit.

### What the oracle does (ground truth — `wide60rt`, real Beetle, boots Tomba2 normally)
Headless boot, Start held to skip logos (`scratch/inputs-skipintro-long.txt`), 9000 frames /150s
(`scratch/wav/oracle_boot_long.wav`, `scratch/logs/oracle_boot_long.log`):
- **0–100s = the opening FMVs**: heavy XA CD-audio. `CDmix` climbs to ~3.25M nonzero CD samples;
  SPUCNT briefly C001/C081 (cdaudio bit0=1) during playback. This is FMV/STR audio.
- **~120–160s = post-FMV menu/attract**: `CDmix` PLATEAUS (≈no new CD audio) yet per-10s RMS is
  ~2800–3200 at 78–99% non-silent (`scratch/win_rms.py`). So that audio is **SPU-VOICE music,
  not CD audio** — confirmed by 96 per-note KON events in this run.
- **KON writer PC histogram: 54×`pc=80050CE8`, 7×`80050CEC`, 6×`80050CE4`** — i.e. every per-note
  key-on is written while the CPU is parked in the **per-frame pace-dwell loop `0x80050CE4`**.
  That can only happen from a **preemptive interrupt handler** firing during the dwell.

### Why the port is silent (the mechanism)
- The game installs a custom IRQ dispatcher (`FUN_800016e4`) that services VBLANK, GPU, CDROM,
  DMA, **Timer0 (0xf0000005), Timer1/2 (0xf0000006), SIO, and SPU (0xf0000009)** event classes.
  The music sequencer runs inside one of the **Timer/SPU IRQ** handlers (it fires during the
  dwell — see the KON PCs above).
- The port models **only VBLANK** (`timing.c` delivers `0xF2000003/0xF0000001` once per `VSync`).
  It delivers **no Timer IRQ and no SPU IRQ**, and it **collapses the pace-dwell** itself
  (`games_tomba2.c ov_frame_update` sets the vblank counter so the dwell falls through). So the
  sequencer ISR has neither a trigger nor the dwell to fire in → **zero per-note KON → the SPU
  mixes silence** (verified: port `spu_render peak=0` every gameplay frame; only the all-voices
  init KON pattern + 162 KOFF housekeeping writes ever fire — `scratch/logs/audio_base.log`).
- **FMV audio is NOT this bug and already works**: the port decodes FMV XA natively in
  `native_fmv.c` through a *dedicated* SDL device, not the SPU mixer / `CDC_GetCDAudioSample`.
  So the FMVs (the most audio-rich part) play; the gap is the SPU-VOICE in-game/menu music.

### Disproven / corrected
- **later-52 candidate #1 (streamed CD music is the bulk gap) — WRONG ordering.** The non-FMV
  music is SPU-voice (KON), not CD audio. `CDC_GetCDAudioSample` being stubbed only costs FMV
  CD-audio-through-SPU (which the port routes around natively anyway). Wiring it would not bring
  back the menu/gameplay music.
- **VBlank callback `0x800506b4` is confirmed (disassembled, `generated/shard_6.c`) to be ONLY
  `lhu/addiu/sh` on the dwell counter `0x800E809C`** — a pure counter bump, NOT the sequencer.
  The journal was right about that; the sequencer is a *different*, Timer/SPU-IRQ handler.

### The proper fix (named; not yet implemented — scope decision for the user)
Make the port run Tomba2's sound-engine tick. Two routes:
1. **Preferred (matches the port's "port the busy-wait to PC, don't simulate the HW" rule):** find
   the sequencer tick entry (the libsnd `SsSeqCalled`-equivalent the Timer ISR calls) and invoke
   it from `ov_frame_update` at the timer's configured rate. Needs one more RE step (the EvCB
   registration for `0xf0000005/6` → callback addr) + the tick rate (so tempo is correct — naive
   once-per-frame would play slow).
2. **More faithful but bigger:** actually deliver the Timer (and SPU) IRQ events per frame at the
   modelled timer rate so the game's own dispatcher runs the ISR. This re-introduces preemptive
   IRQ delivery the port deliberately stripped — larger, with regression risk to the tuned HLE
   boot, so it needs sign-off.
Both need the timer rate to get tempo right; that is why this is a scoped subsystem task, not a
one-liner. Validate either against `oracle_boot_long.wav` (the menu-music window) via the differ.

## 2026-06-15 (later 52) — multi-frame GPU sweep (GPU is solid everywhere) + audio tooling + audio is SILENT (root-caused)
User: "you only compared the first few things… also add audio and audio compare tooling." Did both.

### GPU: validated across 8 scenes, not just one frame — no new structural bugs
Extended PSXPORT_GPUTRACE to capture a COMMA-SEPARATED frame LIST in one deterministic run
(`"600,1000,..,3150:prefix"` → `prefix_f<N>.bin`; single-frame exact-path form unchanged, differ
pipeline intact). Swept f600..f3150 (menu→gameplay), replayed each through ours + Beetle, diffed
both buffers. Result: **the drawn buffer is 0.000% (pixel-identical to the oracle) on ALL 8 frames**
(back buffer 0,256 when it holds the frame; the other buffer shows the same ≤0.4% sub-pixel
affine/edge floor from later-51). So after later-51 there are NO new structural GPU divergences
across scenes — the rasterizer matches the oracle everywhere sampled. (scratch/bin/sweep/, diff_all.sh.)

### Audio tooling added (user ask)
- **PSXPORT_WAV=path** (spu_audio.c): dumps the SPU's mixed 44100 Hz stereo output to WAV,
  INDEPENDENT of SDL — works headless / under PSXPORT_NOAUDIO (the mixer is advanced + drained
  regardless; refactored spu_audio_frame so the SPU runs when EITHER the SDL device OR a WAV
  capture is active). Header patched at exit, ~10 min cap.
- **tools/audio_differ/compare.py**: `stats` (duration, RMS/peak, silence %, first-audible — flags
  all-zeros) and `diff` (xcorr-align ours-vs-oracle then loudness/peak deltas; alignment-free like
  the GP0 differ since HLE vs full-emu don't sample-align). Pure stdlib.
- **PSXPORT_SPU_DBG=1** (spu_beetle.c): register-write count, SPUCNT (enable+xfer mode), KON masks,
  SPU-RAM DMA/data-port transfers, per-frame spu_render peak — to separate a mixing bug from a
  driver/IRQ problem.

### FINDING: the port is SILENT — VERIFIED facts + an HONEST (not-yet-pinned) root cause
A 55 s headless gameplay capture (incl. the f3000 grass scene) is **all zeros**. PSXPORT_SPU_DBG
shows the SPU itself is healthy: enabled (SPUCNT bit15=1), master volume 0x3FFF, SPU RAM uploaded
(359 DMA writes × 256 words ≈ 368 KB), voices keyed on at init. VERIFIED facts:
- Over 2500 frames only **5 KON writes, all the all-24-voices init pattern (0x188=FFFF/0x18A=00FF)
  — ZERO per-note key-ons**. The SPU mixes 735 zero samples/frame.
- Steady SPUCNT = C020 / C0A0: **SPU-IRQ-enable (bit6) = 0** and **CD-audio-enable (bit0) = 0**.
  So the sound path is NOT driven by the SPU IRQ (this DISPROVES my first guess that the stubbed
  `IRQ_Assert` was the cause) and CD audio is not steadily mixed through the SPU here either (bit0
  flips to 1 only briefly — C001/C0A1, a few times).
- The game's registered VBlank callback (0x800506B4, per games_tomba2.c) only increments the pacing
  counter — so the sequencer is NOT the VBlank callback either. (Don't chase "wire VSyncCallback".)
ROOT CAUSE NOT YET PINNED (deliberately not guessing / not bandaiding). Remaining candidates, in
order, for the next focused audio-RE session:
1. The music is likely STREAMED (XA-ADPCM / CD-DA) — that whole path (CD sector stream → SPU CD
   input / MDEC-audio) is stubbed: `CDC_GetCDAudioSample` returns silence. This is the most concrete
   known gap and probably the bulk of the missing audio.
2. SFX are SPU-sequenced; in the FORCED-INPUT (FFF7) attract state the game may simply trigger no
   SFX (no per-note KON because no SFX event), which would be partly EXPECTED, not a bug. Needs a
   run with real SFX-triggering input to tell "broken" from "nothing to play".
3. If a sequencer IS expected to run and doesn't, suspect a hardware Timer (RootCounter) IRQ tick we
   don't deliver — verify by checking Timer setup + whether a sound-update runs off it.
NOT attempted: any fix. Naming it per "no bandaids" rather than shipping a speculative call. GOTCHA
recorded: runs must go through run.sh (sets disc/BIOS/MAIN env); invoking scratch/bin/tomba2_port
directly does NOT boot the game, which briefly masked the SPU-RAM upload activity during triage.

## 2026-06-15 (later 51) — rasterizer fidelity: exact mednafen coverage + raw-texture poly bit → 2.57%→0.29% (a grass frame to 0.000%)
Continued the oracle GP0 differ chase (handoff scratch/handoff.md). Drove the GAME back-buffer
real-divergence (|Δ|>3, dither-filtered, region 0,256,320,240 on the f3000 banner frame) from the
later-45 residual **2.57% → 0.29%**, and a fresh live-captured grass frame to **0.000% (pixel-identical
to Beetle)**. Two root causes, both found via the differ + per-pixel PIXTRACE on both renderers, never
eyeballing:

### 1. Triangle coverage — ported mednafen's EXACT integer scanline edge-walk (the #1 handoff lead)
The absdiff was dominated by the "Go to the Burning House!" text-banner letter edges. PIXTRACE @64,308
(both renderers): our dark banner-board prim covered the pixel and overwrote the white letter; Beetle's
same prim stopped one pixel short. Our `tri()` used a half-space test keeping every pixel with all
edge-functions `wi>=0` — i.e. RIGHT/BOTTOM edges inclusive — so prims were one pixel "fat" and a later
prim mis-claimed a shared edge. A generic top-left fill rule got the direction right (2.57%→2.11%) but
not mednafen's exact sub-pixel endpoint rounding, so it over-fixed some edges and under-fixed others
(PIXTRACE @104,299 then showed ours MISSING a white pixel Beetle drew). Real fix: replicate Beetle/
mednafen's `DEFINE_DrawTriangle` verbatim — y-sort + core-vertex select, `MakePolyXFP`/`MakePolyXFPStep`
fixed-point edges, per-scanline span `[GetPolyXFP_Int(lc), GetPolyXFP_Int(rc))` (left/top inclusive,
right/bottom exclusive). Coverage now matches the oracle pixel-for-pixel. Kept our (already oracle-
validated) barycentric per-pixel shading by extracting it into `tri_px()` and driving it from the
ported walk (coverage and shading decoupled). **2.57% → 1.06%.**

### 2. Raw-texture polygons (GP0 cmd bit0 = texture-blend-disable) — the polygon path ignored it
The remaining residual had a systematic spike: 321 px at exactly |Δ|=9, all where Beetle output a
constant raw texel (e.g. 2E12 = (18,16,11)) while ours output a darker, dithered value. POLYDUMP @105,304
identified the prim: **op 0x2D** (textured quad, bit0 set). For polygons, bit0=1 means RAW TEXTURE —
output the texel verbatim, NO modulation by vertex/command color and NO dither. Beetle's TM0 template
does this; our polygon path always modulated (texel × color / 128) + dithered, so a raw texel 2E12 got
multiplied by the command color (168,72,31) → near-black. This is the SAME bit0 gating the sprite path
already honored (commit fb0c228); the polygon path was missing it. Fix (gpu_native.c): pass a `raw` flag
(`textured && (op&1)`) into `tri`/`tri_px`; when set, skip modulation and dither. **1.06% → 0.29%.**

### Residual = the affine/edge model floor (NOT chased further — diffuse, sub-pixel)
The leftover 0.29% (220 px on the banner frame) is diffuse single-pixel speckle, no structure. PIXTRACE
spot-checks: @8,359 the SAME prim samples a different texel (ours 1A12 vs Beetle 53DE) — affine-UV
sub-texel difference, our per-pixel barycentric UV vs Beetle's incremental fixed-point DDA can land on
adjacent texels at scattered pixels; @288,350 a 4th prim covers a boundary pixel in Beetle but not ours
(residual edge case). Eliminating these would require porting Beetle's per-pixel DDA interpolation
(CalcIDeltas + i_group stepping) wholesale for shading too — a large change for ~0.2%; left as the model
limit. The fresh live grass frame (no banner) is already 0.000%.

VERIFIED (oracle ground truth, headless): differ on f3000 banner frame 2.57%→0.29%; fresh live-captured
f3000 grass frame ours-vs-Beetle = **0.000% IDENTICAL**. Live port (scratch/bin/tomba2_port via run.sh)
rebuilt, ran headless to f3200, reaches GAME and renders — no crash/regression (the diff covers the whole
back buffer: grass/sprites/shadow all unregressed, all improved). Note: the live port is built by run.sh
(compiles runtime/recomp/*.c incl. gpu_native.c with the recompiled shards), NOT `make -C runtime` (that
Makefile's OBJS omits recomp/; it's a stale/secondary path). The differ's replay_ours uses gpu_native.c
standalone via tools/gpu_differ/build.sh.

## 2026-06-15 (later 50) — FIXED the menu-load flicker: only flip the double buffer when a frame drew
User: "flicker is not acceptable." Tooling (GPU_DUMP per-frame luma + GPU_LOG prim counts) pinned it:
during the ~17-frame menu-load gap after the FMVs the game produces NO draw prims (it's streaming in
a background image), but the native frame loop (native_boot.c) flipped the display double buffer
EVERY frame, alternating the display between the one buffer that has content and the still-black
other buffer → black/content/black/content flicker. In steady gameplay/menus every frame draws
(prims>0) so the flip is normal; only the load gap flickered.
- **Fix:** gate the buffer flip on `gpu_prims_since_present() > 0` (new accessor in gpu_native.c) —
  only flip to a buffer we actually drew this frame; otherwise HOLD the last completed front buffer.
  Same "never present an undrawn buffer" rule as the SCEA blink (later-46). During the load the
  display now holds the last good frame (the last OP FMV frame in the real flow) until the menu is
  drawn, instead of flickering.
- **Verified headless:** menu-load region went from alternating black/content to steady (no
  interspersed black), then the title menu renders cleanly; GAME stage still reached and renders the
  level — no regression to gameplay (which draws every frame).

## 2026-06-15 (later 49) — SCEA: Start-skip + stop the CD reads (kill the "CdRead: retry" loop)
User asks: SCEA skippable via Start; and SCEA should do NO CD reads — just cut to the LOGO FMV when
it ends. Both done (native_stub.c).
- **Start-skip:** ov_stub_vsync polls host input (pad_poll_sdl when windowed; pad_buttons() bit 0x0008
  = Start, active-low); on Start it does the MAIN.EXE hand-off immediately (load_exe_image +
  longjmp(g_stub_exit), same as ov_loadexec) — skipping the fades. PSXPORT_SCEA_SKIP forces it
  (headless/always); PSXPORT_NOSKIP disables. Verified: PSXPORT_SCEA_SKIP=1 hands off on the first
  VSync → straight to FMV/DEMO (also skips the CD-init retries).
- **No CD reads:** the "CdRead: retry..." spam (~20×) came from the stub's CdRead (0x8001BA64),
  whose caller (0x8001B89C) retries while the CD-status word (CD struct 0x80026BF8 +20 = 0x80026C0C)
  is negative — and our runtime serves the stub no CD data. RE'd it: caller `bgez`-checks that word;
  the original success path stores the VSync count there. Replaced CdRead with a no-op that writes a
  positive status (mimics success) so the caller stops retrying. The SCEA screen needs no CD data
  (FMVs play natively, MAIN is LoadExec'd from file). Verified headless: "CdRead: retry" count 20→0,
  SCEA still renders both lines correctly, reaches LOGO FMV → DEMO.
NEXT: the menu-load double-buffer flicker (firm "not acceptable") — fix the game-loop flip to not
present an undrawn buffer.

## 2026-06-15 (later 48) — SCEA fade pacing (real-vblank model) + the "headless still headed" bug
1. **PSXPORT_NOWINDOW didn't actually go headless.** present_window() gated the SDL window on mere
   getenv PRESENCE: `s_win_on = getenv("PSXPORT_GPU_WINDOW") ? 1 : 0`. But run.sh ALWAYS sets that
   var (to "0" when headless), and "0" is a non-NULL string → truthy → a window opened even under
   PSXPORT_NOWINDOW=1. Fix (gpu_native.c): check the VALUE (`atoi(w)!=0`), matching gpu_pace_frame.
   Now headless runs open no window / no SDL video init.
2. **SCEA fades/holds ran far too fast (headed).** The stub times its fades by the VBlank count
   (DAT_800267B4 / VSync return). ov_stub_vsync advanced that count by 1 per CALL, but the stub
   busy-polls VSync(-1) many times per frame, so the count raced ahead at the poll rate and
   count-timed fades blew through in a few real frames. Fix (native_stub.c): when windowed, use a
   real-vblank model — the count = elapsed real time × 60/1000 (exactly like the hardware VBlank IRQ,
   independent of poll rate), and a frame-wait (mode>=0) sleeps to the next 60 Hz boundary before
   presenting. Headless keeps the fast per-call path (tests stay fast). Verified headless: no
   regression (305 SCEA frames, smooth 57-step fade-in, reaches DEMO). The real-time fade SPEED is a
   windowed-only behavior — needs on-device confirmation (can't be timed headless).
NEXT: issue still open — menu-load flicker after skipping OP. Tooling (seq_check dump) shows the
DEMO menu-load presents ALTERNATING black/content frames (~f347-355: black,52,black,52,…) =
double-buffer flicker in the native game loop (ov_frame_update flips to an undrawn buffer during the
load). Same CLASS as the SCEA single-buffer blink (later-46) but in the GAME loop; fix there next.

## 2026-06-15 (later 47) — WIRED the intro FMVs: SCEA→Woopee→OP→Menu now plays
Next intro issue (user): FMVs didn't play — boot went SCEA→Menu instead of SCEA→Woopee→OP→Menu.
Tooling diagnosis: a headless stage-log run showed "time out in strNext()" in the DEMO stage — the
game's own STR streaming (StrPlayer/strNext) times out under our runtime (we don't feed CD-streamed
FMV sectors), so the movies were skipped to a black gap. The self-contained native FMV player
(native_fmv.c, native_fmv_play) was fully implemented + decode-verified but had ZERO callers — never
wired into the boot flow.
- **Fix (native_boot.c native_boot_run):** before crt0, play `MOVIE/LOGO.STR` (Whoopee Camp logo)
  then `MOVIE/OP.STR` (opening) via native_fmv_play(). Gated by PSXPORT_NO_FMV (set it for headless
  gameplay/differ tests — FMVs otherwise add frames and shift the global gpu-frame counter).
- **Verified via headless GPU_DUMP + analysis (not eyeballing):** LOGO decodes to the pink/yellow
  "Whoopee Camp" logo on white (first ~40 frames are its white fade-in, then content); OP decodes to
  coherent Tomba video; after the FMVs the game boots START→DEMO and renders the TITLE MENU ("TOMBA!2
  THE EVIL SWINE RETURN", New Game/Load Game). Full chain SCEA→Woopee→OP→Menu confirmed
  (scratch/screenshots/fmv_view_logo.png, fmv_view_op.png, seq_menu.png).
- GOTCHA: native_fmv_play paces to audio by default; PSXPORT_FMV_FPS=0 = uncapped (headless dumps);
  PSXPORT_FMV_MAXFRAMES=N caps frames (fast checks). Movies are Start-skippable.
- Residual (not chased): the DEMO attract still logs "time out in strNext()" (its own in-attract
  stream); harmless — the game proceeds to the menu. Separate from the intro FMVs.

## 2026-06-15 (later 46) — FIXED the SCEA screen: clipped "Presents" + unnatural blink
User flagged the intro sequence is wrong (SCEA→Woopee→OP→Menu; ours does SCEA→Menu, FMVs skipped)
and to fix issues IN ORDER, comparing via TOOLING vs the oracle (never eyeballing). First issue =
the SCEA "Sony Computer Entertainment America / Presents" screen. Two distinct bugs, both fixed:

1. **"Presents" line clipped + text mispositioned.** Tooling: dumped our SCEA VRAM
   (PSXPORT_VRAMDUMP_AT) and the oracle's (PSXPORT_VRAMDUMP) — BYTE-similar content; both lines
   present in VRAM at y≈205-258, centered for a 480-line display. Our render only showed line 1 at
   the bottom. Root cause: SCEA runs in **480i** (GP1(0x08)=0x27: interlace bit5 + VRes-480 bit2),
   GP1(0x07) vertical range = 234 scanlines, but in 480i the field is shown twice so the displayed
   VRAM height is 2×(y1-y0)≈468. Our gpu_gp1 set s_disp_h = y1-y0 = 234, clipping the bottom
   (y≈245-258 = "Presents"). Fix (gpu_native.c gpu_gp1): track interlace+VRes from GP1(0x08) and
   double s_disp_h in 480i. 240p stages (DEMO/GAME, GP1(0x08)=0x01) unaffected.
2. **Unnatural flicker.** Tooling: per-frame mean-luma of the GPU_DUMP showed every 4th frame fully
   BLACK (≈76/305) between text frames. Correlating GPU_LOG (per-present prim/word counts) with a
   new PSXPORT_VSYNCLOG: the stub's per-frame body is VSync(0)[frame boundary] → clear framebuffer →
   busy-poll VSync(-1)×3 (timing/CD) → draw → loop. Our ov_stub_vsync presented on EVERY VSync call,
   so the VSync(-1) poll fired right after the clear and BEFORE the redraw → presented the cleared
   (black) buffer. Real HW refreshes only at the VBlank frame boundary (VSync(0)), never mid-draw.
   Fix (native_stub.c ov_stub_vsync): present ONLY on mode>=0 (real frame-waits); mode<0 polls just
   advance the count (so loops still terminate) + pet the watchdog. After: black frames 76→5, and
   the 5 are only the fade-in (f1-3) / fade-out (f304-5) boundaries — no interspersed flicker; the
   brightness sequence is a smooth fade matching the oracle.
New gated debug tools: PSXPORT_GP1LOG (GP1 command log), PSXPORT_VSYNCLOG (stub VSync mode log).
NEXT (per user): FMVs don't play — SCEA→Menu instead of SCEA→Woopee→OP→Menu. A verified native FMV
player exists (native_fmv.c); the START-stage path that should play \Woopee\OP STR FMVs is skipped.

## 2026-06-15 (later 45) — FIXED the UV sampling residual: affine UV now round-to-nearest
Follow-on to later-44, using the differ to land its named #1 residual. PSX/Beetle sample the texel
NEAREST to the affine (u,v) — Beetle seeds its affine interpolant with a +0.5-texel bias
(`+(1<<(COORD_FBS-1))`, gpu_polygon.c) then truncates = round-to-nearest. Our `tri()` integer-divided
the barycentric UV sum and TRUNCATED, biasing every sample half a texel toward the origin and picking
a neighbouring texel at fractional coords. **Fix (gpu_native.c):** round the affine UV to nearest
(add half the divisor, sign-normalized since `aa` may be negative). Verified via the differ on the
f3000 grass frame: GAME back-buffer real-divergence (|Δ|>3, dither filtered) **17.0% → 2.57%**; total
(incl. dither) 64% → 42%. Live port rebuilt, reaches GAME, renders clean & sharper, no regression
(scratch/screenshots/differ_f3000/live_uv_full.png). REMAINING residual: the ~2.6% |Δ|>3 is now
diffuse (triangle-edge coverage: our top-left fill rule vs Beetle's), and the ~40% |Δ|≤3 is the
dither-matrix mismatch (our custom s_dither4 vs Beetle dither_table/DitherLUT) — next differ targets.

## 2026-06-15 (later 44) — BUILT the GP0 differ + FIXED the "shadow = black wedge" bug
Two deliverables (handoff scratch/handoff.md), both done & verified.

### 1. The cross-renderer GP0 differ (tools/gpu_differ/) — feeds IDENTICAL GP0 to both rasterizers
The whole point: our HLE port and the full-emulation oracle run at different timings/states, so you
CANNOT align by frame number (this trap defeated every earlier screenshot compare). Instead capture
one frame's GP0 word stream + its start-of-frame VRAM from our port, replay it through BOTH our
renderer and Beetle's from the same initial VRAM, and diff the output VRAM — any pixel difference is
then a pure rasterizer-fidelity difference (modulation/blend/dither/coverage), no alignment needed.
- **Capture** (our port): `PSXPORT_GPUTRACE="FRAME[:path]"` (gpu_native.c) writes file `GP0TRC01` =
  initial VRAM + exact GP0 word stream for that gpu frame.
- **Beetle replay**: `wide60rt -gpureplay TRACE OUT` (main.cpp + `GPU_ReplayBegin` in gpu.c) seeds
  Beetle VRAM, replays words via `GPU_WriteDMA`, dumps Beetle VRAM. Software/1x only.
- **Ours replay**: `tools/gpu_differ/build.sh` → `scratch/bin/replay_ours TRACE OUT [VX VY]` links
  gpu_native.c standalone (stub mem_r*). `+VX VY` with `PSXPORT_PROVAT=1` prints the prim our
  renderer used to write that pixel (via new `gpu_prov_dump`).
- **Diff**: `tools/gpu_differ/diff.py OURS BEETLE [--region X,Y,W,H] [--tol N] [--ppm DIR]`.
  `--tol N` filters per-channel |Δ|≤N (dither/rounding noise) so real bugs surface.
- **Per-pixel math trace** (both renderers): `PSXPORT_PIXTRACE="VX,VY"` dumps the sampled texel +
  interpolated vertex color + modulated output for every prim writing that pixel — `[pixtrace ours]`
  (gpu_native.c) and `[pixtrace beetle]` (gpu_polygon.c). This is the per-pixel ground-truth differ.
- GOTCHA: gpu_polygon.c/gpu_sprite.c are `#include`d into gpu.c; the Makefile does NOT track that
  dep, so editing them needs `touch vendor/beetle-psx/mednafen/psx/gpu.c` before `make -C runtime`.

### 2. The bug: gouraud-textured modulation used vertex-A's color FLAT (not interpolated)
- The "soft shadow" Tomba/items cast on the grass is NOT a semi-transparent blend (the handoff's
  premise was WRONG). It's a **gouraud-textured OPAQUE quad** (GP0 op 0x3C, semi=0) that darkens the
  ground texture by a dark-at-center / bright-at-edges vertex-color gradient (PROVAT in the replay →
  op=3C tex=1 semi=0; PIXTRACE confirmed).
- **Root cause (gpu_native.c `tri()`):** the textured-modulation path computed `texel * a.r / 128`
  using `a.r` = the FIRST vertex's color held CONSTANT across the whole primitive, instead of the
  per-pixel barycentric-INTERPOLATED color (which the untextured gouraud path already did right). So
  the whole shadow quad was flat-modulated by v0's dark color (32,32,32) → a uniform near-black
  wedge, instead of a smooth gradient with grass showing through at the bright end. PSX (and Beetle's
  `ModTexel`, fed the interpolated r,g,b) ALWAYS modulate a textured polygon by the interpolated
  vertex color (flat-shaded prims carry the command color in every vertex, so this covers them too).
- **The fix:** interpolate (cr,cg,cb) via the same barycentric weights and modulate by that; dropped
  the `if (shade)` gate so flat-textured polys also modulate by their (uniform) command color — both
  match Beetle. Saturation-clamp (later-42) preserved.
- **Verified via the differ (oracle ground truth, not arithmetic/eyeball):**
  - Visual: the black wedge is gone; ours now shows soft darkened grass = Beetle
    (scratch/screenshots/differ_f3000/shadow_ours_fixed.png vs shadow_beetle.png).
  - Quantified: back-buffer real-divergence (|Δ|>3, dither filtered) **41.4% → 17.0%**; shadow region
    **48.2% → 17.5%**.
  - Per-pixel: PIXTRACE on interior shadow pixels shows ours' interpolated modulation color now MATCHES
    Beetle's (@130,455 exact (165,165,174); @140,452 (164,165,183) vs (164,165,184)).
  - Live port (real gameplay) rebuilt, reaches GAME, renders clean (live_fixed_full.png); grass
    (later-42) and sprites (later-43) unregressed.
- **RESIDUAL (next lead, NOT this bug):** the remaining ~17% is dominated by **UV sub-texel sampling**
  — Beetle adds a +0.5 round-to-nearest bias to affine u/v before truncation (gpu_polygon.c ~L688/697)
  while our `sample_tex` truncates, so we sample a neighbouring texel at fractional coords (PIXTRACE:
  same screen pixel, ours texel 0942 vs Beetle 0901). Plus the dither matrix differs (ours custom
  s_dither4 vs Beetle dither_table/DitherLUT). Both are pervasive low-amplitude fidelity gaps; fix UV
  rounding first (likely the biggest remaining win), then dither, using the differ.

## 2026-06-15 (later 43) — FIXED: sprite tinting = textured rectangles ignored the command color
Follow-on to later-42 (user confirmed "sprite tinting has issues"). Textured rectangles/sprites
(GP0 0x60-0x7F) on PSX modulate the texel by the command color (texel*color/128, saturated, 0x80 =
neutral) — there is NO raw-texture rectangle variant. Our sprite path wrote the **raw texel** and
ignored (cr,cg,cb), so any non-neutral-tinted sprite rendered at full brightness / wrong hue.
- **Found via tooling, not guesswork.** POLYDUMP (sprite path) showed that at a GAME frame 352/355
  textured sprites are neutral (128,128,128 = passthrough, unaffected) and a few carry a real tint —
  e.g. three 32x24 HUD-ish item sprites with command color (39,248,0) (heavy green). PROVAT on one
  (display (157,207), f3060) showed the output pixel = 0xB929 (72,72,112) raw purple = the un-modulated
  texel, drawn by op=65 primcol=(39,248,0). A green-glowing item was rendering purple.
- **The fix (gpu_native.c sprite path):** modulate texel by (cr,cg,cb) with saturation, same rule as
  the polygon path. Neutral sprites (color 128) are unchanged (texel*128/128 = texel).
- **Verified via tooling + visual:** after the fix PROVAT on the same pixel = 0x8222 (16,136,0) =
  exactly texel*(39,248,0)/128 then 5-bit-quantized (21,139,0 -> 16,136,0). The item now renders green
  (scratch/screenshots/spritefix/f03060_hud.png — green spiky item; was blue/purple). Grass still clean
  (later-42 grass-garish stays 0), level still loads/plays/reaches GAME — no regression.
- Remaining GPU faithfulness notes (not bugs hit yet): mask-bit test on VRAM reads is not modeled;
  sprite/poly blend modes assume the standard set. Revisit if a specific effect looks wrong.

## 2026-06-15 (later 42) — FIXED: grass red-blocks = uint8 overflow in texture-color modulation
Root-caused and fixed the scattered garish blocks on the grass (the bug later-39/40/41 circled but
never landed). **It was our rasterizer all along — a saturation bug in gouraud-textured modulation.**
- **Root cause (gpu_native.c `tri()`):** `r = r * a.r / 128` etc. with `r,g,bl` as `uint8_t`. PSX
  modulation is `texel * vertexcolor / 128` **saturated to 0xFF**; doing it in `uint8_t` instead
  WRAPS mod 256. A bright texel channel under a bright (>128) vertex color overflows 255 and wraps to
  a small value — so a bright-green grass texel becomes red. Exact reproduction: green grass texel
  (136,192,24) × vertex color (187,187,197) → R=198, G=192·187/128=**280→wraps to 24**, B=36 →
  output (192,24,32) = 0x9078 = the exact red pixel observed. Scattered because only the brightest
  texel+brightest-vertex combos exceed 255.
- **The fix:** compute the modulation wide (int) and clamp each channel to 255. One-liner, the only
  modulation site (the untextured gouraud path interpolates within the vertex-color range, no
  overflow). Sprites (GP0 0x60-0x7F) don't modulate by command color in our path — a separate
  faithfulness gap, not this bug; left as-is.
- **Verified:** grass-region garish pixels (lower-right, away from the legit-colorful banner/Tomba/
  house) went **402 → 0** at f03000/f03255; the level renders clean green grass matching the oracle
  (scratch/screenshots/redfix/f03000.png — clean, "Go to the Burning House!" quest banner + Tomba +
  items all correct). Level still loads, reaches GAME, and plays (no regression).
- **How the provenance tool cracked it (after a session of dead ends):** added `PSXPORT_PROVAT=x,y[:frame]`
  — a per-pixel "last writer" buffer (gid + frame + prim metadata) stamped in `put_px_b`, queried in
  DISPLAY space at present time. That sidesteps the double-buffer offset that had defeated every prior
  pixel→prim correlation, and immediately showed: the red pixel was actively drawn (age 1 frame, not
  stale) by a 4bpp gouraud prim using the all-GREEN grass palette — which is impossible unless the
  arithmetic mangles a green texel. From there the overflow was a 2-minute check. Lesson: build the
  provenance tool FIRST next time a "which prim drew this pixel" question comes up.
- **Earlier-session wrong leads (now closed):** the clut-(880,507) quad is the DEMO intro subtitle
  (later-41, fine); the (240,0,0) prims were Tomba's own model (later-41b); the clut-(1008,25x)
  palette diffs are minor cyan-shade shifts (later-41b, not this bug). All red herrings — the one real
  bug was the modulation overflow.
- **Durable tooling added across the session:** PSXPORT_PROVAT (per-pixel prim provenance + stale
  detection), PSXPORT_POLYDUMP/POLYAT (poly+sprite dump with vertex colors, OT node, point filter),
  PSXPORT_VRAMDUMP_AT, PSXPORT_STAGETL, PSXPORT_RAMDUMP_FRAME, PSXPORT_WWATCH (+coro g_interp_pc),
  PSXPORT_TEXTDBG; oracle POLYWATCH range + POLYCLUT.

## 2026-06-15 (later 41b) — narrowing the real grass garbage (user confirmed both our frames are wrong)
Follow-up to later-41 after the user confirmed the DEMO frame (f02790) is ALSO wrong (only the oracle
is clean) — i.e. the bug is in our grass-level rendering generally, present in both DEMO attract and
GAME. Narrowed but NOT yet root-caused. Honest state:
- **The garbage = tiny static red/garish specks amid correct green grass**, e.g. VRAM pixel 0x9078 =
  (192,24,32) red at a fixed screen spot, with green grass (clut 816,224 = all greens, byte-identical
  to oracle) immediately around it. Clusters of these specks read as "scattered blocks". The dark
  faceting is the same thing at the dark end.
- **Not wrong vertex colors / not wrong terrain VRAM.** The terrain prims covering a red speck are
  gouraud-textured with NORMAL gray-blue vertex colors and a pure-green palette (no red anywhere); the
  terrain texpages + cluts are byte-identical to the oracle. So the red is NOT produced by the grass
  prim. (The all-red (240,0,0) prims I first found at DEMO (121,96) were **Tomba's own model**, not
  garbage — don't chase those.)
- **There IS a real VRAM divergence, but it's minor and the wrong color:** a ~355-sprite layer (16x16,
  op 0x7C, clut (1008,250-255), tp (896,0)) is drawn every frame; its palettes (1008,253/254/255)
  differ from the oracle by 6-15 entries — but only as **subtle cyan-shade shifts** (e.g.
  (200,240,248) vs (160,240,248)), all cyan/white, never red. So that divergence is real but does NOT
  explain the red specks.
- **Leading hypothesis (unconfirmed): the red specks are STALE framebuffer VRAM revealed through gaps
  in our terrain coverage.** A red speck sits where no opaque terrain triangle lands (bbox overlaps but
  the triangle misses), so an earlier draw shows through; the oracle either fully covers it or clears
  the buffer and we don't. NOT yet proven — the double-buffer (GAME prims alternate off=(0,0)/(0,256);
  the displayed front buffer was drawn the prior native frame) confounded every pixel→prim correlation
  this session, so each "red pixel → covering prim" lookup dead-ended on a non-red palette.
- **NEXT (do this fresh, systematically):** (1) settle the offset/buffer mapping first (dump the BACK
  buffer that frame N draws at off=(0,256) → VRAM y+256, and only correlate within that buffer). (2)
  Decide stale-vs-drawn with a sentinel: fill VRAM with a unique color at boot — if the specks take the
  sentinel they're uncovered/stale (fix = framebuffer clear / terrain coverage), if they stay they're
  actively drawn (find the prim, incl. lines op 0x40-0x5F and the 0x7C sprite layer). (3) Add a sprite
  hook to the oracle (wide60rt currently hooks only polys) for a true prim-by-prim our-vs-oracle diff.
- **Tool added:** PSXPORT_POLYDUMP/POLYAT now also covers the sprite/rect path (GP0 0x60-0x7F), logs
  the OT node addr, and logs all 4 vertex colors for polys.

## 2026-06-15 (later 41) — later-40 FALSIFIED: the clut-(880,507) quad is the DEMO intro subtitle, not the level bug
Picked up the handoff (kill the "spurious title-overlay quad"). Traced the clut-(880,507) op-2D
quad end to end and **falsified later-40's root cause.** The real on-grass garbage in the playable
level is a *different* artifact that is NOT yet root-caused. Do not re-chase the clut-880,507 quad.

- **The clut-(880,507) quad is the DEMO-stage intro NARRATION SUBTITLE, and it renders CORRECTLY.**
  Built by the resident sprite/text renderer `FUN_8007E1B8` from a glyph string (descriptor base
  0x80158000, glyph idx ~149-164) at screen (43,172). It is the on-screen story text
  "Tomba is living peacefully in the country when Zippo finds a mysterious…" overlaid on the attract
  demo (scratch/screenshots/redcheck/f02790.png — readable cream text, not red dots).
- **It is drawn ONLY during the DEMO stage, never during GAME (the decisive correlation).** Stage =
  task0 entry @0x801fe00c: 0x801062E4 = DEMO, 0x8010637C = GAME. With `PSXPORT_WWATCH=800BFEDC,800BFEE0`
  (logs the store of the uv0|clut word 0x7EF71100) every one of the 16 builds is `pc=8007E67C
  stage=801062E4` (DEMO); **zero** in GAME. redpkt confirms: at the GPU frames where the quad emits
  (≈f2690-2800) `stage=0x801062E4`. The oracle draws clut-(880,507) **nowhere** at the level
  (`PSXPORT_POLYWATCH=6900-7400 PSXPORT_POLYCLUT=880,507` → no hits). So the quad is correct title/intro
  content, present in our DEMO, absent (correctly) from GAME. later-40's "title overlay leaks into the
  level" is **wrong** — there is no clut-880,507 prim in the level at all.
- **Stage timeline (PSXPORT_STAGETL, per gpu frame):** gpu f0-2600 task0entry=0 (boot/FMV/intro),
  ≈f2700 DEMO (intro narration over attract gameplay), f2800+ GAME (playable level). The earlier
  native-frame↔gpu-frame guesses in the handoff were off; trust STAGETL.
- **THE ACTUAL BUG (still open): scattered garish red/pink/purple/blue blocks on the GAME grass.**
  Confirmed a real divergence — the oracle's same level (scratch/screenshots/redcheck/oracle_level.png,
  oracle f7290) renders **clean** grass; ours (f03000.png/f03255.png) has scattered garish rectangles
  on the grass (Tomba's area). This is what the user actually sees; it is NOT the subtitle quad.
- **It is NOT a texture-VRAM divergence.** Dumped our VRAM at the level (`PSXPORT_VRAMDUMP_AT=3000:path`)
  and diffed vs the oracle's (scratch/bin/oracle_vram.bin) across the level texpages
  (576,0)/(448,256)/(768,0)/(960,256) and clut rows — **~0% diff (byte-identical)**. One garish grass
  pixel (121,101 magenta 168,0,128) is covered by an op-3C clut-(816,236) tp-(768,0) gouraud prim
  whose palette has NO magenta (yellows/browns/cyans/black, all == oracle) — so that prim can't be the
  source. The magenta must come from another prim/path not yet pinned: the **sprite path** (GP0
  0x60-0x7F, not in polydump), an op-2D prim with a magenta clut, or a semi-transparent blend artifact.
- **NEXT (real fix):** pin the garish prim source — extend the dump to the GP0 0x60-0x7F sprite path,
  and do an **offset-aware** prim-by-prim compare of our level display list vs the oracle's
  (PSXPORT_POLYDUMP/POLYAT here vs PSXPORT_POLYWATCH on wide60rt). Mind the double buffer: GAME prims
  alternate off=(0,0)/(0,256); the *displayed* frame (disp @0,0) is the off=(0,0) set, drawn the prior
  native frame — so a prim dumped at frame N with off=(0,256) shows at N+1. Find the extra/wrong prim,
  then root-cause (UV/vertex divergence from game logic, or a sprite-path rasterizer subtlety).
- **Tools added this session (durable, env-gated):** `PSXPORT_WWATCH=lo,hi` (mem.c — logs interp PC +
  stage of any store landing in [lo,hi); finds who builds a RAM/OT struct; needs the new per-insn
  `g_interp_pc` tracking now also in the coro interpreter). `PSXPORT_POLYDUMP=frame` [+ `PSXPORT_POLYAT=x,y`]
  (gpu_native.c — dump every poly at a frame: op/clut/tp/col/uv/verts/off, optionally only prims whose
  screen bbox covers (x,y)). `PSXPORT_VRAMDUMP_AT="frame:path"` (gpu_native.c — our 1024x512x16 VRAM at
  a frame, to diff vs the oracle dump). `PSXPORT_STAGETL` (gpu_native.c — per-gpu-frame stage timeline).
  `PSXPORT_RAMDUMP_FRAME=N` (native_boot.c — mid-run RAM dump; the 0x8010/0x8011xxxx overlay at gameplay
  differs from end-of-run). `PSXPORT_TEXTDBG` (interp.c — calls to the 2D text/row drawer 0x8007E998).
  redpkt now logs `node=` (OT RAM addr) + `stage=`. Oracle `PSXPORT_POLYWATCH` now takes a `lo-hi`
  range and `PSXPORT_POLYCLUT=x,y` filter (main.cpp).

## 2026-06-15 (later 40) — GRASS RED-BLOCKS root cause: our port draws an EXTRA title-overlay quad
**[SUPERSEDED by later 41 — this diagnosis is WRONG. The clut-(880,507) quad is the DEMO intro
subtitle and renders correctly; it is never drawn in the GAME level. The real on-grass garbage is a
separate, still-open bug. Kept below for the trail, but do not act on its NEXT.]**
Continued the later-39 oracle compare and **falsified its "wrong CLUT contents" hypothesis**, then
root-caused for real. The red blocks are NOT a VRAM or rasterizer bug — our port emits a spurious
primitive the real game doesn't.
- **The offending prim (our port, via PSXPORT_REDDBG + CLUTWATCH):** a flat RAW-textured quad
  `op=0x2D` (textured+quad+raw, opaque, no gouraud), clut=(880,507), texpage=(832,256), screen
  (43,172)-(139,188), uv (0,17)-(96,33) — a fixed-screen 96x16 strip at the bottom-left. The
  texture is mostly index 1 (palette[1]=0x0000 = transparent) with sparse index 15
  (palette[15]=0x0408 = dark red) ⇒ "scattered red dots". Drawn EVERY level frame at the same
  screen position (a 2D overlay, not a GTE world object).
- **VRAM is byte-identical to the oracle (hypothesis killed).** Added `PSXPORT_VRAMDUMP="frame:path"`
  to wide60rt (dumps mednafen's native 1024x512x16 `GPU_get_vram()` at 1x). At the level
  (oracle frame 7290) the oracle's palette @(880,507) = `...2C32 0408` and texture @(833,273) =
  `FFF1 FFFF 1111...` — EXACTLY ours. So the texture/CLUT contents are correct; palette[15]=red is
  correct data. The "pink/no-green palette" in later-39 is real but is NOT a grass palette and NOT
  wrongly placed — it's just unused-at-the-level data sitting in a reused VRAM slot.
- **The oracle does NOT draw this quad at the level (the decisive test).** Added `PSXPORT_POLYWATCH=frame`
  to wide60rt (registers the psxport_on_gpu_poly hook; logs every textured GP0 poly at `frame` with
  decoded clut/texpage + first 3 screen verts). At the oracle's level (f7290, 924 textured polys),
  the geometry around (43,172) is **gouraud-textured grass terrain** (cc=34/3E/3C, clut 800,210 /
  352,496 / 496,488 — none is clut 880,507, none is a flat cc=2D quad). So the oracle's level does
  not emit our red quad. The oracle DOES emit exactly this prim (cc=2D, clut 880,507, screen 43,172)
  — but only at **frames 1286-1324 = the TITLE SCREEN** (scratch/frames/oracle2/e1290.png). So the
  red-dot quad is a **title-screen 2D overlay element**.
- **Conclusion (verified):** our port draws a TITLE/menu 2D overlay quad DURING the level; the real
  game only draws it on the title. Since our port HLE-boots and uses a custom stage state machine
  (native_boot.c / FUN_80052078 transitions), a title/menu overlay object is not torn down when
  transitioning to GAME, so its per-frame draw persists into the level. It's a **game-logic /
  object-state divergence**, NOT GPU (sample_tex/blend/texwindow and all VRAM contents are correct).
  NB our port does NOT draw this quad at OUR title (redpkt's first hits are all level frames) — it
  appears ONLY at the level, consistent with state that's mis-set during the HLE stage transition.
- **Tools added this session (durable, env-gated):** `PSXPORT_REDDBG` + `PSXPORT_CLUTWATCH=x,y`
  (gpu_native.c — REDDBG logs the prim params + palette + texrow on a dark-red pixel, and (with
  CLUTWATCH set) the full GP0 packet `[redpkt]` for polys using the watched clut; CLUTWATCH alone
  logs VRAM uploads covering the watched CLUT row). `PSXPORT_VRAMDUMP="frame:path"` and
  `PSXPORT_POLYWATCH=frame` (wide60rt/main.cpp — oracle VRAM dump + per-frame textured-poly list).
  Oracle-to-level recipe: see later-39.
- **NEXT (the fix):** find the title/menu 2D-overlay object that emits the clut-(880,507) quad and
  why it stays active at the level. Approaches: (a) in GAME.bin, find the draw code that builds this
  fixed-screen quad (search for the clut/uv constants or the (43,172) screen coords) and trace what
  object/flag gates it; (b) check the stage-transition teardown (FUN_80052078 / the title→GAME
  object-list reset) — our custom scheduler likely skips a clear that the real boot does. Then
  deactivate/clear that object on entering GAME. Compare our display list vs the oracle's
  (PSXPORT_POLYWATCH) to confirm the extra prim is gone after the fix.

## 2026-06-15 (later 39) — ORACLE COMPARE: grass "red blocks" = wrong CLUT contents at (880,507)
User reported graphical errors in the now-playable level. Ran the **oracle** (Beetle/mednafen
`runtime/wide60rt`, real GPU) on the same disc to the same first jungle level and compared against
our native renderer (gpu_native.c, our OWN rasterizer). Result: our grass has scattered **dark-red
blocks**; the oracle's grass is **clean green grass-blade fringe** (scratch/screenshots/compare_grass.png,
oracle frame scratch/frames/oracle2/frame_007290.ppm). So the red blocks are a real bug in OUR
rendering, not game content.
- **Oracle run recipe (reaches the level in ~7300 emulated frames):**
  `PSXPORT_NOWIDE=1 PSXPORT_INTERNAL_RES=1x runtime/wide60rt "$TOMBA2_CHD" -bios bios -frames 12000
  -inputscript scratch/oracle_input.txt -dumpdir scratch/frames/oracle2 -dumpinterval 30`, where
  oracle_input.txt pulses Start (`<f> <f+8> Start` every 40 frames) to skip logos + select the menu.
  OpenBIOS works (no -fastboot). Oracle renders at 350x240; ours 320x224 — fine for visual compare.
- **Root-caused with a new probe `PSXPORT_REDDBG`** (gpu_native.c — logs a textured prim's
  mode/clut/texpage/uv + the 16 CLUT entries + the sampled texture row whenever it writes a dark-red
  pixel). The red blocks are: textured 4bpp polys, **neutral modulation color (0x808080)**, blend off,
  clut=(880,507), texpage=(832,256). The fringe texture is fine (indices 1 and 15: palette[1]=0x0000
  = transparent → grass shows through; palette[15] = the fringe color). The bug is the **CLUT
  contents**: our VRAM palette at (880,507) = `0000×8, 03FF, 7380, 7BBF, 727F, 699F, 60BF, 2C32, 0408`
  — a pink/purple/yellow set with **zero green**; palette[15]=0x0408 is the dark red we draw. The
  CLUT pointer + texture indices come from the SAME game GP0 packet on both runtimes, yet the oracle
  renders green ⇒ the oracle's VRAM at (880,507) holds a GREEN palette and ours holds this pink one.
  So our VRAM CLUT at (880,507) is WRONG (stale/overwritten/mis-placed) — a texture/CLUT **VRAM
  upload or ordering** bug, NOT a rasterizer-logic error (sample_tex / blend / texwindow are correct).
- **Ruled out:** `shade = gouraud || !textured` (gpu_native.c:199) skips command-color modulation for
  flat-textured polys — a real faithfulness gap, but NOT the cause here (the modulation color is the
  neutral 0x808080, so it's a no-op for these prims). Worth fixing separately for correctness.
- **NEXT (this bug):** find why (880,507) holds the wrong palette. Likely a VRAM CPU→VRAM transfer
  (GP0 0xA0) or DMA placed the grass palette elsewhere / a later upload overwrote it, due to our
  synchronous-CD + custom-scheduler upload ORDER differing from hardware. To pin it, either (a) add a
  VRAM dump to wide60rt (libretro GPU_RAM) and diff VRAM at (880,507)/(832,256) vs ours, or (b) log
  every CPU→VRAM/DMA write that touches the (880,507) CLUT row in our runtime and check what lands
  there last. Then fix at the transfer/ordering level.

## 2026-06-15 (later 38) — REAL PLAYABLE GAMEPLAY: async area-reader fix → level loads, plays, MOVES
later 37 OVERCLAIMED. The level-intro renders once (gpu ~f2850, native frame ~224) and then the
screen goes **permanently black** — the GAME state machine freezes at sm[48=2 4a=1 4c=2 4e=8] and
the framebuffer (VRAM x<320) stays black while the texture pages (x≥320) stay loaded
(`nonblack=252298` constant). later-37 only ran 240 native frames (the level appears at the very
END of that window) and misread the brief 4e-leaf cycling (9,10,7,8) as a "live game loop". It is
not — it halts at 8. (The 19 "CdRead: retry" log lines are all BOOT-time and do NOT grow during the
black region — a red herring; ruled out.) **This session fixes the real freeze; the game now loads
the level, renders sustained animating gameplay, takes input, and Tomba MOVES.**
- **Root cause — the engine's ASYNC area-data reader never completes (no-IRQ), so the level-load
  flag is never set.** The 4e=8 leaf (GAME.bin `FUN_80106f68`) is a clean wait: `if
  (DAT_1f80019b == 0) return;` (yield, loop) else advance to 4a=1/4c=2/4e=6. `DAT_1f80019b` is the
  area-load done flag. It is set by `FUN_8001db38` (task1's body) **only after `FUN_8001d940`
  RETURNS** (task+0x6c is already 1). `FUN_8001d940` (engine streaming reader) issues a raw libcd
  ReadN and loops (yielding each frame) until the remaining WORD count `_DAT_1f8001f4` hits 0 — but
  that count is decremented only by the per-sector data-ready IRQ callback `FUN_8001d7c4` (a plain
  CdGetSector copy into `_DAT_1f8001f8`). Our no-IRQ runtime never fires it ⇒ count never 0 ⇒
  reader never returns ⇒ flag never set ⇒ the 4e=8 wait spins (cooperatively — yields each frame, so
  no hard spin; the native loop keeps running, screen black).
- **Why the existing CD overrides missed it.** cd_override.c already replaces the *synchronous*
  loaders `FUN_8001db8c` / `FUN_8001dc40` with native reads. But the level uses the *async* path:
  `FUN_80044cd4` (fire-and-forget) / `FUN_80044bd4` (spawn+yield-wait) set `_DAT_1f8001f0`(LBA) /
  `_DAT_1f8001f4`(words) / `_DAT_1f8001f8`(dest) and spawn task1 with entry `FUN_8001db38` →
  `FUN_8001d940` **directly** (`FUN_80051f14(1, FUN_8001db38)`), bypassing the overridden loaders.
- **Fix — `ov_cd_async_read` (cd_override.c, overrides `FUN_8001D940`).** Do the read natively &
  synchronously: copy `_DAT_1f8001f4 * 4` bytes from consecutive sectors at LBA `_DAT_1f8001f0`
  into `_DAT_1f8001f8` (word-granular, exactly as `FUN_8001d7c4` does: 0x200 words = 1 sector =
  2048 B; dest advances by words*4, no sector padding), then zero the count and set the position
  tracker `DAT_800be0e0 = last sector`. The reader returns immediately, `FUN_8001db38` sets
  `DAT_1f80019b = 1`, task1 ends, and the GAME advances. (`FUN_8001D940` is recompiled — index 14 —
  so the override fires even though task1 runs in the flat interpreter: interpreted `jal 0x8001d940`
  → `call_addr` → `is_recompiled` → `rec_dispatch` → `func_8001D940` → `g_override[14]`.)
- **RESULT (verified, headless, FORCE_BUTTONS=FFF7):** at native frame 224 the 4e=8 wait now
  advances (4e=8→6, 4c→1→2, then normal play). Over a 1500-native-frame run (4156 gpu frames),
  **0 near-black frames** in the gameplay region (≥f2900, 1256 frames); scene non-black count varies
  continuously (26k↔71k) = real animation. Frame dumps show a fully rendered jungle level (Tomba,
  trees, house, sky — scratch/screenshots/fix/s03200.png), not the old intro flash.
- **Interactive control + movement VERIFIED.** Pulsing Start in-level opens the in-game pause menu
  (Options / Load data / Quit game — scratch/screenshots/long2/late.png), which only appears during
  real gameplay. Holding Right (PSXPORT_FORCE_HOLD=FFDF FORCE_HOLD_AT=240) scrolls the level: the
  scene moves to a new area between f2950 and f3150 (scratch/screenshots/move/m02950.png vs
  m03150.png). So menu→load→level→play→move all work on input.
- **Tools added this session (durable, env-gated):** `PSXPORT_SCHEDDBG` (native_boot.c — per-slot
  task resume_pc/ra/sp each scheduler step; THE way to locate a cooperative yield-wait: a parked
  task resumes at 0x80051FA4, then read its caller RA off the task stack at sp+0x10 to find the
  real waiting loop). `PSXPORT_RAMDUMP=<path>` (native_boot.c — dumps 2MB main RAM at end of run;
  then `tools/disasm.py <dump> <start> <end>` to read live overlay code, e.g. the 0x8011xxxx level
  overlay absent from the GAME.bin.asm dump). gpu_native.c now `mkdir -p`s PSXPORT_GPU_DUMP (was a
  silent no-op when the dir didn't exist).
- **NEXT:** (1) Audio (tests run PSXPORT_NOAUDIO — verify SPU/XA in real play). (2) wide60: the
  paced 30fps → 60 interpolation (project headline; see [[wide60-scope-decision]]). (3) Play deeper
  — confirm no later async-read or event stalls in subsequent areas/levels.

## 2026-06-15 (later 37) — GAME RENDERS GAMEPLAY: CD-streaming spin fixed → level loads + draws
The GAME-black residual from later 36 is **fixed** — the in-game level now loads and renders
actual 3D gameplay (scratch/screenshots/game_fix/view_2850.png: a Tomba2 jungle level — foliage,
water, character). Root cause was a non-yielding busy-wait in the CD *streaming* reader, not a
render bug.
- **Root cause — `FUN_8001cfc8` (CD streaming task, slot 2) busy-spun forever, wedging the whole
  frame.** When GAME enters its load sub-state it spawns the area/XA streaming task
  `FUN_8001cfc8` (via `FUN_8001d364`→`FUN_8001d2a8`→`FUN_80051f14(2,…)`). That task SEEKS the
  drive to a target sector and then polls the drive head position — `FUN_8001ce90(0x10)` =
  **GetlocL** → result MSF → `FUN_8008a110` (MSF→LBA) — looping until the head reaches the target
  window [task2+0x54 .. +0x58]. We serve all CD data synchronously and model **no drive motion**,
  so our CD-command override left the GetlocL result MSF **zeroed**; `FUN_8008a110(00:00:00)` =
  −150 = 0xFFFFFF6A is never in range → the poll loop (which does NOT yield) spins forever, so
  `native_scheduler_step` never returns → the frame loop is stuck at native frame 71, no present,
  black screen. (The GAME logic in slot 0 had already advanced fine; it only *looked* like a
  render/state hang.)
- **Found with a new durable tool — `PSXPORT_SPINDBG`** (interp.c): rec_coro_run counts
  iterations; if it runs ~80M without yielding/returning it dumps the looping pc window + branch
  regs (+ the CD-stream contract). It pinned the spin to the FUN_8001cfc8 GetlocL poll
  (pc 0x8001CE04..0x8008A188, a0=0xFFFFFF6A). Keep this — it localizes any future non-yielding
  busy-wait instantly (gdb shows pc `<optimized out>`).
- **Fix — `ov_cd_cmd_stream` (cd_override.c, overrides `FUN_8001CE90`):** report the drive AT the
  requested sector immediately (we don't model seek latency). For GetlocL (cmd 0x10) fill the
  result buffer with the BCD MSF of the stream's target start LBA (task2+0x54), so the head is
  "in range"; FUN_8001cfc8 then proceeds into its normal per-frame **yielding** read loop instead
  of spinning. Other streaming commands report success (matches our synchronous-CD model). Only
  the FUN_8001ce90 wrapper is intercepted — FUN_8001d940's reader calls FUN_8001ce04 directly and
  is untouched. NB: this stream copies no data to RAM (dest/words = 0 — it's XA/auto-routed), so
  not modeling its payload is correct for level data; only the seek-position needed unblocking.
- **RESULT (verified, headless, FORCE_BUTTONS=FFF7):** past frame 71 the GAME state machine
  advances normally (0x50 1→4, then 0x4c/0x4a/0x4e cycling = live game loop, no spin), VRAMSCAN
  framebuffer bbox goes (0,0)-(1023,511) (was all x≥320 = textures only), and frame dumps show the
  title menu, a "Please wait" load, then a **rendered 3D level**.
- **Rendering is SMOOTH (no flicker).** Consecutive frame dumps show the scene-pixel count
  ramping continuously (f2840..f2855 ≈47k→63k, then back down) as the camera pans / level
  animates — NOT a per-frame scene↔black alternation. The occasional all-black dump (f2890) is a
  normal fade transition, not a dropped buffer. (Earlier "flicker" suspicion was sampling-bias —
  the sampled frames happened to land on fade endpoints. Corrected.)
- **Speed is a NON-ISSUE — the interp runs GAME at ~3000 fps headless** (measured: 3000 native
  frames incl. boot in ~1.0s; a 100000-frame run completes cleanly, no spin). The earlier
  "~13fps / recompile overlays for speed" guess was WRONG — it was confounded by PSXPORT_GPU_DUMP
  PPM disk I/O (one file write per present) and tool-call timing, not interp cost. Do NOT invest
  in recompiling GAME.BIN/SOP.BIN for speed; the flat interp is ~50× faster than needed for 60fps.
- **Frame pacing DONE — windowed now runs at the game's rate (~30fps), headless stays full speed.**
  `gpu_pace_frame()` (gpu_native.c), called once per native game-frame from `ov_frame_update`
  (games_tomba2.c, NOT from gpu_present — the boot stub drives many presents/frame and pacing
  those stalled the boot), SDL_Delay-throttles to the engine's vblank quota DAT_1f800235 (=2 =>
  30fps). Gated on PSXPORT_GPU_WINDOW != "0" (run.sh sets it to "0" headless — must check the VALUE,
  getenv presence is truthy for "0"); PSXPORT_NOPACE disables (fast-forward). Verified: windowed
  120 frames in ~5s (~30fps, reaches GAME, no crash); headless 3000 frames in ~1.0s (unpaced).
- **GAME progresses through real game flow.** Frame dumps show: title menu (Start Game/Options) →
  "Please wait" load → GAME stage → the **level intro cutscene with story text** ("Tomba is living
  peacefully in the country when Zippo finds a mysterious letter addressed to Tomba." —
  scratch/screenshots/move_test/mv_2847.png) → pulsed Start advances the dialogue → the level view
  (the clean jungle scene, view_2850). So menu/load/cutscene/level all work; input drives the flow.
- **Input test hooks (pad_input.c).** `PSXPORT_FORCE_BUTTONS=<hex>` pulses an active-low mask (edges,
  for menus/dialogue). New: `PSXPORT_FORCE_HOLD=<hex>` + `PSXPORT_FORCE_HOLD_AT=<frame>` HOLD a mask
  continuously from a native frame onward (movement input) — e.g. reach GAME via FFF7 then hold Right
  (FFDF) in-level. NB holding a non-confirm button in a cutscene freezes it (no Start edge to advance
  the text), so to reach interactive play, keep pulsing the confirm button until control begins.
- **NEXT — interactivity + audio:**
  1. Reach interactive control: pulse the confirm button past the intro cutscene(s) until Tomba is
     player-controlled, then verify movement (windowed with a controller is the natural check;
     headless, FORCE_HOLD a direction once control begins and watch the scene scroll).
  2. Audio (tests run PSXPORT_NOAUDIO).
  3. wide60: interpolate the paced 30fps to 60 (the project's headline feature).

## 2026-06-15 (later 36) — INPUT WORKS + OTC DMA fix → title→menu→GAME stage loads (no hang)
Two native-MAIN residuals from later-33 fixed; the boot now drives title → menu → **GAME stage**
on real pad input. Verified headless (state log + frame dumps).
- **Pad input was entirely dead in the native MAIN loop.** The game reads its slot-0 pad packet
  from the FIXED global `DAT_800BF4F8` (status/id/btnlo/btnhi) in `FUN_800524b4`; that buffer is
  filled per-VBlank by the SIO read `FUN_80003A4C`, hooked into the VBlank IRQ by StartPAD. Our
  no-IRQ runtime never fires it, AND `FUN_80003A4C`/`FUN_800040c4` (InitPAD) live in low text
  (0x80004xxx) BELOW MAIN.EXE so they're dead interp misses — the SIO pointer table at `0x0000AEC8`
  stays NULL (verified aec8==0 at boot), buffer status stays 0xFF (no-pad). FIX: new
  `pad_service_frame()` (pad_input.c), called once per frame in the native loop BEFORE the game
  reads input — polls host SDL input and writes the standard digital packet (status 0, id 0x41,
  buttons) straight to the fixed buffers `DAT_800BF4F8`/`51A` (the addrs `FUN_800520e0` passes to
  InitPAD via `FUN_80088b00`). Slot1 forced to 0xFF (no 2nd pad). Headless test hook
  `PSXPORT_FORCE_BUTTONS=<active-low-hex>` PULSES the mask (8 on / 24 off) so each press is a fresh
  EDGE the game's `cur & ~prev` logic (`FUN_800788ac`: `DAT_800e7e68 = ecf54 & ~ecf56`) sees.
- **THE big fix — OTC DMA (DMA channel 6) was unimplemented → malformed/cyclic OT → render hang.**
  `ClearOTagR` (FUN_80081458) builds the reverse-linked empty ordering table NOT in CPU code but by
  firing the **OTC DMA**: its libgpu-vtable worker `FUN_80082424` sets D6_MADR=&ot[n-1], D6_BCR=n,
  then writes `0x11000002` to D6_CHCR (0x1F8010E8). mem.c had no channel-6 handler, so the OT was
  never linked — entries kept stale values and the head pointed into the prim bump-buffer at
  0x800bfe68, where 4 menu prims formed a 4-node CYCLE (9C→88→7C→68→9C…). DrawOTag then walked the
  65536-node cap EVERY frame; harmless at the near-empty title but at the menu it drew huge prims
  65536× = effective hang (gdb caught it spinning in put_px_b under gpu_dma2_linked_list). FIX:
  implement OTC in mem.c io_write (0x1F8010E0/E4/E8): write n words descending from MADR, each →
  next-lower entry, lowest → 0x00FFFFFF terminator; clear CHCR busy so the lib's poll passes. After
  this the OT is a clean ~0x800-entry list ending at the 0xa5a60 sentinel — no cycle, no cap.
- **RESULT (verified, headless, PSXPORT_FORCE_BUTTONS=FFF7 = pulsed Start):** START→DEMO(title s2)
  → Start edge → s3 (menu) → Start → s5 (`FUN_80052078(2)` = load GAME) → **stage=GAME(0x8010637C)
  at frame 66**, GAME's own state machine ticking (demo_state 5→2). No OT warning, no hang,
  baseline (no input) still parks cleanly at the title. The interactive loop now runs until the
  window closes (PSXPORT_NATIVE_FRAMES caps headless; default 120 headless, unbounded windowed).
- **NEXT — GAME renders BLACK (residual).** GAME IS running (its sequencer ticks in the flat interp
  at pc≈0x80108a64, GAME.BIN) and IS drawing: per-frame GPU log shows a big VRAM upload (~15.4k
  gp0words = level textures) then ~29 prims/frame; `PSXPORT_ENVDBG` confirms the draw env is correct
  (clip (0,0)-(319,239) off (0,0) / clip (0,256)-(319,495) off (0,256), alternating double-buffer).
  BUT `PSXPORT_VRAMSCAN` shows all non-black VRAM is at x≥320 (the texture pages); the framebuffer
  region x=0..319 (both buffers) is ENTIRELY black. So the 29 prims don't rasterize into the
  framebuffer — GAME is most likely still at an early loading/fade state (or an asset it needs
  didn't load: the strNext attract stream times out, and there's an early "file not found" + UNIMPL
  B0:0x18). NB the flat interp makes GAME slow (~native-frame 130 in ~100s), so 120 native frames
  doesn't get far past the load. Tools added this session for the chase: `PSXPORT_VRAMSCAN` (whole-
  VRAM non-black bbox per present), `PSXPORT_ENVDBG` (GP0 E3/E4/E5 draw-clip/offset), `PSXPORT_OTDBG`
  (cyclic-OT chain dump). Next: drive GAME further (more native frames / recompile GAME.BIN fns for
  speed), find what it waits on to leave the load (CD/strNext/asset), and confirm whether prims are
  clipped-out vs sampling-black.

## 2026-06-15 (later 35) — AUTHENTIC BOOT WORKS: recompiled stub draws SCEA → LoadExec → MAIN title
Replaced the FAKE native_fmv intro with the AUTHENTIC boot: the disc's boot executable SCUS_944.54
is now **recompiled** and run as the real PSX entry, drawing toward SCEA, then handing to native
MAIN boot (later 33). One faithful path — no boot-mode env toggles (user directive).
- **Recompiler emits the stub as a SEPARATE module** (`emit.py --stub`, STUB_NAMES). The stub
  overlaps MAIN.EXE's address space (both load @0x80010000; stub text 0x10000–0x38800, entry
  0x80018B6C), so a shared `func_<addr>` namespace would collide. emit.py was refactored into
  `emit_module(exe, out_dir, Names, …)`: MAIN keeps `func/rec_dispatch/rec_set_override/g_override`;
  the stub gets `stub_func/stub_dispatch/stub_set_override/g_stub_override` + its own
  `stub_shard_*.c`/`stub_disp.c`/`stub_decls.h`. Both share `rec_dispatch_miss` (BIOS/interp) on a
  dispatch miss. 214 stub fns recompiled from the entry's jal graph; the rest run via the interp on
  the stub's RAM bytes (hybrid, same as MAIN). run.sh extracts SCUS_944.54 + passes `--stub`.
- **boot.c**: removed the `native_fmv_play("LOGO.STR"/"OP.STR")` fake intro. Single path →
  `native_stub_run(&c, MAIN.EXE path)` (runtime/recomp/native_stub.c): loads the stub over MAIN's
  low text, `stub_dispatch(c, 0x80018B6C)`; intercepts the stub's **LoadExec (BIOS A0:0x51)** via
  `g_loadexec_hook` → reloads MAIN.EXE (restoring the text the stub overwrote, as the real boot
  does) + longjmps out → `native_boot_run` takes over. (The stub loads MAIN by name through BIOS
  LoadExec — wrapper @0x80011340 calls the A0(0x51) trampoline with `cdrom:\MAIN.EXE;1`.)
- **Stub libcd/libetc overrides** (mirror cd_override.c/timing.c for the stub's own PSY-Q copies):
  CD_cw **0x8001A0C0** (NOTE: entry is 2 insns BEFORE the `addiu sp` prologue @0x8001A0C8 — the
  trace `… -> 8001A0C0` is ground truth, overriding 0x8001A0C8 silently no-ops) → success; CdSync
  0x80019B78 → 2; CdDataSync 0x8001A944 → done; VSync vblank-wait 0x80017FC4 → advance the native
  frame clock (counter DAT_800267B4) + deliver VBlank events + `gpu_present()`. With these, CD init
  passes cleanly (no more "CD timeout"/"Init failed") and ResetGraph runs.
- **Override plumbing for the stub**: `rec_set_override` is keyed by recompiled-function INDEX, so
  it can't target non-recompiled stub fns. Added (a) `stub_set_override` (the stub module's
  index-keyed table, for recompiled stub fns) and (b) `rec_set_interp_override` (a raw-address table
  in interp.c, consulted by `call_addr` AND by `rec_dispatch_miss` before it enters the interpreter)
  for interpreter-run stub fns. `native_stub.c::stub_override()` registers in both. Watchdog now also
  reports the interp PC and catches SIGSEGV/SIGABRT/SIGBUS with a backtrace.
- **SCEA NOW RENDERS → LoadExec → native MAIN boot (full authentic chain works).** The SCEA state
  machine (crt0 → 0x800111B4 → 0x80011A78, jump table @0x80010054, 20 states) drives the **CD
  controller at the REGISTER level** (0x800123B0 pokes 0x1F801800–0x1F801803 via pointers baked in
  stub .data @0x80025434/38/3C/40; polls the IRQ-flag reg low 3 bits = CD response 1=DataReady
  2=Complete 3=Ack 5=DiskError). Implemented a **native CD controller** (runtime/recomp/cdc_native.c,
  wired into mem.c io_read/io_write for 0x1F801800–3): index banking, param/response/data FIFOs, an
  interrupt queue (INT3-ack-then-INT2-complete), and the command set SCEA issues — GetTN(0x13),
  Init(0x0A), GetTD(0x14), ReadTOC(0x1E), **GetID(0x1A) → returns "SCEA" (licensed/America)**,
  Setloc(0x02), Setmode(0x0E); data reads served from disc.c. Commands complete SYNCHRONOUSLY (ready
  on the next poll) — correct for code that busy-polls without advancing time. The SCEA image is an
  embedded 4-bit-CLUT TIM @0x8001FB5C (no data load needed).
- **VSync: override the PUBLIC VSync(mode) 0x80017E4C** (not the low-level wait) — the stub's timed
  holds BUSY-POLL VSync(-1) expecting a preemptive VBlank IRQ to tick the count; we deliver none, so
  in our cooperative model EVERY VSync call (incl. query) ticks one frame + delivers VBlank events +
  `gpu_present()`. That makes the holds/fades progress and the screen visible. (mirrors timing.c.)
- **VERIFIED on-screen:** SCEA "Sony Computer Entertainment America" renders
  (scratch/screenshots/scea_logo.png), holds, fades; the stub then issues `LoadExec(cdrom:\MAIN.EXE;1)`
  which we intercept → reload MAIN + native MAIN boot → **TOMBA!2 title renders**
  (scratch/screenshots/post_handoff.png). Headless: `PSXPORT_NOWINDOW=1 PSXPORT_GPU_DUMP=dir
  PSXPORT_WATCHDOG=20 ./run.sh`. Remaining = the later-33 native-MAIN residuals (malformed OT, DEMO
  needs pad input, OP movie strNext streaming) — unchanged by this work.

## 2026-06-14 (later 34) — REAL BOOT PATH laid out (oracle call-trace): SCEA = the SCUS boot stub
**User correction:** SCEA is NOT an FMV and the port's `native_fmv_play` intro is a FAKE boot.
Used the oracle (Beetle wide60rt, real BIOS) with a new **streaming call trace**
(`PSXPORT_CALLTRACE="lo-hi[:path]"`, runtime/psxport_hooks.cpp + Beetle cpu.c; logs jal/jalr
targets in range with `# frame N` markers) to get the REAL boot call path. See memory
[[psxport-scea-boot-stub]] + [[psxport-use-oracle-trace]].
- **Finding (definitive):** across all 400 boot frames, 100% of calls execute in `0x80018xxx` =
  **SCUS_944.54 (the boot stub)** code; game-main `FUN_80050b08` is NEVER reached in that window.
  0x80018B6C decodes as valid code in SCUS, garbage in MAIN.EXE → it's the stub running. SCUS holds
  the PSX license string ("Sony Computer Entertainment Inc. for North America area") + `MAIN.EXE;1`.
- **Real boot path:** BIOS → **SCUS_944.54 stub draws the SCEA "…America Presents" screen** (high-
  res 700×480, TIM/font — not ASCII, not FMV, not BIOS) + loads MAIN.EXE → MAIN crt0 (0x800896E0)
  → game-main FUN_80050b08 → START → DEMO (Whoopee logo + OP movie FUN_80106f80) → menu.
- **Why the port has no SCEA:** `runtime/recomp/boot.c` loads MAIN.EXE DIRECTLY and enters game-
  main — it starts at the MAIN.EXE step, skipping the entire stub (SCEA). The native_fmv intro was
  a fake stand-in.
- **NEXT — replicate authentically:** run **SCUS_944.54 as the real entry** (like the BIOS does):
  load it to 0x80010000, run from its header entry (interpret via rec_interp/rec_coro_run — it's
  not recompiled; or add it as a 2nd recomp input). It draws SCEA itself, then loads MAIN.EXE and
  jumps to 0x800896E0 → the existing native MAIN boot (later 33) takes over for Whoopee→OP→menu.
  Blocker to expect: the stub's MAIN.EXE LOADER uses BIOS/its-own file I/O at stub addresses (NOT
  the MAIN.EXE cd_override addresses) — wire native CD/BIOS-file-read for the stub's loader. Trace
  the stub's load path with PSXPORT_CALLTRACE to find its file-I/O calls. REMOVE the fake
  native_fmv_play intro from boot.c.

## 2026-06-14 (later 33) — TITLE SCREEN RENDERS natively (full hybrid boot: init→sched→DEMO→draw)
The PC-PSX hybrid boot now reaches and RENDERS the Tomba!2 title screen
(scratch/screenshots/nb5_f20.png) with NO PSX scheduler/threads/ucontext — verified on-screen.
Chain: crt0 → native init prefix → native cooperative scheduler runs START (loads assets via
the task1/task2 loader handshake) → FUN_80052078(1) → DEMO stage → DEMO draws the title.
- **Native cooperative scheduler (no ucontext)** — `runtime/recomp/native_boot.c` +
  `rec_coro_run` (interp.c). Each task is a resumable coroutine: a yield captures the PSX
  register context and longjmps out; resume restores it and continues at the captured PC. The
  PSX stack lives per-task in g_ram (obj+8), so no native stack/ucontext is needed. `rec_coro_run`
  is a FLAT interpreter (PSX calls chain via the PSX stack, not the C stack), so it can resume
  mid-call-chain; native overrides + BIOS are still invoked as C. **ChangeThread (FUN_80080880,
  ov_switch) is THE switch primitive**: yield (FUN_80051f80,state1) / task-end (FUN_80051fb4,
  state0) / stage-transition (FUN_80052078,state3) all set state then ChangeThread → capture+
  longjmp. native_scheduler_step walks the 3 task slots like FUN_80051e60; FUN_800506d0 re-arms
  a yielded task 1→2 each frame. Per frame it delivers VBlank + sound-DMA(0xF0000009) events the
  game's TestEvent waits poll.
- **Recompiler now seeds from the overlays** (emit.py --overlays): functions reached only from
  the stage overlays (FUN_80044bd4 the task registrar, etc.) were Ghidra/jal-invisible → ran in
  the interpreter, un-overridable. emit.py scans START/DEMO/GAME.BIN for jal targets into resident
  text (109 fns; 1118→1220 recompiled). discdump `get` does nested paths (BIN/X.BIN).
- **Rendering wiring** — the native loop now does the draw/display-env handling it had omitted:
  per frame ClearOTagR the back buffer's OT + set PTR_DAT_800ed8c8 (env pair @ 0x800e80a8 +
  DAT_1f800135*0x2070 — Ghidra `+uVar1*0x81c` is WORD arithmetic = 0x2070 bytes; wrong stride
  was corrupting the odd buffer), and on DAT_1f80019c==0 submit PutDispEnv/PutDrawEnv/DrawOTag +
  flip. GPU hardening: sprite/rect blit clips its dx/dy to the drawing area up front (an
  off-screen sprite was burning w*h sample_tex calls and wedging the frame); OT traversal capped
  at 0x10000 nodes with a malformed/cyclic warning.
- **TOOL: frame-progress watchdog** (`runtime/recomp/watchdog.c`, PSXPORT_WATCHDOG=<sec>) — SIGALRM
  fires if no frame presents within N sec, dumps the stuck backtrace (backtrace_symbols_fd, link
  -rdynamic, build -g) and _exit. Found both wedges above precisely. Use it for any boot hang.
- **RESIDUALS (next):** (1) a MALFORMED/CYCLIC ordering table — DrawOTag from the OT head
  (madr=0x800ea0a4) chains a next-ptr into 0x800bfe68 and hits the 0x10000 cap EVERY frame →
  slow. Root-cause the OT the game builds vs the OT I ClearOTagR/DrawOTag (PTR_DAT_800ed8c8) —
  likely DEMO draws into a different OT, or AddPrim/primitive-buffer setup I'm missing. (2)
  non-fatal "time out in strNext()" CD-stream warning (attract-demo stream; CdReadyCallback not
  driven). (3) DEMO sits at title state (obj+0x48=2) — advancing to the menu/game needs pad input
  (obj+0x6b; ==7 → GAME) and/or the strNext stream. Run headless: PSXPORT_NATIVE_BOOT=1
  PSXPORT_SKIP_INTRO=1 PSXPORT_GPU_DUMP=dir PSXPORT_WATCHDOG=8 PSXPORT_NATIVE_FRAMES=N.

## 2026-06-14 (later 32) — NATIVE HYBRID DRIVER stood up: init prefix + 1st stage transition
Built `runtime/recomp/native_boot.c` (PSXPORT_NATIVE_BOOT=1, wired in boot.c). The PC engine
now drives game-main natively instead of running the infinite PSX scheduler:
- **Approach:** override game-main `FUN_80050b08` with `ov_game_main`. crt0 `func_800896E0`
  runs its BSS-zero/SP/gp/heap setup and calls main, which lands in our override. The override
  runs the ~25 init calls (transcribed 1:1 from FUN_80050b08:31275-31299) via `rec_dispatch`,
  NOT the scheduler loop. Helpers rc0/rc1/rc2/rc3 set a0..a2 then dispatch.
- **VERIFIED (RAM probes):** (1) init prefix runs clean — ResetGraph fires, no break during it
  (the `[break] code 1` after is crt0 post-main, expected). Scheduler state correct: task0
  state=2 runnable, entry=0x800499E8, name=A0F.BIN stack; tasks 1/2 free. (2) Running task 0's
  initial entry FUN_800499e8 natively (after `mem_w32(0x1f800138,0x801fe000)` to point the
  scheduler "current task" at task0) resolves \BIN\START.BIN and FUN_80052078(0) loads the
  overlay raw to 0x80106228 via the native CD override: count@0x80106228=6,
  entry-word@0x8010649c=0x27BDFE38 (exact), task0 state=3 entry=0x8010649C.
- **KEY mechanic:** with BIOS threads stubbed to no-ops, FUN_80051f80 (yield) / ChangeThread
  just return — so a NON-looping task fn (FUN_800499e8) called via rec_dispatch runs straight to
  completion. But the stage SEQUENCERS (FUN_801064f0 / DEMO / GAME entries) are infinite
  do/while(true) loops whose only exit is the scheduler's state==3 RESTART — with no-op
  ChangeThread they'd spin forever. So they MUST be reimplemented natively as per-frame state
  machines (one obj+0x48 iteration per frame == one original yield), called from a native frame
  loop that replaces the scheduler call FUN_80051e60 in LAB_80050c6c.
- **Game-main loop body (LAB_80050c6c), to reimplement natively:** per frame — DAT_800e809c=0;
  framebuffer ptr swap (DAT_800bf544/4f4 from DAT_1f800135); FUN_800788ac (tick, overridden by
  ov_frame_update); **FUN_80051e60 (scheduler -> replace with native stage step)**; FUN_80080f6c(0)
  (draw sync); busy-wait DAT_800e809c<DAT_1f800235 (vblank); FUN_800506d0 (present); buffer swap
  on DAT_1f80019c (0=swap+continue, 2=swap, 3=stay).
- **NEXT:** native frame loop + native step of stage 0 (START, FUN_801064f0): its loop loads
  asset manifests (OPN/A0*/TOMBA2.* into DAT_800be118/1e0/0f0), registers loader tasks
  FUN_80044f58/FUN_8004514c (run them directly — they complete synchronously w/ native CD), then
  transitions to DEMO. Then native DEMO step (the s0-s7 SM, leaf fns listed in "later 31") to
  reach the title/menu on-screen; then GAME. Drive native FMVs at the right point (START plays
  OPN, the menu->game uses the existing native_fmv path).

## 2026-06-14 (later 31) — STAGE MAP NAILED: START/DEMO/GAME overlays; corrects "later 30"
The 3 stage entries in `PTR_LAB_800a3ecc[0..2]` = `{0x8010649c, 0x801062e4, 0x8010637c}` are
entry points into THREE SEPARATE overlay files, all loaded RAW to the **same base 0x80106228**
(so only one is resident at a time — that's why stages 1/2 are NOT disassemblable from
`ram_f1000.bin`; at that snapshot 0x801062e4/0x8010637c hold START.BIN's *string/data* bytes,
not code). `FUN_80052078(stage)` = `FUN_800450bc` reads `(&DAT_800be1e0)[stage*2]` (lba,size)
to `0x80106228` then restarts task 0 at `PTR_LAB_800a3ecc[stage]`. The 3 files come from the
START.BIN manifest `\BIN\{START,DEMO,GAME}.BIN`:
- **Stage 0 = `\BIN\START.BIN`** (LBA 1904, 1648 B) → entry `0x8010649c` = intro/boot
  sequencer (`FUN_801064f0` is its tail). Loads asset manifests (OPN/CRD/SOP/A0*.BIN,
  TOMBA2.{IDX,IMG,DAT,SND}, SWDATA.BIN, VOICE/DEMO/BGM.XA), registers loader tasks, then
  state 3 → `FUN_80052078(1)` → DEMO.
- **Stage 1 = `\BIN\DEMO.BIN`** (LBA 1879, 5372 B) → entry `0x801062e4` = **title/menu +
  attract** sequencer. 8-state jump table @`0x8010622c`. State 4 (`0x80106580`) reads the menu
  result `obj+0x6b`: ==7 → set `DAT_1f800134=1`, go state 5 (`0x801065dc`) → `FUN_80052078(2)`
  = **start GAME**; ==1/2 → state 2.
- **Stage 2 = `\BIN\GAME.BIN`** (LBA 1882, 11636 B) → entry `0x8010637c` = **gameplay**
  sequencer. Sub-state from `DAT_1f800134`; per-frame loop (incr `obj+0x198`, `FUN_80051f80`
  yield) dispatching state handlers `FUN_801086e0/720/784` (all inside GAME.BIN). GAME→...
  `FUN_80052078` at `0x80108a48` (back to a stage).
- **CORRECTIONS to "later 30":** (a) the 0x80106xxx resident overlay is **START.BIN, not
  OPN.BIN** (OPN.BIN is just one asset START.BIN loads; verified: extracted START.BIN is
  byte-identical to ram_f1000@0x80106228, 0 diffs, entry word `27bdfe38`). (b) START.BIN is
  BOTH the manifest AND code (one overlay). (c) "disasm stages 1/2 from ram_f1000" is a dead
  end — extract the files instead.
- **Tooling:** `tools/disasm_overlay.py <bin> [start] [end] [--base=]` disassembles an overlay
  at base 0x80106228 (resyncs past data/jump-tables). Extract overlays with
  `scratch/bin/fmv_compare dumplba <lba> <size> <out>`. Reference dumps in
  `scratch/decomp/{START,DEMO,GAME}.bin.asm`; overlay bins in `scratch/bin/overlays/`.
- **DEMO state machine** (obj+0x48 = state; jump table @0x8010622c; common tail incr obj+0x198
  then `FUN_80051f80` yield then loop). Decoded from scratch/decomp/DEMO.bin.asm:
  - **s0** `0x801063c0`: `FUN_80045080(0x80108f9c,2)`; `FUN_80044bd4(FUN_80044f58,2,0,0)` (run
    loader); `FUN_8007982c`+`FUN_80075240`+`FUN_8001cf00(1)` (gfx/audio setup); -> s1.
  - **s1** `0x8010641c`: `FUN_80106f80(0)` (load/fade poll) -> if done s2; watches DAT_800e7e68.
  - **s2** `0x80106464`: `FUN_8010696c()` = title input/decision -> 1: s7; 2: branch on obj+0x68
    -> s3 or s4 (clears obj+0x50/0x6b, the menu result).
  - **s3** `0x801064e8`: `FUN_80106ac4()` -> 1: s7; 2: obj+0x68==1 -> s5 (DAT_1f800134=0) else s6
    (`FUN_800750d8(1)`); 3: back to s2.
  - **s4** `0x80106580`: `FUN_8007bf20(0,0,0)`; reads **menu result obj+0x6b**: ==1 -> s2/obj+0x68=1;
    ==2 -> s2/obj+0x68=0; **==7 -> s5 + DAT_1f800134=1** (start GAME).
  - **s5** `0x801065dc`: `FUN_80052078(2)` -> load GAME.BIN, restart task 0 at stage 2.
  - **s6** `0x801065ec`: `FUN_8007b45c`; if obj+0x50==3 `FUN_80106824(1)`+`FUN_80106690(1)`;
    `FUN_8001cf2c`/`FUN_80075a80`. **s7** `0x80106668` (tail, undisassembled here).
  Leaf fns the native DEMO driver must call: FUN_8010696c, 80106ac4, 8007bf20, 80106824,
  80106690, 80106f80, 80045080, 80044bd4(FUN_80044f58), 8007982c, 80075240, 8001cf00/8001cf2c,
  80075a80, 800750d8.
- **NEXT:** decode DEMO s7 + GAME's handlers (FUN_801086e0/720/784); then build the native PC
  driver (hybrid): run `FUN_80050b08` init calls, then a native frame loop = native re-impl of
  each stage's per-frame state machine calling the recomp leaf draw/update fns (CD/wait already
  native), no scheduler. Exploit "leaf tasks complete synchronously" — call them directly.

## 2026-06-14 (later 30) — DIRECTION: PC-PSX HYBRID (PC drives recomp logic); boot RE started
**User architecture decision** (see memory `psxport-hybrid-architecture`): psxport is a PC-PSX
HYBRID. PC-native engine owns boot/graphics/FMV/audio/input and is the DRIVING FORCE — it owns
the frame loop and **calls the game's RE'd per-frame entry points directly**, never blocked by
PSX threading/waits/IRQ ping-pong (all stripped). Game LOGIC (AI/quests/player/menu) stays
recomp. Execution model = "PC loop calls RE'd per-frame entry points": reimplement the control
flow (sequencing / per-frame state machine) natively, call the game's leaf logic functions
(draw, per-object update) which are normal recompiled functions that return. NOT running the
infinite-loop yielding tasks as-is (needs ucontext, ruled out). SCEA is game-drawn (not BIOS).
Target flow: PC boot -> SCEA -> WhoopeeCamp FMV -> OP FMV -> main menu. FMVs done.
- **Boot RE so far:** game main = `FUN_80050b08` (init calls: FUN_80089788/80085b20/800898a0/
  80080bf0(3)/.../80085900(3)/8001cc00/800520e0/80085900(1)/**80051e00**(sched init)/
  **80051f14(0,FUN_800499e8)**(register first task)/80085bb0; then the scheduler loop at
  LAB_80050c6c: per-frame FUN_800788ac(tick)+FUN_80051e60(scheduler)+vblank-wait+FUN_800506d0,
  branching on DAT_1f80019c). The PC engine should run the INIT calls then replace the loop.
- **First task** `FUN_800499e8` resolves `\BIN\START.BIN`, then `FUN_80052078(0)`.
- **START.BIN is a FILE MANIFEST**, not code (extracted via new harness `dumplba`): count=6 +
  paths `\CD\SWDATA.BIN \CD\TOMBA2.{SND,DAT,IMG,IDX}` then many `\BIN\A0*.BIN` overlay files.
- **Stage-entry table** `PTR_LAB_800a3ecc[0..2]` (MAIN.EXE data @0x800a3ecc) = `{0x8010649c,
  0x801062e4, 0x8010637c}` (the 0x80106xxx OVERLAY region = intro sequencer FUN_801064f0 etc.,
  loaded from disc, interpreted — partially decompiled in scratch/decomp/overlay.c).
  `PTR_FUN_800a3ed8` = `0x800499e8` (resident).
- **Stage entries are NOT in ram_f1000_all.c decomp** — disassemble from the RAM snapshot
  `scratch/bin/tomba2/ram_f1000.bin` with `python3 tools/disasm.py <dump> <a> <b>` (capstone).
  **Stage 0 = 0x8010649c = the PROLOGUE of the intro sequencer**; overlay.c's FUN_801064f0
  (0x801064f0) is just its TAIL (the do/while loop — Ghidra split the function). Stage 0
  sets up a task obj (stack@sp+0x190), calls FUN_80081218 + FUN_80080f6c, then resolves
  OPN.BIN/START.BIN/IDX/XA files and runs the state machine that chains stages via
  FUN_80052078(param) -> restart task at PTR_LAB_800a3ecc[param]. Stages 1,2 = 0x801062e4,
  0x8010637c (disasm next). The 0x80106xxx overlay (OPN.BIN) is resident in ram_f1000.bin.
- **NEXT (step by step):** (1) disasm stages 1/2 (0x801062e4, 0x8010637c) + find where SCEA
  is drawn and the SCEA->WhoopeeCamp->OP->menu transitions live; (2) build the PC-driven boot:
  native engine runs FUN_80050b08's init calls, then a native frame loop drives each stage
  (call leaf draw/update via rec_dispatch/interp, native FMV for the two movies), no scheduler.
  Harness `dumplba <lba> <nbytes> <out>` extracts any sector range.

## 2026-06-14 (later 29) — FMV FULLY WORKING (video+audio+speed); next: boot -> main menu
FMV is done — video, audio, and speed all correct (verified on-screen by user: WhoopeeCamp
logo + Tomba render cleanly, with sound, correct rate, clean to the corners).
- **Column-major macroblocks** (`911bf4a`'s predecessor `9ddb98f`): the game emits MDEC
  macroblocks COLUMN-major (emit index k -> row=k%mby, col=k/mby); we placed row-major ->
  sheared/spread frames. Found via the `framemap` ASCII oracle (synthetic row-major tiling
  tests all passed; only a real frame showed the shear, stride=mby). Don't re-chase.
- **XA-ADPCM audio** (`911bf4a`): STR interleaves CD-XA audio sectors (submode 0x04, Form2);
  player dropped them. `xa_decode_sector` transcribed from mednafen cdc.c (oracle), dedicated
  SDL device at 37800Hz. `disc_read_raw` exposes the raw 2352B sector (the XA subheader).
  Sound oracle `tools/fmv_compare xacmp` = bit-exact vs independent reference (0/250k diff).
- **Speed**: paced to the AUDIO/media clock (was a fixed-15fps guess = too slow).
- **Bottom-right black bits**: final <0x20-word MDEC remainder fell to a linear data-port
  read (no voffs scatter); `mdec_dma_out_rest` drains it scatter-aware (MDEC_DMARead isn't
  gated by the 0x20 burst, mdec.c:899). Drain now 100% scattered.
- FMV harness modes: `<lba> <size> <frame#>` (VLC diff), `idcttest` (table self-test +
  placement tests), `framemap` (ASCII luma map), `strscan` (CD-XA framing), `xacmp` (sound).

**NEXT — user's target flow:** PC boot -> SCEA -> FMV#1 (WhoopeeCamp/LOGO.STR) -> FMV#2
(Tomba/OP.STR, Start to skip) -> **main menu**. FMVs done; SCEA + the main menu are the gap.
The main menu is interactive GAME code built on the cooperative task scheduler (FUN_80051e60 +
OpenThread/ChangeThread, which we stubbed to no-ops). Reaching it needs that scheduler working
WITHOUT ucontext/threading (per user) — i.e. run cooperative tasks via the hybrid interpreter,
which holds explicit PSX CPU state (PC/regs/SP-in-g_ram) so a yield = save/restore a state
struct (no native stack, no ucontext). Investigate interp.c + scheduler next.

## 2026-06-14 (later 28) — FMV compare harness vs oracle: VLC proven correct; quant-order fix
Built `tools/fmv_compare` (commit `2d41a68`) per user request. Our FMV pipeline already runs
the decoded run/level stream through mednafen's MDEC (the oracle) for IDCT/YCbCr, so the only
non-oracle code is the STR-demux + BS/MPEG-1 VLC (`bs_decode_frame`). The harness reads a real
STR frame off the disc and diffs our VLC output against an INDEPENDENT reference decoder
(fresh Table B-14 transcription as bit strings, independent bit reader/escape/sign), word-for-
word, first divergence → (block,coeff). An `idcttest` mode links the REAL mednafen MDEC and
decodes synthetic blocks to verify the quant+IDCT table uploads.
- **VLC is CORRECT (verified):** LOGO frames 5/40 + OP frame 70 all MATCH the reference
  (3600/7001/12731 codes identical). The earlier "identical ramp across blocks" was real
  content, not a bug. Earlier fear of a DC-prediction/VLC bug = WRONG, ruled out.
- **Tables reconstruct correctly (idcttest):** DC-only block → flat; single AC coeff → clean
  cosine ramp. (Orientation looks "transposed" but that is mednafen's internal Coeff/ZigZag
  convention, self-consistent with how the game feeds it — NOT our bug. Don't re-chase it.)
- **FIX (quant order):** the quant matrix was uploaded RASTER, but mednafen stores it linearly
  (mdec.c:844) and dequantizes via `QMatrix[CoeffIndex]` (scan order, mdec.c:703) → it must be
  ZIGZAG/scan order: `qz[scan]=quant_raster[ZigZag[scan]]`. DC (idx0) unaffected (ZigZag[0]=0);
  AC got the wrong per-frequency weight. Now reordered in `mdec_upload_tables`.
- **Harness gotcha (fixed):** for the synthetic test, sample a macroblock well past FIFO
  warm-up and index `px[(mb*16)*WIDTH + ...]` (row stride!) — an early off-by-stride read hit a
  no-AC block and falsely showed "AC dropped".
- **STILL RESIDUAL:** real frames remain visibly blocky (recognizable but heavy). VLC + core
  tables are verified-correct, so the residual is DOWNSTREAM: suspect the MDEC FIFO feed/drain
  interleave (`mdec_decode_to_rgb555` burst loop) and/or the chroma (Cb/Cr 8x8) path. The
  `idcttest` showed an odd/even-row wrinkle on the single-AC block worth chasing there. Use the
  harness (extend `idcttest`) to nail it. `bs_decode_frame`/`mdec_decode_to_rgb555` are now
  non-static for harness use.

## 2026-06-14 (later 27) — DIRECTION: NO threading / NO ucontext; PC-native intro boot
**User (emphatic): "no threading", "you can't use ucontext", "boot the game PC native way."**
The intro wedge was diagnosed, then the whole emulated-thread approach was dropped.
- **Root cause of the intro wedge (the ucontext path):** the game runs a cooperative task
  scheduler (`FUN_80051e60`) over task objs at `0x801fe000` (stride 0x38; state word at
  obj+0: 0=free 1=sleeping 2=runnable 3=restart-at-new-entry 4=running). The intro sequencer
  (`FUN_801064f0`, idx0) is an infinite SM on its OWN obj+0x48; at inner-state 1 it calls
  `FUN_80044bd4(FUN_8004514c,1,1,0)` which (param_4==0) **busy-waits `while(DAT_1f80019b==0)
  FUN_80051f80(1)` for the loader to signal done**. The loader (`FUN_8004514c`, idx2/801fe070)
  yields at state=1 inside a sub-call and **never reaches `DAT_1f80019b=1; FUN_80051fb4()`**,
  so the flag stays 0 → registrar spins forever → sequencer frozen at inner-state 2 → intro
  never advances. Confirmed via the obj+0x48/state trace in threads.c (PSXPORT_THR_TRACE).
  The native CD/file loads all SUCCEED (12 reads incl START.BIN@1904 + 326KB@LBA9684); the
  wedge is the cooperative-task completion handshake, not I/O.
- **Decision:** the game's runtime IS a coroutine task system (infinite-loop tasks that yield/
  resume mid-function each frame) — running that recompiled code faithfully needs stack
  save/restore (ucontext/fibers). Per user we are NOT doing that. So we **do not run the
  game's intro scheduler at all** — drive the intro natively.
- **Done (commit `b60c3d4`):**
  - `threads.c`: ucontext coroutine layer **removed**; OpenThread/CloseThread/ChangeThread are
    now no-op stubs (scheduler no longer run).
  - `boot.c`: native intro — `native_fmv_play("MOVIE/LOGO.STR")` (SCEA + Woopee Camp) then
    `("MOVIE/OP.STR")` (Tomba!2 opening). `func_800896E0` (scheduler entry) NOT entered.
    `PSXPORT_SKIP_INTRO=1` bypasses.
  - `native_fmv.c`: per-frame Start-skip (rising-edge) + pacing (`PSXPORT_FMV_FPS`, default 15).
  - `run.sh`: **native_fmv.c was never in the build** (FMV player was dead code, 0 callers) —
    added to SRC; added `-lm` (IDCT cos/lround).
  - Disc map: `MOVIE/LOGO.STR` LBA 11491 (2.6MB), `MOVIE/OP.STR` LBA 152238 (20.7MB),
    `MOVIE/END.STR` LBA 162374.
- **Verified:** builds, boots, both FMVs demux→VLC→MDEC→present end-to-end (300 MBs/frame,
  320x240), zero threads/ucontext. Frames in `scratch/screenshots/intro2/`.
- **KNOWN BUG (next):** `bs_decode_frame` DC-coefficient prediction is wrong — early frames
  wash out (pale), later frames show the real scene with vertical banding (OP visibly a dark
  red scene; LOGO mostly white). Pre-existing decoder bug (from the e75f1fd FMV commit), only
  now exercised. Decode-quality pass is the next task.
- **OPEN (future milestone):** post-intro hand-off (title/gameplay) is the game's task system;
  running it without threading/ucontext needs a resumable-execution design (undecided).

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

## 2026-06-14 (later 12) — S2 started: recompiled core RUNS from boot; HLE surface mapped
`runtime/recomp/boot.c` loads MAIN.EXE into g_ram, enters `func_800896E0`. Emitter now
discovers direct-`jal` targets (fixpoint, stops at first UNKNOWN so data doesn't inject
seeds) → caught a Ghidra-missed fn `0x80089860` (1597→1598). Dispatch misses route to
runtime `rec_dispatch_miss`. **The core executes real boot code.** Measured boot needs:
- BIOS (in order): `A0:0x39` InitHeap, `B0:0x19`, `B0:0x5B`, `C0:0x0A` ChangeClearRCnt,
  `A0:0x72`, `B0:0x35`. Then indirect fn `0x8009A8E8` (via `jalr` — direct-jal discovery
  can't see it; needs a fn-ptr/indirect seed or manual add).
- HW regs: I_MASK/I_STAT, DMA DPCR, Timer1, CDROM, and a **GPUSTAT `0x1F801814` ready-poll**
  that spins (mem.c returns 0). Minimal GPU/timer status needed to advance.
- NEXT: A0/B0/C0 HLE table for those ~6 calls + seed `0x8009A8E8` + minimal GPU/timer
  status; stand up S4 diff harness vs Beetle to verify bit-exact. Build: `tools/recomp/
  build.sh` (leaf tests); boot recon: compile boot.c instead of test_leaf.c, run under
  `timeout`.

## 2026-06-14 (later 13) — S2: recompiled core boots through BIOS into CD/event subsystems
`runtime/recomp/hle.c` = recomp-native HLE BIOS (transcribed faithfully from the proven
`hle_kernel.cpp`): heap A0:0x33-0x39, HookEntryInt, FileWrite→stderr, GetB0/C0Table,
ChangeClearPAD, GPU_cw, C0 installers, and `syscall` Enter/ExitCriticalSection via `$a0`.
`mem.c` reports GPUSTAT (`0x1F801814`) permanently ready (+toggling bit31) to clear the
boot ready-poll. Emitter EXTRA_SEEDS for jalr-reached fns `0x8009A8E8/ADC4/AA4C`.
- **Verified**: boot runs deep real game code — heap init → HookEntryInt → CD init (emits
  `CD_init`/`CD_cw`/`CD timeout` via FileWrite) → past GPU handshake → OpenEvent/EnableEvent/
  WaitEvent loop + CD-command retry loop. Reproduce: `tools/recomp/build.sh` (leaf tests +
  boot). Leaf tests still pass.
- **S5 boundary (honest stop):** the CD-retry + WaitEvent loops block on CD-complete / VBlank
  **IRQs that nothing generates yet**. Faking "event fired"/CD-done = bandaid (refused).
- NEXT (big phase): peripheral + IRQ/event delivery (lift CD/VBlank/GPU/SPU from Beetle; wire
  IRQ → invoke s_int_handler like wide60 hle_irq.cpp; implement events). Plus S4 diff harness
  vs Beetle, and an auto indirect-pointer (lui+addiu) seed scan to end CD-helper whack-a-mole.

## 2026-06-14 (later 14) — DIRECTION: no CD/HW emulation; native overrides infra DONE
User refined: "no CD code, no emulation, pure PC native." → don't emulate CD/IRQ; **override
the game's CD/streaming fns with native file I/O, synchronous completion**. This is the
recomp-overrides path. Override points already RE'd this session: `FUN_8008c1ec` (read
blocks@LBA), `FUN_8008bf50`/`FUN_8008b8f0` (CdSearchFile), read-SM `FUN_8008c294`/done flag
`0x800AC308`/completion `FUN_800899bc`, low-level `CD_cw` loop (`0x8009Axxx`).
- **Override infrastructure DONE + validated:** emitter emits `gen_func_X` (recomp body) +
  `func_X` wrapper checking a runtime override slot; `rec_set_override(addr,fn)`/
  `rec_func_index`. Body kept alive (A/B + diffable), overrides fire on direct+indirect
  calls, super-call = `gen_func_X`. `test_leaf.c` verifies replace/fire/super-call/toggle-off
  (all pass). Matches recomp-overrides skill (runtime table, not compile-time exclusion).
- NEXT (S3): native by-LBA disc backend (flat image or libchdr) + override the CD
  read/resolve/complete fns to use it synchronously; native VBlank/event source for
  WaitEvent. Then verify boot reaches title/FMV. Plan: docs/recomp_port_plan.md.

## 2026-06-14 (later 15) — CD override targets pinned; seed mistake corrected
Mapped the exact functions to override for native-file CD (no emulation), all recompiled:
- **`0x8008B2D8` CdInit** = boot blocker (emits CD_init then CD_cw/CD timeout polling CD I/O
  regs with no IRQ → spins). `0x8008AC34` CD_cw, **`0x8008A6EC`** low-level command+wait
  (CD-timeout chokepoint). `FUN_8008c1ec` read-N@LBA, `FUN_8008c294` read-SM/done
  `0x800AC308`, `CdSearchFile 0x8008b8f0`.
- **Corrected my mistake:** `0x8009A8E8/ADC4/AA4C` were NOT functions — they're mid-function
  jump-table labels inside the **printf/format-parser at `0x8009A76C`** (indirect-only,
  Ghidra-missed), surfaced as misses because computed `jr` → rec_dispatch (no jump-table
  recovery). Replaced those seeds with the real entry. Parser still needs jump-table recovery
  OR a native printf override (the PC-native fix). Not the boot blocker (just debug logging).
- NEXT (S3): native by-LBA disc backend (discdump image / libchdr) + override CdInit +
  command-wait + FUN_8008c1ec to complete synchronously from file; native VBlank/event for
  WaitEvent; verify boot → title/FMV. Override targets all in docs/recomp_port_plan.md.

## 2026-06-14 (later 16) — S3 CD DONE: native by-LBA reads, CdInit/timeouts gone; boot → VSync
Implemented the native CD backend + overrides. **Boot now runs CdInit and CD commands
natively (no controller, no IRQ handshake) and advances past CD into graphics/event init.**
- **Disc backend `runtime/recomp/disc.c`** (libchdr, prebuilt `build/.../libchdr-static.a`):
  `disc_read_sector(lba, out2048)` = hunk-cached CHD read, extracts the 2048-B user data
  (mode-aware offset), same as `tools/discdump.cpp`. Disc path via PSXPORT_TOMBA2_DISC /
  PSXPORT_DISC / `.env`. **Verified standalone:** LBA 16 = `CD001` (ISO PVD), LBA 23 = `PS-X`
  (MAIN.EXE header) — correct bytes by LBA.
- **CD overrides `runtime/recomp/cd_override.c`** (recomp-overrides; bodies kept alive):
  `0x8008B2D8` CdInit → v0=0 (drive ready, skip HW handshake; caller still installs libcd
  callbacks); `0x8008AC34` CdCommand → 0, `0x8008A6EC` CdSync → 2 (the spin-on-DAT_800ac298
  waiters, now moot — every data read is native); `0x8008C1EC` `FUN_8008c1ec(blocks,lba,buf)`
  → reads blocks×2048 from the disc straight into buf, returns 1. Registered by
  `cd_overrides_init()` from boot.c. build.sh links libchdr+lzma+miniz+zstd.
- **Result:** the `CD timeout` / `CdInit: Init failed` spin is GONE. Boot proceeds: ResetGraph
  → **`VSync: timeout`** (next blocker) + event/thread/pad/card BIOS calls now surfacing
  (B0:0x08 OpenEvent, 0x0A WaitEvent, 0x0C EnableEvent, 0x0E OpenThread, 0x4A/4B card,
  C0:0x02/03 SysEnqIntRP, A0:0x70 _bu_init). No `FUN_8008c1ec` read fires yet — the game
  stalls at VSync before it mounts the CD filesystem, so the override read is verified by the
  standalone backend test, not yet end-to-end in-game.
- **NEXT (S3 cont.): native VBlank/VSync + events.** `FUN_80085900` is libetc VSync; the
  vblank counter is **`DAT_800abde0`**; `FUN_80085a78(target)` spins until it reaches target
  → `VSync: timeout` because no IRQ increments it. Fix = a native frame source: override
  `FUN_80085900` to advance `DAT_800abde0` per VSync(0) and return it for VSync(-1); implement
  the event table (OpenEvent/EnableEvent/TestEvent/WaitEvent) in hle.c + deliver VBlank per
  frame tick (transcribe from the proven wide60 hle_kernel.cpp). Then the game should mount the
  CD FS (FUN_8008bbe8 → FUN_8008c1ec reads) and we verify the native read path end-to-end.

## 2026-06-14 (later 17) — events+VSync+threads HLE'd: boot REACHES the StrPlayer main loop
Implemented the rest of S3's "native VBlank/event" surface; **boot now runs into the resident
StrPlayer main loop `FUN_80050b08` (the per-frame game loop)** — verified deterministically
via gdb backtrace (the recomp uses the native C stack, so `bt` names the game fn:
`gen_func_80050B08 <- gen_func_800896E0 <- main`, identical across 3 runs). Leaf tests pass.
- **Events in `hle.c`** (transcribed from proven wide60 hle_kernel.cpp): B0:0x07 DeliverEvent,
  0x08 OpenEvent, 0x09 Close, 0x0A WaitEvent (can't block → reports ready+clears `fired`),
  0x0B TestEvent (read+clear), 0x0C Enable, 0x0D Disable. 16 EvCB slots, id base 0xF1000000.
  Plus B0:0x12-0x16 pad no-ops, 0x4A/0x4B card, C0:0x02/0x03 SysEnq/DeqIntRP→elem, A0:0x70
  _bu_init.
- **Native VSync `runtime/recomp/timing.c`**: overrides libetc VSync `FUN_80085900` — VSync(0)
  advances a native frame clock into `DAT_800abde0`, VSync(-1) queries it. Killed the
  `VSync: timeout` spin (`FUN_80085a78`).
- **BIOS threads (hle.c, STOPGAP):** OpenThread hands back a handle + records entry PC;
  **ChangeThread is a NO-OP** — the static-recomp core runs on the native C stack, so a real
  PC+reg context switch isn't possible by swapping a struct (unlike the wide60 interpreter).
  Fine while boot is straight-line; the StrPlayer FMV prebuffer thread + the 0x80080860
  green-thread coroutine primitives will need a real coroutine override (ucontext/sep stack).
- **CURRENT BLOCKER (next):** the main loop spins at the per-frame **vblank pace-dwell
  `0x80050CE4`** = `do {} while (DAT_800e809c < DAT_1f800235)`. `DAT_800e809c` (display-frame
  counter, 0x800E809C) is bumped by the game's **VBlank ISR callback**, registered at
  `0x80050C58` via `FUN_80085bb0` = libetc **VSyncCallback** (routes to the libapi interrupt
  vector `*(0x800abda0+0x14)(4, cb)` — UNMODELED, so nothing increments it). The callback is
  `&LAB_800506b4`, a **mid-function label** (not a recompiled entry → can't just rec_dispatch
  to it). FIX OPTIONS: (a) override `FUN_80085bb0` to capture the cb addr(es) + seed
  `0x800506b4` as a callable entry (emitter seed) and pump the cb once per frame between the
  counter reset (0x80050C?? sets DAT_800e809c=0) and the dwell — natural pump point is the
  pre-dwell call `FUN_80080f6c(0)`; (b) model the libapi interrupt vector + a frame tick that
  invokes registered class-4 (vblank) callbacks. (a) is the localized, PC-native route.
  Tooling note: **gdb attach + `bt` is the spin locator** for the recomp (C stack == game
  call stack); build `boot_dbg` with `-O1 -g`.

## 2026-06-14 (later 18) — "don't dwell": main loop runs per-frame work; next = BIOS threads
**User steer (saved to memory [[recomp-port-busywaits]]): port HW busy-waits to PC behavior
("make it not dwell"), don't simulate the VBlank IRQ to satisfy them.** Applied: dropped the
vblank-callback capture/seed/pump idea; instead `games_tomba2.c` overrides the per-frame state
update `FUN_800788ac` (sole caller = the main loop, runs once per iteration before the dwell)
to super-call its body then set the display counter `DAT_800e809c` to the quota `DAT_1f800235`
— so the pace-dwell `0x80050CE4` falls through on its first check. (Exactly the state the real
VBlank handler — cb `0x800506B4`, a pure counter increment — would have produced, computed
directly. Host present loop will pace frames later.) `timing.c` VSyncCallback override is now a
clean no-op. **Result: the StrPlayer main loop `FUN_80050b08` runs its per-frame work** —
gdb samples hit varied real fns each tick (libgpu `80083364`/`80081458`, StrPlayer dispatch
`8008179C`, scheduler `80051E60`, memcpy `8009A3E0`); StrPlayer state `DAT_1f80019c`=0.
- **NEXT BLOCKER pinned — BIOS thread context switch.** Still **zero CD reads**: the intro/
  loader **task** (`FUN_800499e8`, registered via `FUN_80051f14(0,…)`) never starts. The
  cooperative scheduler `FUN_80051e60` runs tasks via **BIOS threads**, not custom coroutines:
  disasm of the "context-switch primitives" shows they are plain libapi gate stubs —
  `FUN_80080860`=OpenThread(B0:0x0E), **`FUN_80080880`=ChangeThread(B0:0x10)**,
  `FUN_80080890`/`a0`=Enter/ExitCriticalSection(syscall a0=1/2). The scheduler does
  `state2→ChangeThread(handle)`, `state3→{EnterCS; handle=OpenThread(pc,sp,gp); ExitCS;
  ChangeThread}`. **Our ChangeThread is a NO-OP** (later-17 STOPGAP) → tasks never run.
  FIX = real BIOS threads: give each PSX thread its own **native stack (ucontext/makecontext)**;
  OpenThread creates a context that will enter `gen_func_<pc>`; ChangeThread `swapcontext`s;
  the boot/main thread is also a context. This is the static-recomp coroutine subsystem — the
  one genuinely hard piece. Tooling: gdb attach + `bt` locates the spin/return (C stack == game
  stack); `break gen_func_<addr>` checks whether a fn is reached.

## 2026-06-14 (later 19) — NATIVE BIOS THREADS (ucontext): loader task runs; CD reads in-game
**The hard piece — native BIOS thread context switch — is in (`runtime/recomp/threads.c`),
and the native CD read path is now verified END-TO-END inside the running game.** The
cooperative scheduler's tasks are BIOS threads; disasm confirmed the "coroutine primitives"
are libapi gate stubs: `FUN_80080860`=OpenThread, `FUN_80080870`=CloseThread,
`FUN_80080880`=ChangeThread; a task yields via `ChangeThread(0xFF000000)` (the main/scheduler
thread). `FUN_80051f14` creates each task with `OpenThread(entry, stack, gp)`.
- **threads.c:** each PSX thread gets its own **native stack via ucontext**; `ChangeThread`
  saves the running thread's R3000 regs, restores the target's, and `swapcontext`s to the
  target's native stack (main = slot 0; handles 0xFF0000NN). A fresh thread starts in a
  trampoline that `rec_dispatch`es its entry PC and, on return, switches back to main. The
  single shared R3000 is register-swapped across switches. Overrides the three gate stubs;
  hle.c B0:0x0E/0x0F/0x10 route to the same impl. Replaces the later-17 ChangeThread no-op.
- **VERIFIED:** boot now runs the loader task `FUN_800499e8`, which `CdSearchFile`s
  `\BIN\START.BIN` → our native `FUN_8008c1ec` → real disc reads **LBA 16 (ISO PVD), 18 (path
  table), 373 (dir sector)** — correct bytes, the override read path exercised by real game
  code at last. Cooperative scheduling is healthy (gdb: clean `swapcontext` from
  `FUN_80051f80`, threads round-robining, per-frame memcpy `8009A3E0`).
- **NEXT:** the loader task only *resolves* `\BIN\START.BIN` (stores its descriptor
  `DAT_800be1e0`=LBA) then yields; the actual file-content ReadN hasn't fired yet (still 3
  reads = dir only). Find what consumes the descriptor to load+run START.BIN (likely the next
  scheduler task / StrPlayer state advance), and confirm boot progresses toward the title/FMV.

## 2026-06-14 (later 20) — native file load (FUN_8001db8c); START.BIN loads; overlay exec is next
Boot now loads the first code overlay natively and reaches the **overlay-execution** boundary.
- **The engine's file loader is `FUN_8001db8c(dest, lba, size)`** — NOT FUN_8008c1ec. Its real
  body spawns a reader sub-task (`FUN_8001db38`→`FUN_8001d940`) that issues a raw libcd ReadN
  and copies sectors in a per-sector IRQ callback (`FUN_8001d7c4` = plain `CdGetSector` copy,
  no decompression). That async/IRQ path can't be fed by our no-IRQ overrides → the reader
  looped forever (remaining-count never hit 0). **Overrode `FUN_8001db8c`** (cd_override.c) to
  read `ceil(size/2048)` consecutive sectors from `lba` into `dest` natively, copying exactly
  `size` bytes — faithful (the callback was a plain copy). `FUN_8008a110` confirmed the
  descriptor LBA is absolute (`(min*60+sec)*75+frame-150`).
- **Thread bug fixed:** tasks `CloseThread(self)` then `ChangeThread` away (`FUN_80052078`);
  freeing the live native stack in thread_close → SIGSEGV in munmap. Now thread_close just
  frees the slot; the stack is reclaimed on slot reuse (thread_open), never while live.
- **VERIFIED:** `[cd] loadfile 1648 B @ LBA 1904 -> 0x80106228` — `\BIN\START.BIN` loads to the
  intro-overlay region. No crash.
- **NEXT SUBSYSTEM — overlay code execution.** START.BIN (1648 B) at `0x80106228` IS MIPS code
  (the intro sequencer `FUN_801064f0` lives inside it). The game jumps into it → **miss
  `0x8010649C`**: the `0x80106xxx` overlay region is ABOVE MAIN.EXE's text (`0x800BE800`) and
  was never recompiled. Options: (a) statically recompile the overlay files (START/OPN/GAME/…
  .BIN) with overlay-aware dispatch (they may share the `0x80106xxx` load address → only one
  resident at a time); (b) a hybrid in-RAM MIPS interpreter as the rec_dispatch-miss fallback
  (also clears the printf/SetVideoMode jump-table misses). Decide + implement next.

## 2026-06-14 (later 21) — HYBRID INTERPRETER: overlays run; game executes intro logic
**Overlay code execution solved with a hybrid fallback interpreter (`runtime/recomp/interp.c`).**
The static recomp covers MAIN.EXE's resident text; overlays load from disc at runtime above it
(0x80106xxx) and swap at shared addresses, so they can't be statically recompiled ahead of
time. `rec_interp(c, pc)` runs any non-recompiled RAM code directly from g_ram using the SAME
runtime + the SAME instruction semantics as the emitter (so interpreted == recompiled). Wired
as the `rec_dispatch_miss` fallback for code addresses in [0x10000,0x200000): a jal/jr/jalr
into non-recompiled RAM enters the interpreter; a call back into a recompiled fn routes to
rec_dispatch (`is_recompiled` check). Also clears the in-function jump-table misses (printf
0x8009A8E8, SetVideoMode 0x80091E18) by interpreting from the computed target.
- **VERIFIED:** the START.BIN overlay (incl. intro sequencer `FUN_801064f0`) now executes — no
  misses, no `[interp] bad opcode`. It runs `CdSearchFile` for the next playlist file (new read
  LBA 1905), and the game progresses through its **timer-paced task schedule** (task-0 state
  1→2 as its timer expires over ~8s). Leaf tests still pass. The recomp core stays 100%
  recompiled; only dynamically-loaded overlay code is interpreted (legit hybrid execution).
- **STATE: the game boots MAIN.EXE and runs its full software stack** — HLE BIOS, libcd (native
  file I/O), libetc VSync, events, the cooperative scheduler on real native threads, overlay
  load + execution, and the StrPlayer main loop drawing each frame. It advances the intro logic
  but invisibly: the next subsystems are the **output/IO peripherals — GPU (rasterizer +
  display), MDEC (FMV video), SPU (audio), pad input** — to be lifted from the Beetle GPL-2 fork
  per the plan. GPU first (so output is visible/verifiable). These are the large remaining tier.

## 2026-06-14 (later 22) — GTE (COP2) LIFTED from Beetle: real geometry coprocessor
First peripheral-tier lift: the **GTE is now Beetle's real implementation**, not a no-op stub.
All the game's geometry (RTPS/RTPT projection, NCLIP, matrix/color/depth) flows through COP2;
our stub silently zeroed it, so any 3D was inert.
- **`runtime/recomp/gte_beetle.c`** compiles `vendor/beetle-psx/mednafen/psx/gte.c` as-is and
  adapts it to our interface: `gte_op`→`GTE_Instruction`, `gte_read/write_data`→`GTE_ReadDR/
  WriteDR`, ctrl→`GTE_ReadCR/WriteCR` (1:1). Faithful-first shims for the externs gte.c needs
  (PGXP off `gMode=0`/no-op NCLIP, savestate stub, **widescreen GTE-scale hack OFF** — that's
  the wide60 tier later). `gte_init()` (GTE_Init+Power) called from boot. stubs.c keeps only
  COP0. build.sh adds the mednafen/libretro-common include paths.
- **VERIFIED:** standalone RTPS — identity rotation, vertex (64,0,256), H=256 → **SX=64**
  (= IR1·H/IR3 = 64·256/256), the projection is bit-correct. Builds clean, leaf tests pass, no
  boot regression. (No GTE ops fire during the 2D logo intro — expected; GTE is gameplay 3D.)
- **Lift pattern established** (compile the Beetle C module as-is + a thin adapter + faithful
  externs) for the remaining peripheral tier: **GPU** (the big one, needed for visible output),
  then MDEC, SPU, pad.
- **GPU lift SCOPED (next):** software path is viable — `rhi_intf.c` defaults `rhi_type =
  RHI_SOFTWARE` and the GL/Vulkan backends are `#ifdef HAVE_OPENGL/HAVE_VULKAN` (omit those
  defines → software only, headless). Files: `gpu.c` (command processor, 95KB) +
  `rhi_intf.c` (renderer dispatch) + `gpu_polygon.c`/`gpu_sprite.c`/`gpu_line.c`/
  `gpu_polygon_sub.c` (software rasterizer). `gpu.c` interface: `GPU_Write(ts,A,V)` (GP0/GP1),
  `GPU_Read(ts,A)` (GPUREAD/GPUSTAT), `GPU_WriteDMA/ReadDMA/DMACanWrite` (GPU DMA),
  `GPU_Update(ts)` (scanline timing), `GPU_StartFrame(espec)` (render to a surface),
  `GPU_Init/Power`. **Shim surface ~54 externs** (gpu.c) + rhi_intf.c's settings/libretro deps:
  IRQ_Assert, TIMER_* (dot/hretrace/vblank), PSX_SetEventNT/EventCycles, ReadMem (→ our
  mem_r32 for DMA), PGXP_* (off), psx_gpu_* config globals, rhi_lib_* (omit). **Wiring work:**
  route mem.c 0x1F801810/14 ↔ GPU_Write/Read; model **DMA channel 2** (GPU DMA, the
  ordering-table linked-list walker the game uses — `FUN_80082d04` submits OTs) feeding
  GPU_WriteDMA; provide the VRAM/scanout surface + a present/dump path; feed a synthetic
  timestamp (we pace via VSync, not cycles). Largest single lift; needs iterative verification.

## 2026-06-14 (later 23) — DIRECTION: NATIVE rendering, not PSX-GPU emulation (user)
**User: "make the game itself do PC native rendering instead of PSX emulated rendering."** So
we do NOT lift Beetle's PSX GPU (that's emulated rendering). Instead the game submits its draw
primitives as GP0 command packets (its output protocol) via **GPU DMA channel 2 walking
ordering-table linked lists**; we parse that stream and rasterize it with **our own native
renderer** to a window, at our chosen resolution. No PSX GPU hardware emulation. This is the
from-scratch native renderer the wide60 plan already chose, and it makes widescreen/60fps
natural. (The Beetle-GPU-lift scoping in "later 22" is therefore superseded for rendering — but
the lift pattern + GTE stay; GTE projects the geometry whose 2D primitives we then draw.)
- **Intercept point:** GPU DMA2 (`0x1F8010A0/A4/A8`) linked-list walker → GP0 packet parser;
  direct GP0/GP1 (`0x1F801810/14`) writes; GPUSTAT reads report DMA/cmd ready (game polls
  `&0x4000000`). Game submits OTs via libgpu (`FUN_80082d04`/`FUN_80082fb4` queue+DMA).
- **Native GPU module (building):** VRAM (1024×512×16b for textures + framebuffer) + GP0
  parser (draw-env/texpage/clut, fill, VRAM load/store/copy, flat/gouraud/textured tri+quad,
  sprites/rects, lines) + software rasterizer with VRAM texture+CLUT sampling + GP1 display +
  present (PPM dump headless / SDL window). Built ground-up so resolution/widescreen are ours.

## 2026-06-14 (later 24) — MDEC + SPU lifted (parallel subagents) + SDL window; all integrated
PM-mode session: two developer subagents lifted **MDEC** (`mdec_beetle.c`) and **SPU**
(`spu_beetle.c`) from Beetle in parallel, each following the `gte_beetle.c` template
(compile the Beetle .c as-is + thin adapter + faithful externs). Both verified standalone
(MDEC status correct post-reset; SPU produced exactly 735 stereo frames/NTSC). PM integrated:
- **MDEC** wired in mem.c: regs MDEC0 `0x1F801820` (data) / MDEC1 `0x1F801824` (ctrl/status);
  **DMA0** (MDEC-in, RAM→decoder) and **DMA1** (MDEC-out, decoder→RAM), block-mode. `mdec_init`
  from boot. (Note: `mdec_dma_out` drains linearly — ignores the per-word macroblock scatter
  offset; if FMV pixels come out mis-ordered, switch DMA1 to `MDEC_DMARead(&offs)` placement.)
- **SPU** wired into the build + `spu_init` (register/DMA4/audio-pull wiring + an SDL audio
  sink is the remaining step; module links & runs). STOPGAPs noted in spu_beetle.c: IRQ_Assert
  and CDC_GetCDAudioSample (CD-DA) need routing later.
- **SDL live window** (`gpu_native.c`, `PSXPORT_GPU_WINDOW=1`): the native framebuffer in a
  real window (3× scale), SDL always linked, opens on demand. Headless PPM dump still works.
- **Dedup:** the three adapters each defined `MDFNSS_StateAction` → multiple-definition link
  error; kept one copy (gte_beetle.c), removed the others. Builds clean, leaf tests pass, no
  boot regression (START.BIN still loads, scheduler runs).
- **WHY no FMV yet:** the logo plays through the StrPlayer's **async streaming** CD path
  (`FUN_8008c960` ReadN + per-sector IRQ callbacks → MDEC), which we have NOT overridden — only
  the synchronous reads (`FUN_8008c1ec`, `FUN_8001db8c`) are native. So MDEC is integrated and
  ready but never fed. **NEXT:** wire the StrPlayer streaming read natively (feed stream sectors
  → MDEC decode → VRAM upload), the intro-sequencing path the earlier "later 4-8" work mapped.

## 2026-06-14 (later 25) — MDEC placement + SPU audio integrated; one-shot run.sh (macOS+Linux)
PM sprint, two more developer subagents (both delivered + verified):
- **MDEC DMA-out macroblock placement** (mdec_beetle.c): `mdec_dma_out` now PLACES each word at
  `buf[i + offs]` matching Beetle's DMA1 `CH_MDEC_OUT` (verified: `i+offs` is an exact
  permutation of [0,total), stride 6=24bpp / 4=16bpp; value-for-value vs a reference scatter).
  PM wired DMA1 in mem.c to clear+drain+copy the full post-scatter region (the interleave
  reaches forward; copy the whole MADR transfer, not the return count).
- **SPU audio output** (spu_audio.c, SDL): `spu_audio_init` (44100/S16/stereo, lazy, gated by
  PSXPORT_NOAUDIO) + `spu_audio_frame` (advance 564480 sys-clocks = 735 frames, drain, queue,
  4-frame cap). PM wired the SPU register file `0x1F801C00-1FFF` + DMA4 in mem.c, `spu_init`/
  `spu_audio_init` at boot, `spu_audio_frame` once per frame (sole spu_update driver).
- Builds clean, leaf tests pass, no boot regression. (MDEC/SPU end-to-end output is untestable
  until the intro streams — see below — but both are wired + unit-verified.)
- **`run.sh` (repo root) — fully automated, macOS + Linux:** resolves the disc (arg / env /
  .env / *.chd drop-in), CMake-builds libchdr + discdump, extracts MAIN.EXE, recompiles the
  core + builds the native runtime, launches in an SDL window. macOS-aware: `_XOPEN_SOURCE=700`
  (ucontext), pkg-config sdl2, getconf/sysctl cores, no `timeout`/GNU-isms, brew hints.
  Verified end-to-end on Linux (builds `scratch/bin/tomba2_port`, runs, CD reads). Knobs:
  PSXPORT_NOAUDIO / PSXPORT_NOWINDOW / PSXPORT_GPU_DUMP / CC.
- **Critical-path status (visible output):** the StrPlayer **streaming** read (`FUN_8008c960`)
  is NEVER reached — the game inits MDEC (`FUN_8009C620`) but the interpreted intro overlay
  (START.BIN) stalls before chaining to load/play the logo, so no FMV. Only 5 CD reads ever.
  This is the deep intro-sequencing blocker ("later 2-8"); needs further RE of what the
  interpreted sequencer waits on (logic/state vs an interp-correctness gap). Single-owner next.

## 2026-06-14 (later 26) — DITCH GHIDRA (binary-only) + parallel shard build + run.sh; R/B fix
Reproducibility + build-speed sprint (user-driven). The build now needs **only the repo + the
ROM** — no Ghidra, no committed decomp-derived data.
- **Binary-only recompilation:** `emit.py` seeds purely from the binary now — `{entry} | EXTRA_
  SEEDS`, grown by `discover_funcs` (direct-jal fixpoint). 1154 functions recompiled; the ~445
  reached only via function pointers run through the hybrid interpreter (faithful). **Verified
  identical boot** to the Ghidra-seeded build (same CD reads, START.BIN@1904) — and the printf
  jump-table now prints clean strings (`ResetGraph:jtb=…`, `MDEC_in_sync timeout:`) since the
  interpreter handles it. `PSXPORT_USE_GHIDRA=1` (+ local scratch decomp) still available to
  recompile more for speed; default doesn't touch Ghidra. Repo audited clean: scratch/ (decomp
  dump) gitignored, the optional address list gitignored — only our own Ghidra *tooling* scripts
  remain (don't ship decomp output).
- **Parallel build:** `emit.py` splits output into `generated/rec_decls.h` + 8 `shard_<n>.c`
  (gen_func bodies, round-robin) + `shard_disp.c` (override table + wrappers + dispatch
  switches). `run.sh` compiles all TUs to .o with `xargs -P` then links (`-j16` observed); old
  monolith path stubbed. `PSXPORT_SHARDS` tunable.
- **`run.sh` (repo root): one command, repo + ROM only.** CMake-builds libchdr/discdump,
  extracts MAIN.EXE, binary-only recompiles, parallel-builds, launches the SDL window. macOS
  fixes from user testing: committed func-list dependency removed (was the Mac blocker),
  libchdr *header* path (source tree) vs *.a (build/), no pipefail+`ls`/`head` footgun. Verified
  end-to-end on Linux; built + ran the game loop.
- **Rasterizer R/B-swap fixed** (gpu_native.c `cmd_r`/`cmd_b`): GP0 color packs `0x00BBGGRR`
  (R=low byte). Found by the GPU-QA subagent (which otherwise proved the rasterizer's geometry/
  fill/gouraud all pixel-correct). build.sh leaf tests pass; no boot regression.

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

## 2026-06-17 (later-89) — HW renderer M3: textured GPU rasterization (CLUT-in-shader) renders Tomba2; draw-area clip fix.
Textured triangle pipeline (tritex.vert/frag): samples a VRAM snapshot (avoids render/sample feedback
loop) with 4/8/16bpp + CLUT lookup + texture-window, affine (noperspective) UV, per-pixel modulation,
transparent-texel discard, exactly matching SW sample_tex's addressing. Per-prim state (texpage, CLUT,
window, draw-area) carried as flat vertex attributes -> one draw call. Tee'd from gp0_exec (textured
opaque polys). Diff harness (PSXPORT_VK_DIFF) draws untextured + textured over the uploaded SW VRAM.
- **Renders correctly:** title screen + the in-engine demo scene (structure/lava/character/HUD) all
  render via the GPU textured path, visually matching SW.
- **DRAW-AREA CLIP was the key fix:** SW clips polys to s_da_*; VK didn't, so polys overdrew the atlas/
  top region (big spurious block). Per-prim draw-area discard in the shader: f3000 demo 19.0% -> 14.4%
  mismatch. Texture window added (no measurable change on this scene; correct to have).
- **Residual ~14% on the busy demo** = sub-pixel edge-coverage + UV/color rounding differences on ~944
  small tris (GPU pixel-center vs SW integer-coord rasterization) — visually invisible (scene matches),
  same class as the SW-vs-Beetle residual. Reducing it = matching SW's exact fill/rounding rules (later).
- OPEN: semi-transparency (4 blend modes; skipped in the tee), sprites/rects/lines/fills (M4), then
  switch present to VK VRAM (M5). PSXPORT_VK=1 windowed; SW path + default untouched.

## 2026-06-17 (later-90) — HW renderer M3/M4 VISUALLY VALIDATED (user can't tell VK from SW).
User compared the VK render vs SW render of the demo scene front buffer (vk_out/sw_out): "can't even
tell them apart." So the GPU textured pipeline (M3 polys + M4 sprites, CLUT-in-shader) renders Tomba2
correctly; the ~14% pixel "mismatch" was invisible off-by-1 (ordered dither + UV/color rounding, not
implemented to match SW exactly — and not worth chasing per the user). Validation method going forward:
SEND renders for the user to eyeball, don't naive-pixel-diff. M4 sprites tee'd (rects as 2 tris).
- REMAINING for VK to BE the renderer (M5): VK owns VRAM (CPU uploads, VRAM->VRAM copies, fills, lines),
  semi-transparency (4 blend modes — needs VK-owned VRAM to matter), then present from VK VRAM + retire
  SW (default-on). Until then VK renders the tee'd prims over the uploaded SW VRAM (validated identical).

## 2026-06-17 (later-91) — HW renderer COMPLETE: Vulkan is the DEFAULT renderer; SW retired to oracle/fallback.
M5 finished. VK owns VRAM and renders every PSX primitive type (polys, sprites, lines-as-quads, fills/
copies/uploads via dirty-region mirroring, semi-transparency 4 blend modes). gpu_vk_enabled() now
DEFAULTS ON for windowed runs; PSXPORT_SW_GPU=1 (or PSXPORT_VK=0) forces the SW rasterizer (the proven
oracle). Headless always stays SW (no window -> no VK). Validated: 5000-frame windowed run across boot/
title/ship-demo/field-demo/tutorial = ZERO validation errors (RADV); frames render indistinguishably
from SW (user-confirmed "looks great"). PSXPORT_VK_SHOT=frame dumps the live VK frame.
- Architecture per frame: mirror SW-written dirty regions -> snapshot textures -> OPAQUE pass ->
  snapshot post-opaque framebuffer -> SEMI pass (samples snapshot as texture + blend dest) -> present.
- OPEN (refinements, not blockers): strict per-op draw order (currently opaque-batch then semi-batch =
  standard separation, fine for Tomba2); residual reduction (off-by-1 dither/rounding); perf numbers.
- This is the foundation the user wanted before widescreen + object interpolation.

## 2026-06-17 (later-92) — Widescreen WORKS on Tomba2 (falsifies the old "ineffective" note) + extended culling.
With the native VK renderer owning display, the GTE widescreen hack works on Tomba2:
- **PSXPORT_WIDE=1**: gte_init() sets widescreen_hack=1 + aspect 16:9 (squish projected X around centre);
  gpu_vk present fits 16:9 instead of 4:3 -> wider horizontal FOV. (Old later-era note "ineffective on
  Tomba2" was on the oracle; FALSIFIED here.) 2D/sprites bypass the GTE (HUD-stretch is the next per-game item).
- **PSXPORT_CULL=1 (extended culling, user-requested):** the game's FUN_8007712c culls each object by
  distance AND a FOV cone (depth/dist < ~0x370 ≈ ±77°) — over-culls (pop-in; widescreen edges dropped).
  ov_object_cull (game_tomba2.c) now, after the game's cull, RE-INCLUDES objects it dropped that are
  within an extended distance (PSXPORT_CULL_FAR, def 0x6000 ≈ 3.4x the 0x1c00 max) + wider cone
  (PSXPORT_CULL_FOV, def 0x80 vs 0x370): mark visible@+1, return 1. Near/behind culling kept intact.
  Verified: more right-edge structure/objects render in widescreen. Tunable via the two envs.
- OPEN: HUD stretch under 16:9 (sprites need un-stretch/reposition); the re-included far objects rely on
  the +1 visible flag driving the draw (works in test); tune thresholds with the user.

## 2026-06-17 (later-93) — PC-native widescreen: TRUE wider FOV, NO squish/stretch (replaces the rejected hack).
User REJECTED the old PSXPORT_WIDE (Beetle GTE squish-X 0.75 + display-stretch 4:3->16:9): "we are making
a PC game, don't squish anything." Replaced with a genuinely native renderer-side widescreen:
- **Key insight (no squish):** keep the GTE's NATIVE projection scale; to show a wider FOV, just
  RE-CENTER the framebuffer-local view into a WIDER target. Math: widescreen_x = native_x + (FBW-320)/2
  (a constant shift, NOT a 0.75 scale) — same per-unit scale (full horizontal resolution preserved),
  +54px of world on each side. The GTE already projects geometry past the 4:3 edges (measured earlier:
  ~11% in the right band); it was only being clipped. So widescreen = native projection + widened clip
  + wider target. widescreen_hack now hard-OFF in gte_beetle.c.
- **Where the wide image lives (VRAM is fully packed — can't widen in place):** the VK R16_UINT image is
  grown VRAM_H(512) -> IMG_H(992); rows [512, 992) are a VK-only scratch framebuffer (FB_Y0=512, up to
  856x480 = 16:9 @ 2x). gpu_vk.c relocates the tee'd geometry into the FB via a VERTEX push-constant
  transform (tritex.vert): local = i_pos - i_da.xy (da.xy = active framebuffer origin), fb =
  ((local.x+WIDE_OFF)*ss + fb_x0, FB_Y0 + local.y*ss); clip overridden to the FB rect so wide geometry
  isn't dropped. Textures still sampled from VRAM rows <512 (unchanged). Present samples the 16:9 FB 1:1
  (no stretch). Reuses the whole existing pipeline (image/renderpass/snapshot/semi); non-wide path is
  byte-identical (verified: f1500 nowide == baseline). PSXPORT_SS (1..2) supersamples (FB 856x480).
- **Cull coupled to widescreen (the user's point: "I thought we ported the culling and adjusted it"):**
  the static terrain/water TILES also go through the per-object cull, so the wider FOV needs a wider
  re-include cone + farther distance or the new edges/corners stay black. ov_object_cull now AUTO-widens
  when PSXPORT_WIDE is on (PSXPORT_WIDE implies cull, defaults fov 0x00 / far 0x8000 vs 0x80 / 0x6000 for
  plain CULL). Fills the deep-water horizon + side bands; only the water tile-grid's TRUE outer edge
  leaves tiny corner wedges (would need bg-plane extension to kill — deferred).
- Validation-layer clean (RADV). User-shown f1500/f3000/f700: correct proportions, more world each side,
  HUD at native scale + centered (NOT stretched). OPEN/next (user: "all of these"): SSAA downsample
  present shader, HUD edge-anchoring (2D sprite path), shaders/lighting. WATCH: aggressive cull can cause
  entity walk-through ghosts (journal later-52) — needs playtesting now that the user is engaged.
- **later-93b (same session):** PSXPORT_SS now defaults to 2 (FB 856x480 = fills the image exactly;
  sharper than 428x240 upscaled to the window). **HUD edge-anchoring done** (gpu_vk_sprite_anchor_dx,
  wired in the gp0 0x60-0x7F sprite tee in gpu_native.c): 2D sprites bypass the GTE, so instead of the
  renderer centering them, each sprite shifts by (Xc-160)*(FBW/ss-320)/320 native px before the ss scale
  — Xc=160 stays centered, Xc=0 pins to the new left edge, Xc=320 to the right. Native size preserved.
  Verified f3000: score/items move to the corner (cmp_hud_left.png). Inert when not wide.
- **STILL OPEN (user: "all of these"):** (1) true hi-res beyond the 1024-wide image cap — needs a
  DEDICATED FB image (the cram-into-VRAM-rows trick caps FBW at 1024; the semi-blend frag samples one
  combined sampler for textures<512 AND the FB blend-dest, so a separate FB needs a 2nd sampler binding
  + frag change). (2) shaders/lighting — wants an RGBA8 FB + post passes (same dedicated-FB refactor).

## 2026-06-17 (later-94) — MEMORY CARD now works (was hung on "Checking MEMORY CARD..."). PC-native, zero delay.
User: "memory card doesn't work." Symptom (reproduced via tools/drive.py: title -> Load Game -> slot):
the Load screen hung forever on "Checking MEMORY CARD...".
- **Root cause 1 — card I/O primitives were UNIMPL in the HLE.** memcard.c set rec_set_override on BIOS
  addresses 0x8009xxxx, but those addresses NEVER execute in this pure-HLE-BIOS build — the game's
  statically-linked libcard/libmcrd calls the BIOS via the `li t0,0xB0; jr t0` trampoline, which funnels
  to rec_dispatch_miss -> recomp_hle (hle.c). recomp_hle only handled B0:0x4A/0x4B; _card_read(B0:0x4E),
  _card_write(B0:0x4F), _card_status(B0:0x5C), _card_info(B0:0x4C), _card_chan(B0:0x50), and the A0-table
  _card_info(A0:0xAB)/_card_load(A0:0xAC) all fell through to "UNIMPL". Fix: `card_hle_a0`/`card_hle_b0`
  in memcard.c, dispatched from the A0/B0 default cases in hle.c; they do SYNCHRONOUS host-file I/O
  (card_read_frame/card_write_frame) = PC-native, zero delay. Dead rec_set_override calls removed.
- **Root cause 2 — completion delivered to the WRONG event class.** libcard completion is event-based;
  this HLE has no SIO IRQ, so the override must DeliverEvent the completion itself. Captured (PSXPORT_EV_LOG)
  the game's "checking" loop: it TestEvents the **SwCARD class 0xF4000001** (NOT HwCARD 0xF0000011) every
  frame for spec EvSpIOE(0x0004). `card_deliver_complete` now fires SwCARD+HwCARD EvSpIOE (NOT NEW/0x2000,
  which would mean unformatted -> format prompt). hle_deliver_event only fires open+ENABLED matching slots.
- **VERIFIED (drive.py):** Load -> "Select slot" (slot 1/2 detected) -> select slot 1 -> reads dir frame 63
  -> **"No data for Tomba!2"** (correct for an empty card; was a hang). scratch/screenshots/card_final.png.
  Card file = scratch/saves/tomba2.mcr (128 KB). OPEN: WRITE/save round-trip not yet tested end-to-end
  (needs a save-point playthrough) — but it's the same now-proven completion path (B0:0x4F + SwCARD IOE).

## 2026-06-17 (later-95) — Vertex smoothing (PGXP subpixel) — kills the PS1 vertex wobble, PC-native.
PSX projected 3D vertices to INTEGER screen coords, so geometry jitters as the camera/object moves
(the classic PS1 "wobble"). Beetle's GTE ALREADY computes the subpixel-precise projected coords
(gte.c TransformXY -> precise_x/y/z) and hands them to PGXP_pushSXYZ2f — which was a dead no-op stub
in gte_beetle.c. Implemented it for real instead of leaving 3D snapped-to-integer:
- **gte_beetle.c:** PGXP_pushSXYZ2f now caches (precise_x,y,z) in a 32K-entry hash, keyed by the
  packed integer SXY (`v` = XY_FIFO[3]) the game copies verbatim into its GP0 vertex packets. Exposes
  `pgxp_lookup(sx,sy,&px,&py,&pz)` (1=hit) and `pgxp_frame_reset()`. **Value-keyed PGXP-lite:** on a
  key collision lookup just misses -> integer fallback, so a wrong match can only cost smoothing, never
  correctness. **Reset every presented frame** (gpu_native gpu_present_ex) so a stale precise value from
  a prior frame can't be re-applied to a freshly integer-placed vertex (the cross-frame-wobble trap).
- **gpu_vk.c:** added float-position draw entries `gpu_vk_draw_tritri_f`/`gpu_vk_draw_semi_f` (TexVtx.x
  was already float; the vertex shader already divides floats). The old int entries are now thin
  wrappers that widen to float -> one impl. (+ stubs in the no-VK build.)
- **gpu_native.c:** ONLY the polygon tee (op 0x20-0x3F = GTE-projected 3D) looks up the precise coords
  by each vertex's integer (v[i].x,v[i].y) and passes FLOAT subpixel positions (+s_off). Sprites
  (0x60-0x7F, bypass the GTE / 2D HUD) and lines stay integer — correct, they have no GTE-precise coord.
  Gated PSXPORT_PGXP (default ON; =0 = exact old integer behavior for A/B vs the oracle).
- **VALIDATED (f1500 swing/water, f3000 lava):** geometry fully intact, nothing warped/missing in either
  scene. On-vs-off diff (scratch/screenshots/pgxp_diff_1500.png): differences are concentrated on the
  POLYGON EDGES of every 3D primitive = subpixel vertex repositioning (proves broad cache-hit coverage),
  exactly the expected signature. widescreen_hack is OFF so precise_x is native (no squish baked in).
  NOTE: wobble reduction is a TEMPORAL effect — stills can't show it; user judges in motion. OPEN: if
  motion reveals mis-snap (value-key collisions), add PGXP-proper RAM-address tracking (handoff note).

## 2026-06-17 (later-96) — Lighting/shading model REVERSE-ENGINEERED (groundwork for a native lighting engine).
User goal: RE the game's rendering/lighting so we can intercept + replace it PC-native (e.g. a real
lighting engine instead of the PSX one). Built `PSXPORT_GTEPROBE=<frame>` (gte_beetle.c): dumps the GTE
ops that ACTUALLY execute + a lighting/fog control-register snapshot; corroborated by a static histogram
of every `gte_op(...)` immediate in `generated/shard_*.c`. **Full model now in docs/engine_re.md.** Result:
- **NO dynamic GTE lighting at all** — `NCDS/NCDT/NCCS/NCCT/NCS/NCT/CC/CDP` = 0 executions, 0 call-sites.
  No light sources, no normal·light-matrix shading.
- **Vertex colors are BAKED** in model data; **`GPF`** (very high count) scales them by a scalar IR0
  (per-object brightness / fade). **`DPCS`/`DPCT` depth-cue = the atmosphere "lighting":** color lerped
  toward **FarColor (CR21-23)** by `IR0=DQB+DQA·H/Sz`. FarColor is **scene-tinted** — f1500 water=(0,0,0)
  fade-to-black, f3000 lava=(1280,0,0) red; DQA=6/DQB=0 both. (RTPS/RTPT/MVMVA/NCLIP/AVSZ run as expected.)
- **Interception point:** final per-vertex RGB → GP0 gouraud polys → gpu_native gp0_exec tee (rs/gs/bs).
- **Unlock for native lighting:** PGXP (later-95) already caches per-vertex screen x/y + precise_z, so we
  can unproject to view-space position and derive per-FACE normals (edge cross-product) IN THE RENDERER —
  enabling native directional/point lighting, normal shading, SSAO, and a replacement per-pixel fog (tint
  from CR21-23). No need to fight any existing dynamic lighting (there is none). NEXT: pick the lighting
  style + build the normal-reconstruction + shader path (scope question to user).

## 2026-06-17 (later-97) — NATIVE LIGHTING ENGINE: normal+depth reconstruction + directional light + fog.
User scope (AskUserQuestion): "Full pipeline (normals first)" — build the shared normal/depth
reconstruction, then layer directional light + fog + AO. Built the foundation + first two layers:
- **View-space position capture (gte_beetle.c):** PGXP cache now also stores the GTE's view-space vertex
  (IR1/IR2/IR3 = rotation·V + translation, read via GTE_ReadDR(9/10/11) at PGXP_pushSXYZ2f time, where
  TransformXY hasn't touched IR yet). New `pgxp_lookup_view(sx,sy,...)`. This is the ONLY normal source —
  the game has no GTE lighting (later-96).
- **Per-face normal (gpu_vk.c tex_emit):** for each triangle with 3 view-space verts (looked up in the
  polygon tee, gpu_native.c), normal = normalize(cross(e1,e2)), oriented toward the camera at the origin
  (flip if dot(N, faceCenter) > 0). depth = view Z. Passed as new TexVtx fields nx/ny/nz/depth (vertex
  attrs loc 7/8). 2D sprites/lines/HUD pass NULL view -> zero normal -> shader leaves them untouched (they
  bypass the GTE so they'd never hit the cache anyway). If ANY vert misses the cache, the whole tri is unlit.
- **Shaders (tritex.vert/frag):** frag gets a fragment push-constant LPC (offset 48): l0=(dir.xyz view
  space, mode 0=off/1=directional/2=normal-viz), l1=(ambient,diffuse,fogNear,fogFar), l2=(fogTint.rgb,
  fogEnable). Lighting modulates the SOURCE color (unpacked from 555 -> float -> relight -> repack) BEFORE
  the semi-blend. Output is still R16_UINT 555 (5-bit) — quality ceiling until the dedicated-RGBA8-FB
  refactor (handoff). Native fog = mix(color, tint, depth ramp).
- **Config (gpu_native.c, env one-shot):** PSXPORT_LIGHT=0/1/2, PSXPORT_LIGHT_DIR="x,y,z",
  PSXPORT_LIGHT_AMB / PSXPORT_LIGHT_DIFF, PSXPORT_FOG=1 + FOG_NEAR/FAR/RGB. Default OFF (byte-identical to
  later-96 unless enabled). `gpu_vk_set_light()` pushes the LPC each tritex batch.
- **VALIDATED:** normal-viz (PSXPORT_LIGHT=2, scratch/screenshots/normviz_1500.png) shows the 3D world
  cleanly colored by face orientation — vertical tower uniformly green, ground red, catapult/barrel faceted
  = reconstruction is CORRECT. Directional light (=1) shades the world (tower left-dark/right-bright, grass
  gradient), 2D HUD + water untouched; RMSE vs unlit 0.04. Validation-layer clean. By-eye (no oracle — this
  is a new PC-native feature Beetle lacks). OPEN/NEXT: SSAO (needs depth attachment + dedicated FB), world-
  space light (transform dir by the GTE rotation matrix CR0-4 so the sun is camera-stable), RGBA8 FB to lift
  the 5-bit banding, smooth (per-vertex) normals if flat shading looks too faceted.

## 2026-06-17 (later-98) — DIRECTION RESET: no GP0-stream tricks; RE the engine, port it native.
User rejected the renderer-side tricks I'd been adding (supersampling, PGXP value-keyed vertex smoothing,
lighting read out of the emulated GTE): "I don't want super sampling or other tricks, I want you to reverse
engineer and port the game engine to PC native so we can tweak those natively." Then: build tooling that
runs BOTH cores side-by-side synced on a GAME STATE (attract/demo start), not frame numbers — but RE the
game more FIRST. Memories: [[pc-native-not-emulator-hacks]] (updated), [[dual-core-state-synced-diff]] (new).
- **De-tricked the defaults:** PSXPORT_SS=1 (native res), PSXPORT_PGXP=0 (the value-keyed smoothing was the
  likely water-grid breaker). Trick code kept only as opt-in env flags pending the native port. (uncommitted)
- **RE win — projection setup found (docs/engine_re.md):** `gen_func_800509B4` (0x800509B4) does InitGeom +
  `SetGeomOffset(160,120)` + `SetGeomScreen(350)` → screen center (160,120), focal length H=350, 320×240.
  This is the **native widescreen lever**: override to OFX=214 + widen draw-env/clip to 428, keep OFY/H →
  the GTE projects a genuinely wider FOV, no squish, no renderer re-center. (Found by histogramming
  gte_write_ctrl reg targets in generated/: OFX/OFY/H written at exactly 2 sites.)
- **Water:** established (from the lighting normal-viz) that water is NOT GTE-projected — it's a separate
  screen-space layer (terrain gets normals, water doesn't). The user reports it rendering wrong (green
  smear). NOT yet root-caused. NEXT RE: provenance (`PSXPORT_PROVAT`) on a water pixel → owning node/handler
  → read in decomp; compare vs oracle at a synced game state (dual-core harness).
- **Dual-core harness BUILT (`tools/dualcore.py`):** runs port + oracle together, each on its own FIFO,
  gates both to a guest-RAM **state latch** (not frame number). Found the right latch: **0x800BE258==2**
  = scene/field active (port in-demo=2, oracle title=0); the stage word 0x801fe00c is too coarse
  (0x801062E4 = attract = title AND demo both; 0x8010649C = START/logo). `sync`/`step`/`shot` (side-by-side
  + diff heatmap). **Limitation found:** the latch catches the scene-LOAD edge (oracle shot can be black)
  and the two cores' attract demos are NOT frame-locked — equal `step` drifts (port demo cycles back to
  title while oracle still mid-scene). NEXT: a `loadram` port REPL cmd to load an identical 2MB guest-RAM
  water-scene snapshot into BOTH cores (oracle `-loadstate`), so both rebuild the same frame from RAM →
  drift-free render compare. THEN root-cause the water. (docs/diff-driver.md updated.)

## 2026-06-17 (later-99) — Graphics OWNERSHIP: scene classifier + first engine fns ported native (0-diff).
Direction: stop black-boxing the graphics; reimplement the engine's draw path in native C. Done + verified:
- **Scene classifier** (PSXPORT_SCENEDUMP, gpu_native.c): native read-only OT walk classifying every prim
  (poly/rect/fill/VRAM-copy/env). The port now ACCOUNTS for each draw. Finding: 2 DrawOTag/frame (clear +
  main), water = textured GEOMETRY (no reflection copy) → broken water was the PGXP trick (now off).
- **GTE projection ported native** (game_tomba2.c ov_set_geom_offset/ov_set_geom_screen, 0x800846D0/F0):
  writes CR24/25/26 directly; OFX=160/OFY=120/H=350. **0-pixel-diff** vs recomp (PSXPORT_GEOM_RECOMP=1).
- **DrawOTag ported native** (ov_draw_otag, 0x80081560 → our gpu_dma2_linked_list): the per-frame draw
  submission routes through our native OT walk. **0-pixel-diff** vs recomp (PSXPORT_OT_RECOMP=1) at f1500.
- **libgpu fn labels corrected** from debug strings: FUN_80080f6c=DrawSync (NOT DrawOTag), 80081560=DrawOTag,
  800815d0=PutDrawEnv, 8008179c=PutDispEnv. engine_re.md fixed.
- **Fade flash root-caused**: the engine's prologue fade-in is a SMOOTH modulation-color ramp (frame-stepped,
  no overlay) — so the flash is RENDERER-side (VK present / FMV handoff), not the engine.
- **Widescreen (native) attempt — REVERTED, blocked:** moved the horizontal shift into the engine projection
  (OFX 160→214, WIDE_OFF→0) — math-equivalent to the proven later-93 re-center, and OFX is correct
  (CR24=0x00D60000, 428x240). BUT the VK wide-FB shows **regular vertical black bars**, and the PROVEN
  later-93 config (WIDE_OFF=54, OFX=160) shows them TOO → a **pre-existing VK wide-FB regression** (capture
  or render), NOT my OFX change. Readback buffer is correctly sized (VRAM_W*IMG_H), so not a shot-size bug;
  cause not yet found (suspect a change between later-93 and now). Reverted the wide experiment to keep the
  tree clean; widescreen is off by default (./run.sh = 4:3, unaffected). NEXT: isolate the wide-FB bars
  (bisect gpu_vk since later-93 / compare live-window vs VK_SHOT), then re-land native widescreen on OFX.
- **later-99b — wide bars ROOT-CAUSED + tricks removed:** bisected — wide is CLEAN at SS=2, BARS at SS=1.
  The vertical bars are a wide-FB rasterization gap at native 1x (the 320→428 +54-shifted geometry leaves
  periodic 1px column gaps that SS=2 covers); NOT my OFX/DrawOTag change (bars persist with both reverted).
  My SS default 2→1 exposed it. Action: **reset the renderer (gpu_vk.c/gpu_native.c/shaders) to the clean
  pre-session state (e6de790), removing the user-rejected PGXP + lighting tricks entirely**; re-applied only
  SS=1 default + the scene classifier (RE tool). Native projection + DrawOTag overrides (game_tomba2) kept,
  re-verified **0-pixel-diff** vs recomp on the default path. Default ./run.sh = 4:3 native, clean, no tricks.
  OPEN: widescreen still needs the 1x wide-FB gap fixed (gap-free without SS) before re-enabling — the gap
  mechanism (1px columns at 1:1 raster of the +54-shifted geometry) is the next renderer target.
  (gte_beetle still defines PGXP_pushSXYZ2f — required by Beetle's gte.c — but its cache is now unused/dead.)

## later-101 — DECISION: consolidate to ONE execution substrate (interpreter-only runtime)
**Context:** chasing task #6 (full field native depth) I instrumented the interpreter's GTE-op path
(prologue back-scan attribution of *interpreted* RTPT/RTPS — reliable, unlike the prior session's
"windowed jal-decode" RTPCALLER histogram, which was a red herring). Ground truth: the field's
missing-depth world polys come from **interpreted OVERLAY submitters**, dominantly:
- `0x8013FB88` GT3 (RTPT=187838 over 600f, ~313/f) — SAME record layout + algorithm as the resident
  GT3 library `submit_poly_gt3`, only the color mask differs (0x00F0F0F0 vs 0xFFF0F0F0 — padding byte,
  VRAM-identical).
- `0x8013FE58` GT4 (RTPT=258542 RTPS=164519, ~431/f) — the GT4 sibling.
- `0x801464C0`/`0x8013DD34`/`0x80109C80`… smaller.

**The bug that motivated the pivot:** the scan-on-load DOES register native overrides for `0x8013FB88`
/`0x8013FE58` (`[submit] own overlay GT3/GT4 @ …`), but they **never fire** — the body runs
interpreted anyway (proven: the GTE probe counts them). Root cause = the recomp/interp SPLIT: two
override tables (`g_override[]` by recomp-fn-INDEX for resident MAIN, `g_iov[]` by raw ADDRESS for
overlays) and two interpreters (`rec_interp` flat + `rec_coro_run` coroutine). The override only fires
on call paths that consult the right table; the overlay submitter is entered via a path that doesn't.
Perverse allocation: the SLOW interpreter runs the HOTTEST code (the render submit firehose, which lives
in overlays), while the recompiler runs resident bookkeeping.

**Decision (user):** consolidate to a SINGLE substrate = **interpreter-only runtime**. Drop the
recompiler from the runtime. Rationale: overlays are first-class in Tomba2 (the render path itself is
overlay code) and the interpreter handles them for free; recompiler-only would mean *building*
N64Recomp-style overlay relocation/dispatch. Interpreter-only deletes the most complexity (emit.py from
the build, generated/, shard build, rec_dispatch, the index-vs-address override duality — the very seam
that caused this bug). The real oracle stays Beetle (`wide60rt`). **Keep emit.py + generated C as a
SEPARATE OFFLINE analysis tool** (Ghidra-like pseudo-C; it's exactly how the byte-packed variants were
RE'd this session) — just not in the runtime.

**Speed de-risked (the one real risk):** measured ~140.5M interpreted instructions / 600 field frames
(~234K interp-inst/f, the overlay render firehose) at ~100–150M inst/sec. A worst-case FULLY-interpreted
frame (~400–560K PSX inst total) ≈ 3–5ms — well inside the 16.7ms/60fps budget, and the firehose that
dominates today's interp load is precisely what becomes native C. MAIN.EXE's raw bytes are ALREADY in
g_ram (`boot.c load_exe` memcpy's text), so the interpreter can run MAIN directly — feasible.

**Bonus:** interpreter-only is the proper root-cause fix for task #6 — with one address-keyed override
table the scan-registered overlay GT3/GT4 overrides will actually fire and record depth, so the
252+136/f misses should largely vanish for free.

**Verified milestone kept (substrate-independent):** native byte-packed POLY_GT4 submit (`0x80027768`,
`engine_submit.c ov_submit_poly_gt4_bp`), **0 u16 VRAM diff** vs `PSXPORT_SUBMIT_RECOMP=1` at field f560.
ABI/record fully RE'd (a1=CLUT-Y<<22, a2=OT-Z bias s16, a3=U offset; no count, loop while ctl>0; OT base
*0x800ED8C8; DPCT/DPCS depth-cue colors). Note: barely used in the field (~0.8 prim/f) — the field GT4s
are the overlay variant above.

**Migration scope (counted):** 23 `gen_func_` super-calls in runtime+engine → an interp super-call;
14 `func_8…` direct refs; 49 override registrations (already address-based). Plan: unify override tables
(one address-keyed), hand-written `rec_dispatch` = override-or-interp-from-RAM, `rec_super_call(c,addr)` =
interpret original bytes, stop linking generated/shard_*.c, route MAIN entry through interp, then unify
the two interpreters. Verify boot→field→runs + frame rate + depth coverage jump.

## later-102 — DONE: interpreter-only runtime LANDED + full field depth coverage (task #6)
Executed the later-101 decision. The runtime no longer links the recompiler: MAIN.EXE and the boot
stub both run from g_ram via the interpreter. New `runtime/recomp/dispatch.c` replaces the generated
dispatch infra (rec_dispatch / rec_set_override / rec_func_index / stub_dispatch / stub_set_override +
`rec_super_call` = interpret the original body for A/B oracle). One address-keyed override table
(rec_set_override now routes to the same g_iov as rec_set_interp_override). build_port.sh + run.sh drop
generated/shard_*.c + stub_shard_*.c; emit.py is analysis-only (run.sh gates it behind PSXPORT_RECOMP=1).
~9 `gen_func_XXXX(c)` super-calls → `rec_super_call(c,0xADDR)`; MAIN entry `func_800896E0(c)` →
`rec_dispatch(c,0x800896E0)`.

**Verified:**
- Boots stub→MAIN→field, runs the native frame loop, clean exit. ZERO bad opcodes.
- **0 u16 VRAM diff** at field f560: interp-only MAIN == the old recompiled build (bit-identical), and
  native overlay-owned == fully-interpreted (PSXPORT_SUBMIT_RECOMP=1).
- **Speed: ~1.45s / 565 headless frames (~390 fps)** — same as the recompiled build; the speed risk is
  dead. (The earlier 5-min "spin" was the BIOS bug below, not slowness.)

**Three real bugs fixed on the way (all latent seam bugs of the old split):**
1. `rec_dispatch` must route BIOS vectors (`li $t2,0xA0; jr $t2`) to `rec_dispatch_miss` (HLE), NOT
   interpret code at 0xA0. (The old generated rec_dispatch did this via its `default:`.) This was the
   boot crash into string data.
2. `rec_interp` only honored overrides on `jal`/`jalr`, NOT on plain `j` / computed `jr` TAIL-CALLS —
   so a tail-called native override (submitters are tail-called) was bypassed and the original body
   interpreted. Now both interpreters check `coro_native_call` on every transfer.
3. `rec_overlay_loaded` flushed ALL auto (scan) overrides on EVERY overlay load — so a later data/asset
   overlay silently wiped the GAME code-overlay's submitter overrides, leaving the field's dominant
   submitters un-owned. THIS is why the prior session saw "owning overlays changed depth by ZERO."
   Fixed: `iov_flush_auto_range(base,size)` drops only overrides INSIDE the just-loaded region.

**Result — task #6 SOLVED:** with the overlay submitters (`0x8013FB88` GT3, `0x8013FE58` GT4, …) now
actually firing as native with depth, field depth coverage jumped **records made 634→1807, misses
428→34** (~30% → ~98% real per-vertex depth; the residual 34 are genuine 2D UI). Faithful 0-diff holds.

NEXT: (a) optional — unify the two interpreter loops (rec_interp recursive + rec_coro_run flat) into
one (they share exec_simple; this is the remaining "two things"); (b) build the SBS depth pic for the
user to judge visuals; (c) overlay-banner depth semantics (design call). Recompiler now lives only as
the offline Ghidra-like analysis aid (generated/, PSXPORT_RECOMP=1 to regen).

## later-103 — DONE: ONE interpreter loop (flat) + run.sh fix
Executed handoff task #1 (the user's core remaining ask: "one interpreter"). `interp.c` had TWO
control-flow loops sharing `exec_simple`: `rec_interp` (RECURSIVE — mirrors PSX calls on the C stack,
`jr ra` = C return) for synchronous nested calls / `rec_super_call`, and `rec_coro_run` (FLAT/resumable
— keeps the PSX stack in g_ram, exits at CORO_SENTINEL) for cooperative tasks. Collapsed them into a
single flat core `interp_flat(c, pc, stop_ra)`; the two public entry points are now thin wrappers that
differ only in the return sentinel:
- `rec_coro_run(c,pc)` = `interp_flat(c, pc, CORO_SENTINEL)` (task; scheduler enters its top fn with
  ra=CORO_SENTINEL).
- `rec_interp(c,pc)` (synchronous): save ra, set ra=CORO_SENTINEL, `interp_flat(c,pc,CORO_SENTINEL)`,
  restore ra. The target's own prologue/epilogue saves+restores whatever ra holds, so the net effect
  matches the old recursive rec_interp exactly; nesting is safe (each invocation's PSX frames sit above
  the caller's, so only the target's own `jr ra` hits the sentinel). Removed `call_addr`, `is_recompiled`,
  `override_for` (the index-vs-address override duality is fully gone — coro_native_call does ONE
  address-keyed lookup). `trace_call` (PSXPORT_INTERP_TRACE) re-wired into interp_flat's jal/jalr sites.

**Two real bugs found+fixed turning the recursive loop flat (both latent gaps the C-stack model hid):**
1. **crt0 terminal halt.** crt0 (0x800896E0) is `…; jal main(0x80050B08); break 0x1`. On real PSX main
   never returns; our native main (ov_game_main) returns after N headless frames, so control reaches the
   `break` at 0x80089784, falls into FUN_80089788 which saves/restores ra=0x80089784 and `jr ra` LOOPS
   back to the break → 27M-line `[break] code 1` spin. The old recursive rec_interp escaped this by chance
   (returned to C on the next `jr ra`). Root-cause fix (NOT an address special-case): a MIPS `break` is a
   program trap and we HLE the BIOS — there is no handler to resume into, so `break` ENDS the run. Safe:
   the field run executes exactly ONE break (this terminal), never on a hot path.
2. **tail-call into an override at the sentinel level** (a top-level body that `j`/computed-`jr`s into an
   override sets `pc = c->r[31]` = sentinel): added a top-of-loop `if (pc == stop_ra) return;` so reaching
   the sentinel by ANY path ends the run (sentinel 0xDEAD0000 is poison, never real code → no-op for
   normal flow).

**Verified (unified build):** boots stub→MAIN→field, clean exit, ZERO bad opcodes, ONE break (terminal),
clean shutdown ("returned from crt0" → "native_stub_run returned"). Depth coverage IDENTICAL 1807/34.
**0 u16 VRAM diff** at field f540 on THREE axes: (a) unified-flat == committed-recursive build (proves
the refactor changed nothing), (b) native-submit == PSXPORT_SUBMIT_RECOMP=1 fully-interpreted (faithful
gate holds on the unified interp). Same ~390 headless fps.

**Also fixed run.sh (was outdated/broken):** `[ -n "$PSXPORT_RECOMP" ]` under `set -eu` aborted EVERY
normal run with "unbound variable" (the var is unset unless you ask for the analysis recompile) → now
`${PSXPORT_RECOMP:-}`. Dropped the dead `-Igenerated` include (no linked TU includes generated/* since
the interpreter-only pivot) from run.sh + build_port.sh, and refreshed the stale "recompiler input" /
"compiles the recompiled core" comments. run.sh now builds+launches+exits cleanly end-to-end.

NEXT (handoff remainder): #2 SBS visual verify (PSXPORT_SBS shotseq not writing — debug gpu_vk.c
gpu_vk_shotseq), #3 overlay-banner depth semantics (ASK user), #4 optional generated/ include trim
(done for the build scripts; rec_decls.h is no longer included by any linked TU).

## later-104 — DONE: native-depth occlusion fixed (3-band depth model) + SBS visual verify
Handoff #2 (visual verify) + #3 (depth semantics, user steer: "more ownership"). The SBS A/B dump
(now working — fixed a silent `fopen` failure in gpu_vk.c gpu_vk_dump: it never `mkdir`'d the
PSXPORT_VK_SHOTSEQ dir) revealed that **task #6's "98% native-depth coverage" did NOT yield correct
occlusion**: with PSXPORT_NATIVE_DEPTH=1 the **entire foreground (terrain/hut/trees/Tomba) was occluded
by the water+sky background** — only the GTE-projected 3D objects (fruit, bird) survived. Reproduced in
the real single-channel NATIVE_DEPTH (not just an SBS artifact).

**Root cause:** the native-depth D32 buffer had only TWO bands — 3D world [0, 0.9375] (real per-vertex
depth) and a 2D OVERLAY band (0.9375, 1] for HUD. But Tomba2's **water and sky are screen-space 2D
layers with NO GTE projection** (engine_re.md "Water/reflection"), so every backdrop prim was `is3d=0`
and got dumped into the NEAR overlay band (nearest, wins) → it covered the whole 3D world. The overlay
band is right for HUD (composite OVER the world) but wrong for backdrops (must sit BEHIND it).

**Fix — 3-band depth model** (gpu_vk.c + gpu_native.c tee): split the non-3D prims into
- **2D BACKGROUND band [0, NATIVE_3D_MIN=0.0625)** — backdrops (water/sky), FAR, behind the world,
- **3D WORLD band [0.0625, 0.9375]** — real per-vertex depth (proj_pz_to_ord remapped via `ord3d()`),
- **2D OVERLAY band (0.9375, 1]** — HUD/UI/banners, NEAR, over the world (unchanged).

Background vs HUD is split by **OT submission order**: a 2D prim drawn BEFORE any 3D prim this frame is
a backdrop (`gpu_vk_set_order_2d_bg{,_n}` → far band); AFTER, it's HUD (overlay band). Tracked by a
per-frame `s_seen3d` flag (reset with s_prim_order). This matches the painter/OT semantics (backdrops
are submitted first, HUD last) and is what "owning more" of the depth means — the backdrop now gets a
deliberate native far depth instead of an accidental near one.

**Verified:** PSXPORT_NATIVE_DEPTH=1 now renders the full field correctly (water behind terrain, HUD
banner "Go to the Burning House!" on top); SBS A/B panels match across field frames 500/550/600. The
only intended difference is true-3D depth on objects (floating fruit now correctly occludes/shows vs
foliage by real Z, not OT order). **Faithful gate intact: 0 u16 VRAM diff** at f540 (the depth bands
only touch the VK render path, never the software rasterizer / s_vram).

Residual / next: the OT-order backdrop-vs-HUD split is a heuristic (a backdrop drawn AFTER a 3D prim, or
a HUD drawn before one, would be misbanded) — fine for the static field; revisit if a scene interleaves
them. Foreground objects vs tree-canopy foliage now occlude by true Z (a look change from the OT
original) — the banner-style "OT-on-top intent" question (handoff #3) is now concrete and may want a
per-layer call.

## later-105 — 60fps + widescreen SEPARATED (rename wide60 → fps60) + feature readiness
User direction: finish 60fps, but first SEPARATE the widescreen and 60fps code (they were conflated
under the "wide60" name even though widescreen lives in gpu_vk.c PSXPORT_WIDE/IRES and 60fps lives in
engine/wide60.c). The two were already decoupled in code (neither references the other); the conflation
was purely the name. Renamed the PORT's 60fps feature wide60 → fps60: `engine/wide60.c`→`engine/fps60.c`,
`wide60_*`→`fps60_*`, `gpu_w60_*`→`gpu_fps60_*`, `g_wide60_on`→`g_fps60_on`, `PSXPORT_WIDE60[_GATE/_SYNTH/
_SDBG]`→`PSXPORT_FPS60_*`, debug channel `wide60`→`fps60`. LEFT ALONE: the Beetle ORACLE's own
`runtime/wide60.{cpp,h}` + the `wide60rt` binary (separate reference emulator), and provenance comments
in hle.c/native_boot.c that cite the oracle's HLE. Verified pure-rename: full build OK, **0 u16 VRAM
diff** at f540 (faithful gate), 60fps measures identically under PSXPORT_FPS60.

Feature readiness audit (asked "ready for hi-res/widescreen/60fps/lighting/AO mods?"):
- **Higher internal resolution** (PSXPORT_IRES=N, gpu_vk.c) — WORKS (verified: genuine denser
  rasterization, crisp 3D edges; up to 3x 4:3 / 2x 16:9, VRAM_W-capped).
- **Widescreen** (PSXPORT_WIDE) — WORKS (verified: true wider FOV, more world on the sides, no stretch;
  HUD edge-anchoring). Native depth makes both render correctly now.
- **60fps** (PSXPORT_FPS60) — full system built (capture/match/synth/frame-behind present) but **0%
  interpolating**: measured live `rtp_with_obj=0 tagged=0`, all 927 field prims SNAP → presents the real
  frame = 30fps. Blocker: the field's render-time RTPS has no object context. Unblock paths: (a) own the
  field per-object render dispatch to tag draws, or (b) switch the synth to the already-built GTE-
  transform SXY remap (xobj/wide60_build_remap, bypasses the tag) — note build_remap/xobj_match are NOT
  currently called in fps60_frame_commit (dormant). Needs a MOTION scene to validate (idle field has
  nothing to interpolate).
- **Better lighting** — was built (9d81ff8) then REMOVED (8e959e9 "remove rejected renderer tricks");
  current tritex.frag has no lighting/normals/fog. Needs fresh approach.
- **Ambient occlusion** — not started; UNBLOCKED by the new 3-band depth (D32 s_depth attachment exists);
  needs a new SSAO pass + normals (reconstructable from depth) + composite into the 1555 uint VRAM.

## later-106 — 60fps foundation VALIDATED: GTE-transform object matching is 100% on the field
Activated the dormant GTE-transform object matcher in fps60_frame_commit (xobj_match + xobj_report +
xobj_commit — were defined but never called, so s_xA stayed empty). This is the interpolation path that
does NOT need the per-poly object tag (the ocen/Prim.obj path is blocked at rtp_with_obj=0). Each run of
RTPS/RTPT sharing one GTE transform (CR0-7 = model-view, camera baked in) = one object; cross-frame
identity = its local-vertex fingerprint.

**Measured (idle field, PSXPORT_FPS60=1 PSXPORT_DEBUG=fps60):** objects=126/frame, **matched=126
(100.0%)** every frame, TRdelta avg=369 max=3220 (GTE fixed units) — i.e. there IS real per-object motion
even "idle" (ambient/water/idle-anim) for interpolation to smooth. So the object+camera identity
foundation the user asked for ("interpolate game objects and camera between frames") is SOLID and proven
on real gameplay. Cheap (126×126 ≈ 16k cmp/frame; only when PSXPORT_FPS60=1, default off → faithful path
untouched).

**Remaining to finish 60fps = the SYNTHESIS.** Two prior synth attempts: (a) ocen per-object 2D centroid
translation — blocked (Prim.obj=0, all snap); (b) build_remap SXY reprojection re-rasterized into the
separate s_interp buffer — looked "terrible/smeared" (later-86) because re-rasterizing the captured 2D
GP0 SUBSET is lossy (missing occluders/fills → hidden geo reappears, no depth). The RIGHT approach now
that we own the native renderer + 3-band native depth: for the in-between frame, interpolate each matched
object's transform and RE-SUBMIT the display list through the NATIVE VK renderer (gpu_vk_draw_* with the
D32 depth), so occlusion is correct — no lossy s_interp re-rasterize. Present frame-behind: A, lerp(A,B),
B… NEXT: wire build_remap's per-vertex SXY remap into a native-renderer re-submit + a motion-scene visual
check (idle field has motion but driving Tomba/camera pan shows it best).

## later-107 — 60fps: restored the SEPARATE-LAYER synth (user architecture: layer on top, NO resubmit)
User correction: "interp60 should be a separate layer that lives on top. no resubmit." (Matches
[[wide60-60fps-architecture]] + the later-83 design — I had wrongly proposed re-submitting through the
native renderer.) The current code had REGRESSED from the proven later-83 build_remap path to the
ocen per-object centroid path, which is blocked (Prim.obj=0 → all snap → 30fps). Restored the correct
design in fps60_synthesize:
- `fps60_build_remap()` now runs in frame_commit (after xobj_match, before xobj_commit): interpolate
  each 100%-matched object's GTE transform to the midpoint, reproject its verts through the REAL Beetle
  GTE → an old-SXY → interp-SXY table.
- synthesize() ALL-OR-NOTHING per prim (later-83): a poly remaps only if EVERY vertex resolves in the
  table (whole prim = one interpolated object); sprites/lines + partial/2D prims SNAP. Re-rasterizes into
  the SEPARATE s_interp buffer ON TOP (VRAM untouched) — not a resubmit. Removed the dead ocen machinery.

**Result:** interpolation ACTIVATES — field synth f600 = 927 prims, **528 remapped / 399 snapped** (was
0/927). Object match 100%, gated by PSXPORT_FPS60 (faithful path 0 u16 VRAM diff at f540, verified).

**Two open problems found (the real remaining work):**
1. **Re-raster fidelity.** On a STATIC field frame (A==B, mean|A−B|=0) the synthesized in-between still
   differs from the real frame by **mean ~8.24/255 (~3%)** — the separate-layer re-rasterizer does NOT
   faithfully reproduce the frame (missing non-captured GP0 ops: background fills, VRAM-copy/water
   reflection, semi-transparency order). So with motion OR not, the in-between shimmers vs the real
   frames. THIS is the blocker for the separate-layer approach: the re-raster must reproduce the full
   frame (capture+replay ALL draw ops, correct blend/occlusion), or the present must show the in-between
   only on real motion. NEXT.
2. **Headless motion validation is hard.** The idle field is fully static (A==B) and the new
   `PSXPORT_AUTO_WALK=l/r/u/d` (native_boot.c — holds a D-pad dir after field-reached, a deterministic
   motion scene) did NOT visibly move the character (TRdelta a constant 369/frame = ambient anim only;
   A==B persisted). Need to confirm the held input reaches the game / the char can walk here, or pick a
   scene with real camera pan, to validate interpolation visually. Live eyeball (`PSXPORT_FPS60=1
   ./run.sh`) remains the documented check.

## later-108 — STATUS: 60fps PARKED (waiting for user live-test); other mods handed to a fresh session
60fps (PSXPORT_FPS60) is implemented (later-106/107) — separate layer, camera+3D-object interpolation,
100% object match, 528/927 field prims reproject. **Parked waiting for the user to live-test**
(`PSXPORT_FPS60=1 ./run.sh`); headless validation is a dead end (synth dumptest reads byte-identical VRAM
buffers in the field — see later-107). Do NOT keep iterating fps60 until the user reports.
Fresh session continues the OTHER mods (handoff: scratch/handoff.md): (1) ambient occlusion / SSAO —
unblocked by the 3-band native depth (D32 s_depth exists); (2) better lighting — rebuild fresh (the
9d81ff8 lighting was removed in 8e959e9; read that first). Hi-res + widescreen already DONE (later-105).

## later-109 — DONE: PC-native SSAO (PSXPORT_SSAO), curvature model, between opaque & semi
Task #1 from the handoff. Built a screen-space ambient-occlusion post pass over the VK renderer, gated
`PSXPORT_SSAO` (implies NATIVE_DEPTH; OR'd into the native-depth gates in gpu_native.c + gte_beetle
attach_enabled; disabled under SBS). New: `shaders_vk/ssao.frag` (+present.vert reused as its vertex
stage), gpu_vk.c `create_ssao`/`ssao_pass` (own R16_UINT target s_ssao_img, color-only rpass ending in
TRANSFER_SRC, 2-binding descriptor color+depth, pipeline), s_depth gets SAMPLED_BIT, gte_beetle
`proj_near_pz()` getter. AO'd color → s_ssao_img → copied back into s_tex (present/dump pick it up free).
Depth linearize: stored depth = ord3d(proj_pz_to_ord(pz)), affine in 1/pz → undo band remap + affine
to view-Z; only the 3D band [MIN,MAX] is touched (sky/backdrop/HUD pass through).

**Two real findings (not tuning), each fixed at the root:**
1. **Naive "is the neighbour closer" AO washed the whole TILTED ground** (21% of pixels darkened, a
   uniform smear on the grass). Root cause: a tilted plane always has a "closer" downhill neighbour →
   false occlusion. Fix = **curvature AO via opposite-neighbour pairs**: a flat/tilted plane has
   center == average(opposite neighbours) → 0 AO; only genuine concavities darken. Dropped to 2.7%,
   concentrated on creases/foliage/contacts.
2. **AO darkened the 2D menu UI.** Root cause: the menu's blue fill is SEMI-transparent → writes no
   depth → the depth buffer under it is the 3D terrain, so a post-everything AO pass saw "3D" there.
   Fix = run SSAO **between the opaque and semi passes** (AO belongs to opaque geometry; translucent
   UI/water composites OVER it). The menu now correctly shows the AO'd terrain *through* its glass and
   is not itself darkened.

**Verification (single deterministic run — cross-run pixel A/B is unreliable; AUTO_GAMEPLAY scene
state drifts between processes, the same headless dead-end as fps60).** Added `PSXPORT_SSAO_VIZ=1`
(AO factor as grayscale on 3D pixels, original color on 2D/sky) → confirmed on the field: UI menu +
sky EXCLUDED (shown in color), 3D world AO-eligible, flat ground NOT washed, AO lands on hut/foliage/
object creases + contacts. Vulkan validation layers: ZERO errors from the new rpass/barriers/depth-
sample/copy (only the pre-existing headless present-rpass PRESENT_SRC_KHR VUID remains). Faithful gate:
SSAO only touches the VK path, never s_vram; default off → SW/oracle path byte-identical.
Tunables (env, logged once): SSAO_STRENGTH=1.0, _RADIUS=5px×IRES, _BIAS=0.01, _RANGE=0.15. Next: user
live-tune the look; then task #2 (better lighting, rebuild fresh).

## later-110 — DONE: PC-native directional lighting (PSXPORT_LIGHT), deferred normals from depth
Task #2. User steer on the prior (rejected) lighting: "I don't remember but if it was rejected either
the agent did it or it wasn't PC native like it was hacky." So: do it the PROPER PC-native way, not the
old PGXP per-face-normal hack (value-keyed cache, entangled in the forward tritex pass). Built a
DEFERRED directional light that **reconstructs real geometric normals from the depth buffer** —
shares the SSAO deferred pass (ssao.frag + gpu_vk.c ssao_pass, now AO+light), gated PSXPORT_LIGHT
(implies NATIVE_DEPTH via the same gates; disabled under SBS). No GTE/PGXP coupling.

How: per depth pixel reconstruct view pos P = ((sx-cx)*pz/H, (sy-cy)*pz/H, pz) — cx,cy,H from new
gte_beetle getters proj_screen_center()/proj_plane_h() (H = CR26, set each frame by engine_submit
proj_set_H; center = OFX/OFY, standard 160/120). VRAM→screen map handles faithful AND wide/hi-res-FB
(inverse of tritex.vert relocation: origin/inv_scale/wide_off). Normal = normalize(cross(dPdx,dPdy))
with a closer-neighbour pick (don't bleed a normal across a silhouette), oriented to face the camera
(view -Z). Shade = ambient + diffuse·max(0,N·L), applied to the baked color as albedo. Light dir =
to-light vector in view space.

Verified (single deterministic run; cross-run pixel A/B unreliable as for SSAO): PSXPORT_SSAO_VIZ=2
shows the reconstructed normals = COHERENT per-surface (flat ground uniform, hut faces distinct, sky/UI
excluded) — proves H/cx/cy correct (a wrong H would collapse all normals to camera-facing). VIZ=3 = lit
factor. Lit screenshots: terrain/foliage gain real directional FORM, conservative by default (amb .65 /
diff .5 → subtle, doesn't crush baked art); composes with SSAO; UI/sky untouched. Vulkan validation
ZERO new VUIDs (only the pre-existing headless present-rpass PRESENT_SRC). Faithful gate: only VK path,
default off. Tunables: PSXPORT_LIGHT_DIR="x,y,z"/_AMBIENT/_DIFFUSE. Both PC-native mods (SSAO+LIGHT)
now done; user to live-tune the look. Hi-res/widescreen/SSAO/lighting all landed; 60fps still parked.

## later-111 — DONE: Dear ImGui mod-toggle overlay (PSXPORT_UI) + live mod state
User: "add imgui and a toggle for everything (wide/60/lighting/ao) then we'll RE the game's own options
menu and move them there." Built the interim overlay; the game-options-menu RE is the NEXT step (move the
toggles there later — this overlay is the stopgap UI).

- **Vendored Dear ImGui** stable v1.91.9b into `vendor/imgui/` (core + SDL2 + Vulkan backends; MIT).
- **Live mod state** `runtime/recomp/mods.{h,c}` — `g_mods` (ui/wide/ires/ssao/light + ssao & light
  params), seeded once from cfg by mods_init(), then mutated LIVE by the overlay. gpu_vk.c now reads
  g_mods EVERY frame (s_wide/s_ires became accessor macros over g_mods; ssao_on/light_on read g_mods;
  ssao_pass params read g_mods live) so a toggle/slider takes effect immediately. 60fps = the existing
  extern int g_fps60_on, flipped directly by the overlay.
- **Overlay** `runtime/recomp/imgui_overlay.{h,cpp}` (C++; a C bridge header). Inits ImGui on the port's
  EXISTING VK device + present render pass (no second device), draws into the swapchain inside the present
  render pass (after the present quad, before EndRenderPass). Toggle visibility with ` or F1. Checkboxes:
  Widescreen, 60fps, SSAO, Directional light; sliders: Internal res (1-3, capped 2 in wide), and the SSAO
  / light params (shown when their toggle is on).
- **Runtime-toggleable SSAO/LIGHT:** PSXPORT_UI forces the native-depth path + deferred-resource creation
  ON (OR'd into the native-depth gates in gpu_native.c/gte_beetle.c, and create_ssao via ui_infra()), so
  SSAO/LIGHT can be flipped on at runtime even if they started off. Disabled under SBS.
- **Build:** the port is otherwise pure C; added C++ support to BOTH run.sh and tools/build_port.sh
  (compile .cpp with $CXX, link with $CXX for libstdc++; -Ivendor/imgui[/backends]). New TUs: mods.c +
  imgui_overlay.cpp + the 6 vendored imgui .cpp.

Verified: builds+links clean; windowed `PSXPORT_UI=1 PSXPORT_GPU_WINDOW=1` brings the overlay up
("[imgui] overlay up"), runs 30s+ with NO Vulkan validation errors / crash, all toggles render (cropped
screenshot confirms). Game render path with UI on is fine (headless UI=1 field frames bright). Faithful:
default off. NOTE the default ImGui font has no em-dash glyph (use ASCII in labels). Run windowed:
`PSXPORT_UI=1 PSXPORT_GPU_WINDOW=1 ./run.sh` (or add PSXPORT_WINDOWED=1).

## later-112 — DONE: replace the game's in-game Options menu with our PC-native (ImGui) menu
User: "The game has an options menu but it doesn't have any options worth keeping. We can replace it
with a much richer menu." → RE'd the in-game pause/Options menu and hooked it to show our overlay.

- **RE (full state machine in `docs/engine_re.md` "In-game pause / Options menu"):** the pause menu is a
  **task in the GAME overlay**; body/dispatcher `0x8010810C` indexes a 12-entry table at `0x801062EC` by
  the page byte `task+0x6B` (task = `*(u32)0x1F800138`). Page 1 = main menu "Options / Load data / Quit
  game" (`FUN_8007eae4`); Cross over "Options" sets page→3. Page 3 (`0x801082C0`) calls **`FUN_8007b45c`**
  = the Options submenu (Messages / Sound / Screen adjust / Controls = `FUN_8007f104` — the options the
  user discarded). Disassembled `FUN_8007b45c` via `tools/recomp/decode.py` (it's outside the decomp
  dump): Triangle→page 2 (close), Circle→page 1 (back), SFX `FUN_80074590`.
- **Hook (`engine/game_tomba2.c` `ov_options_menu`, gated `PSXPORT_UI`):** `rec_set_override(0x8007B45C,…)`
  — while page 3 runs, force our overlay visible (options-mode) instead of drawing the game's options, and
  own the same back-nav: **Circle** → `task+0x6B=1` + cursor reset + SFX `(0x14,0xFFF7)`; **Triangle** →
  `task+0x6B=2` + SFX `(0x11,0)`. **Faithful fallback:** if the overlay isn't inited (headless/window-less)
  it super-calls the real `FUN_8007b45c` so nothing is lost. Added `imgui_overlay_set_visible/_options_mode`
  to the overlay (suppresses `~`/F1 toggle in options-mode; shows "Circle: back  Triangle: close" hint).
- **Verified:** hook is reached — headless `PSXPORT_AUTO_GAMEPLAY=1 PSXPORT_UI=1 PSXPORT_DEBUG=ui` with a
  forced Cross at the auto-appearing pause menu (~f720) logs `[ui] FUN_8007b45c reached`. (This also
  CORRECTS docs/driving-the-game §5: the auto-appearing menu DOES respond to forced Cross — it selects
  "Options".) **Windowed verified:** `PSXPORT_UI=1 PSXPORT_GPU_WINDOW=1 PSXPORT_AUTO_GAMEPLAY=1` +
  forced Cross → the overlay path runs (`[ui] in-game Options -> PC-native overlay`) and the cropped
  screenshot (`scratch/screenshots/options_overlay_crop.png`) shows OUR menu (title + "Circle: back
  Triangle: close" hint + the mod toggles) standing in for the game's options screen. SDL window opens
  top-left (0,0), 960x720 — NOT centered (earlier centered-crop assumption was wrong; the overlay is at
  ~(20,20)).

## later-113 — overlay ON BY DEFAULT (windowed) + aspect modes {4:3,16:9,21:9,Auto} + auto internal-res
User (frustrated): "you keep gating everything behind flags; I type ./run.sh and press F1 but no imgui
shows." Root cause: the overlay init + native-depth/deferred infra were gated on `PSXPORT_UI`, which
`./run.sh` never sets. Fix: **the overlay + its live-toggle infra are now ON BY DEFAULT for any WINDOWED
VK run** — plain `./run.sh` brings the menu up and F1 toggles it; no flag. `PSXPORT_UI=0` opts out.
- Decision lives in `mods_init()` (mods.c): `g_mods.ui = 1` when windowed (`PSXPORT_GPU_WINDOW` && !headless
  && !SW && VK!=0), else 0; `PSXPORT_UI=1/0` forces. The native-depth gates (gpu_native.c ×3 lazy +
  gte_beetle `attach_enabled`) now read **`g_mods.ui`** (not `cfg_on("PSXPORT_UI")`) and call the idempotent
  `mods_init()` first so the value is set before the first GP0/GTE caches it (init-order safe). **Headless
  stays faithful** (ui=0 → no native-depth) so the VK render-diff tooling is unchanged. Options-menu
  override (0x8007B45C) is now always registered (its super-call fallback handles the no-overlay case).
- **Aspect selector** replacing the wide checkbox: `g_mods.aspect` ∈ {4:3,16:9,21:9,Auto}. 4:3=320,
  16:9=428, 21:9=560 native FB width; **Auto = the live window aspect** (`240*win_w/win_h`, even, ≤VRAM_W).
  Present letterboxes to the selected aspect; Auto fills the window. `wide_native_w()`/`WIDE_OFF()` are now
  dynamic (16:9 stays byte-identical: 428, off 54 — no regression to the known-good path).
- **Auto internal-res** `g_mods.ires_auto`: ires ≈ round(window_h/240), clamped so `native_w*ires ≤ 1024`
  (VRAM_W) and ≤3. Overlay shows the computed `(Nx)` + a "Render WxH | window WxH" status line.
- Env: `PSXPORT_ASPECT=4:3|16:9|21:9|auto` (legacy `PSXPORT_WIDE=1`→16:9); `PSXPORT_IRES=N|auto`.
- Verified: plain windowed run (only `PSXPORT_GPU_WINDOW=1`, NO `PSXPORT_UI`) → `[imgui] overlay up` +
  screenshot `scratch/screenshots/default_ui.png` (Aspect 4:3, Render 320x240 | window 960x720). 21:9 +
  auto-ires verified (Render 560x240, ires auto-capped 1x; `scratch/screenshots/aspect_menu.png`).
  KNOWN: pushing the FOV to 21:9 on the seaside scene shows a garbled band where the game submits no
  geometry for the extra-wide sides (a content/FOV limit of ultra-wide, NOT the 16:9 path) — needs a look.

## later-114 — REGRESSION fix (native-depth default) + engine-ownership audit (widescreen is a hack)
User: "the game becomes completely corrupted [widescreen] — most of the engine is still a black box; port
the render/camera layer." Two things:
- **Regression I caused, now fixed (`796740b`):** later-113 routed the native-depth/deferred path through
  `g_mods.ui` and defaulted it ON for windowed → every plain `./run.sh` ran the incomplete native-depth
  model over the whole game = corruption. **A menu being available must never change rendering correctness.**
  Decoupled: overlay stays default-on; native-depth/deferred (gpu_native gates, gte_beetle attach_enabled,
  ui_infra) revert to opt-in via `PSXPORT_UI` env (off on plain `./run.sh`). SSAO/light greyed in the overlay
  unless `PSXPORT_UI=1`. Verified plain windowed run renders the field scene FAITHFULLY (HUD+char+env intact,
  `scratch/screenshots/faithful_scene.png`).
- **Honest correction:** my "21:9 = content limit" was an eyeballed guess (the project bans that). Real root
  cause: **widescreen is a renderer-side HACK** — we render the native 4:3 projection into a wider FB and
  spread verts in a shader, while the engine still CLIPS (PutDrawEnv `0x800815D0`) and CULLS
  (`0x8007712C`) to 4:3 and draws water/sky/HUD as screen-space 2D. So wide → empty/garbled sides. Fix = own
  the view at the SOURCE (OFX via `gen_func_800509B4`, the clip, the frustum, the 2D layers), which our
  already-owned `proj_native_vertex` (0-diff) + per-vertex depth then render correctly.
- **Deliverable (user picked "audit first"): `docs/engine-ownership-audit.md`** — full map of what's owned
  (DrawOTag, projection math+depth, resident submit, asset upload) vs the black box (PutDrawEnv/PutDispEnv,
  projection-config, frustum cull, 2D water/sky/HUD, overlay emitters, VRAM copies, camera basis), why
  widescreen corrupts, and a prioritized, oracle-gated port plan (diagnose → PutDrawEnv/Env → OFX config →
  frustum cull → 2D layers → retire the FB hack). Scope confirmed: gameplay logic STAYS interpreted (the
  Beetle emulator is the oracle). **Terminology correction (user caught it):** the runtime is
  INTERPRETER-ONLY (later-103) — un-owned code runs on the flat interpreter, NOT recompiled; the recompiler
  is an offline analysis aid. Fixed the audit + engine_re.md wording ("recomp MIPS" → "interpreter").

## later-115 — DIRECTIVE: full ownership of the engine layer, no faking (respawn-driven)
User: "the next step is full ownership of the game engine, no faking anything, respawn when you need to."
= execute the WHOLE `docs/engine-ownership-audit.md` plan to completion: own the entire render/camera/submit/
loop layer natively, RETIRE every renderer-side fake (the FB-widescreen hack `push_wide`/`WIDE_OFF`/aspect
scratch FB, the sprite-anchor FB hack), so widescreen/effects come from a genuinely wider engine frame.
Gameplay logic stays interpreted (oracle = Beetle). Handed off to a fresh session via `cci respawn` with
`scratch/handoff.md` to run the audit plan with full context budget. First task there: diagnose the wide
corruption with SCENEDUMP/PROVAT vs the oracle, then own PutDrawEnv/PutDispEnv (0-diff @4:3) → OFX config.

## later-116 — DIAGNOSIS (audit step 1): wide "corruption" is a pure present-time effect, engine output is aspect-invariant
Fresh respawn session (handoff.md). Executed audit step 1 — characterize the wide corruption with the
render tooling, NOT by eye (the prior "21:9 = content limit" eyeball was wrong). Method: drive headless to
the field (`PSXPORT_AUTO_GAMEPLAY=1`, field reached ~native-frame 328) and classify the submitted OT with
`PSXPORT_SCENEDUMP` at the SAME game-state across aspects. (Note: `s_frame` is the *present* counter and
runs ~7× the native-loop frame — at native-frame 420 s_frame≈3006; dumped at s_frame=2900 = field.)
- **Result — byte-identical OT at every aspect.** 4:3, 16:9, 21:9 all submit the same field display list:
  `poly=531 rect=355 line=0 fill=0 vramcopy=1 env=9` (plus the env/clear OT `fill=1 env=6`). The engine
  submits the EXACT same geometry regardless of `PSXPORT_ASPECT`.
- **Conclusion (confirms audit B1/B3/B4, empirically):** the engine produces NO wider content. OFX stays
  160, the clip stays 320, the frustum cull stays 4:3, and water/sky/HUD stay screen-space 320 — nothing in
  the aspect path touches the engine's submission. Today's "widescreen" is entirely a **present-time shader
  effect**: `gpu_vk.c` `push_wide` takes the fixed 320-wide projection and (per `tritex.vert` wide branch)
  CENTERS it into a wider scratch FB (`local.x + WIDE_OFF`, WIDE_OFF=(428-320)/2=54), then the present
  stretches that FB to the window. So 16:9 = 4:3 content stretched, not a wider FOV. That IS the "fake".
- **Levers verified for the genuine fix:** (1) Beetle's GTE does the real RTPS/RTPT projection using CR24
  (OFX) — `gte_op` runs `GTE_Instruction`; `proj_native_vertex` is only a PROJPROBE verifier, NOT the live
  path. So setting CR24 wider (via the already-owned `ov_set_geom_offset`) genuinely widens the 3D FOV.
  (2) the VK tee passes `s_da_*` (the clip) as `i_da` per-prim, and `gpu_gp0(0xE4…)` sets `s_da` — so the
  clip can be widened engine-side and it propagates to the VK shader. (3) the VK scratch FB (`use_fb`) is
  independent of PSX VRAM's 320-wide buffer layout, so genuine-wide content (X∈[0,428]) can be rasterized
  there 1:1 without VRAM clobber — genuine-wide is inherently a VK feature (SW VRAM stays 4:3 = the oracle).
- **Next (audit step 2/3):** own the projection config (OFX lever) + the clip (PutDrawEnv) engine-side,
  default-OFF behind `PSXPORT_WIDE_ENGINE` so 4:3 stays byte-identical (0-diff gate), then the shader places
  wide content 1:1 (drop WIDE_OFF centering). 2D water/sky/HUD (B4) + frustum cull (B3) remain after.

## later-117 — GENUINE engine-level wide FOV (audit step 3): OFX lever, default-OFF, 4:3 byte-identical
Implemented the first "no faking" widescreen lever: the ENGINE projects a genuinely wider horizontal FOV
(via Beetle's GTE) instead of the present-time FB spread. Gated `PSXPORT_WIDE_ENGINE` (default OFF) so the
faithful 4:3 path is untouched (the 0-diff gate).
- **OFX widen** in the already-owned `ov_set_geom_offset` (engine/game_tomba2.c): the gameplay projection
  center is 160 (=320/2); when `gpu_vk_wide_engine()` is on, substitute the aspect center (214 @16:9 =
  428/2) so CR24 drives Beetle's RTPS/RTPT to project across the wider screen. Only the gameplay config
  (ofx==160) is widened; InitGeom's reset (ofx==0) is left alone. Verified: headless logs
  `[geom] WIDE_ENGINE OFX 160 -> 214` + `CR24=00D60000` (wide), and the default run keeps `OFX=160`
  `CR24=00A00000` with NO WIDE_ENGINE line — 4:3 path provably untouched.
- **1:1 placement** in `gpu_vk.c push_wide`: WIDE_OFF (the 320-in-428 re-center) is the SPREAD; in
  wide-engine the projection is already wide, so pass wide_off=0 → content placed 1:1 in the scratch FB.
  For the non-wide-engine path the expression is byte-identical (`s_wide ? WIDE_OFF() : 0`), so the
  existing FB-hack path is unchanged (no regression).
- **Accessors** in gpu_vk.c: `gpu_vk_wide_engine()` (PSXPORT_WIDE_ENGINE && aspect!=4:3),
  `gpu_vk_wide_engine_ofx()` (wide_native_w/2), `gpu_vk_wide_engine_w()` (wide clip width).
- **Verified (headless VK shot at the field, s_frame=2900, `scratch/screenshots/field_{43,wide169,fbhack169}.png`):**
  - 4:3 (320×224): faithful scene, correct.
  - genuine-wide 16:9 (428×240): the 3D world is genuinely WIDER — terrain+structure on the far left and
    more ocean on the right that are OFF-SCREEN in 4:3 are now visible, with correct (un-stretched)
    proportions. ASYMMETRIC because the screen-space 2D (the "Go to the Burning House!" banner / HUD) is
    still anchored at 320 → it's cut off on the right. That asymmetry is the proof it's a real FOV widen,
    not a stretch.
  - FB-hack 16:9 (428×240, the old default): SYMMETRIC, banner fully centered = the whole 4:3 frame
    uniformly stretched (the "fake").
- **Known remaining artifacts (the next audit steps, NOT regressions):** (1) vertical sky/backdrop stripes
  appear in BOTH 16:9 paths (pre-existing — the non-wide-engine code path is byte-identical to before my
  change, and it has them too) = the screen-space 2D sky/water layer (**B4**) not yet widened; (2) the HUD
  banner pinned/cut at 320 (**B4**); (3) side geometry that the 4:3 frustum culled is still absent on the
  extreme sides (**B3**). None block the 3D-FOV foundation.
- **NEXT:** B4 — own the 2D water/sky/HUD submit and widen/anchor it to the aspect (kill the stripes + the
  cut HUD); then B3 frustum cull; then own PutDrawEnv/PutDispEnv to widen the clip engine-side (step 2,
  currently the VK shader clips to the FB rect so it's not blocking); then retire the FB spread (step 6).
- **Corrects memory [[tomba2-mods-status]]:** "Widescreen DONE/works (true wider FOV, no stretch)" was
  FALSE — that was the FB-hack stretch. Genuine wide is in progress behind PSXPORT_WIDE_ENGINE.

## later-118 — PC-NATIVE per-pixel depth is now the DEFAULT (always-on), not opt-in (user directive)
User (emphatic): "PC game native depth should ALWAYS be active. Genuine wide can't be genuine without
native depth. Everything needs to be PC GAME." Acted on it.
- **Root-cause of the opt-in:** the prior session (796740b) reverted native-depth to opt-in
  (`PSXPORT_UI`/`NATIVE_DEPTH`) because defaulting it on "could corrupt not-yet-owned submit paths." That
  was a PRECAUTIONARY revert (a bandaid), not a pinned bug — exactly the "gate it off instead of fixing the
  cause" trap. Re-checked empirically: native depth renders the field, the boot/prologue cutscene, AND
  genuine-wide 16:9 CORRECTLY (`scratch/screenshots/nd_{def43,boot,wide169}.png`). Field ndepth stats:
  **depth records=1807, lookups hit=1807, miss=34** — the 34 misses are exactly the 2D sprites/HUD that
  bypass the GTE (correctly classified 2D); 3D-vertex attach coverage is effectively 100%. No corruption.
- **Change:** new `native_depth_on()` (gte_beetle.c) = the single gate, **default ON**. Opt OUT only for
  oracle A/B diffing: `PSXPORT_FAITHFUL_DEPTH=1` (or legacy `PSXPORT_NATIVE_DEPTH=0`). Replaced the
  scattered `cfg_on("NATIVE_DEPTH")||SSAO||LIGHT||UI` gates (gpu_native.c ×3, gte_beetle `attach_enabled`)
  with it. The deferred SHADING pass (SSAO/light, `deferred_on()`/`ui_infra()`) stays OPT-IN — only the
  per-pixel DEPTH model is now default. `s_seen3d` (backdrop-vs-HUD 2D classification) is therefore now
  maintained every run, which the genuine-wide 2D work needs.
- **Why it matters for wide:** the OT-order painter's algorithm assumes the authored 4:3 viewpoint; widen
  the FOV and that order mis-occludes. Real per-pixel depth (D32, per-vertex view-Z) is correct at any FOV.

## later-118b — genuine-wide 2D backdrop fills the frame (kills the vertical stripes)
The genuine-wide 16:9 (later-117) showed vertical sky/backdrop STRIPES. Root cause (confirmed, not
eyeballed): the screen-space 2D sprites (sky/water = 355 rects in the field) were spread apart by the
per-sprite `gpu_vk_sprite_anchor_dx` FB-hack — adjacent backdrop tiles get DIFFERENT anchor shifts →
gaps. Fix (gpu_native.c sprite path, gated `gpu_vk_wide_engine()`): in genuine-wide, scale the whole 2D
plane uniformly to the wide width about the framebuffer origin (`XL/XR = o + (X-o)*wide_w/320`) so a tiled
backdrop fills the frame contiguously (no gaps). Verified: `scratch/screenshots/field_wide169b.png` — sky
gradient + ocean now fill the 16:9 frame cleanly, stripes gone. STEPPING STONE: this also scales HUD size;
the proper split (backdrop scales to fill, HUD anchors at native size) uses the now-always-on `s_seen3d`
bg/HUD flag — next. The HUD banner cut at 320 also remains (a clip item, B4/step2).
