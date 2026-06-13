# Debug / progress journal

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
