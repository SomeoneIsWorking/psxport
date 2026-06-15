# Debug / progress journal

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
