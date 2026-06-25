# Issues #8 & #9 — effect-sprite "vertical bars" / striped-sampling family — RE + PC-native fix spec

READ-ONLY RE (no source modified). Methodology per `docs/gfx-debug.md`: the engine OWNS its render
PC-native; there is NO oracle. The SW rasterizer (`raster_sprite`/`sample_tex`) is the in-tree texel
REFERENCE that the VK fragment shader (`tritex.frag`) must match — and currently DOES, which is the
heart of why the prior "fix" did not close these bugs.

- **#8** "Walking dust clouds render as vertical bars" — the dust-puff effect Tomba kicks up while
  walking renders as a row of gray vertical bars/columns instead of soft puffs.
- **#9** "Striped-bar corruption on effect/impact scene" — a burst/impact effect (forest, bright
  white-blue radial) shows a blue vertical-striped bar (top-middle) + a gray striped bar (right-middle).

Both have the **same striped signature**: a textured effect prim whose interpolated U marches past the
right edge of its texpage cell and re-samples the wrong VRAM columns (and, in 4/8bpp, the wrong CLUT
indices) → vertical strips. They are **one root-cause family** (see §5).

---

## 0. Status: there is a PRIOR "fix" that did NOT close these — read this first

Commit `2a11b4f` ("gpu: ... fix dust/effect vertical-bar sampling (#8/#9)") attempted a fix and the
issues are still OPEN. That commit **removed** an unconditional `u &= 255; v &= 255;` from the fragment
shader (`tritex.frag`), on the reasoning that the SW reference (`sample_tex`) wraps U/V only through the
texture window, so forcing `&255` re-wrapped an over-256 U back into the same page and produced bars.

That diagnosis is **half right and the fix is a bandaid that moved the bug, not removed it**:
- It made `tritex.frag` AGREE with `sample_tex` — but both now sample **off the right edge of the
  texpage cell into adjacent VRAM** when U exceeds the cell width. The two paths agree on WRONG behavior.
- "Vertical bars" is exactly what off-page marching looks like in 4/8bpp: each step of 4 (or 2) texels in
  U advances one VRAM halfword; past the cell the halfwords belong to the neighbouring atlas tile / to a
  CLUT-index range that maps to a few repeating palette entries → narrow vertical color columns.

So the real question the prior session skipped: **why is the effect prim's U ever ≥ the cell width?**
That is a UV-generation / texture-window-application problem upstream of the sampler, NOT a `&255` clamp
choice in the shader. Neither clamping nor un-clamping at the shader is the fix; the **U must be confined
to the cell the way the PSX confines it — the texture window — and that confinement is currently not
reaching these prims.**

---

## 1. The effect-sprite submit path (guest packet → VK draw)

Effect billboards in Tomba2 are emitted as **op-0x60..0x7F textured rect/sprite** GP0 packets through the
guest OT, OR as textured quads (POLY_GT4) — both funnel into the same texel sampler. Trace:

### (a) Textured rect/sprite — `gp0_exec` op 0x60..0x7F (`runtime/recomp/gpu_native.cpp:899`)
- `textured = op & 0x04`, `semi = op & 0x02` (`:900`). Dust/impact puffs are semi-transparent textured
  sprites (op 0x65/0x66/0x67).
- Reads `u0 = uv & 0xFF`, `v0 = (uv>>8)&0xFF`, and **sets the CLUT from the same word**:
  `set_clut((uv>>16)&0xFFFF)` (`:905`). The **texpage is NOT in the sprite packet** — a sprite samples
  the texpage set by the most recent GP0-0xE1 / poly texpage word (`s_tp_x/s_tp_y/s_tp_mode`, `set_texpage`
  `:498`). The **texture window** is whatever GP0-0xE2 last set (`s_tw_mx/my/ox/oy`, decoded `:1125`).
- The quad corners are built with `qu[4] = { u0, u0+w, u0, u0+w }`, `qv[4] = { v0, v0, v0+h, v0+h }`
  (`gpu_native.cpp:978`). **So the top-right/bottom-right corner U is `u0 + w`** — and `w` is the sprite's
  *screen* width (`:907`, from the WH word, or 8/16 for sized sprites). The shader then linearly
  interpolates U across `[u0, u0+w]`.
- Tee'd to VK: `gpu_gpu_draw_semi` (semi) / `gpu_gpu_draw_tritri` (opaque) (`:995-1003` non-RQ;
  `rq_emit_or_queue` `:987` under the render queue), carrying `s_tp_x,s_tp_y, mode, s_clut_x,s_clut_y,
  s_tw_mx,s_tw_my,s_tw_ox,s_tw_oy`.

### (b) Textured quad — POLY_GT4 native submit (`engine/engine_submit.cpp:300` `submit_poly_gt4_native`)
- Reads 4 explicit UV bytes `u[k]/v[k]` from the record (`:327-330`), CLUT `(uv0>>16)` (`:324`),
  texpage `(uv1>>16)` (`:325`), and tees via `gpu_draw_world_quad` (`gpu_native.cpp:579`). Here each
  corner carries its OWN UV (not `u0+w`), so this path only stripes if the SOURCE UVs already exceed the
  cell — i.e. a content/atlas issue, less likely the dust puff.

### (c) The texel sampler both paths reach
- SW reference: `raster_sprite` (`gpu_native.cpp:659`) → `sample_tex(u,v)` (`:233`).
- VK: `tritex.frag` (`runtime/recomp/shaders_vk/tritex.frag:29-39`), fed by `tritex.vert` (UV is
  `noperspective` affine, `tritex.vert:14,27`).

Both decode **identically** (this is verified, see §2): texture-window wrap, then per-mode
(`0=4bpp,1=8bpp,2=15bpp`) VRAM fetch + CLUT lookup.

---

## 2. The decode is CORRECT and SW==VK — so the bug is NOT in bpp/CLUT/texpage math

The decode arithmetic is right for all three color modes and matches between the SW reference and the
shader. Side-by-side:

| step | SW `sample_tex` (gpu_native.cpp) | VK `tritex.frag` |
|------|----------------------------------|------------------|
| texwin wrap U | `:234` `u = (u & ~(tw_mx*8)) \| ((tw_ox&tw_mx)*8)` | `:30` identical |
| texwin wrap V | `:235` same | `:31` identical |
| 4bpp (mode 0) | `:243` `w=*vram(tpx+(u>>2),tpy+v); idx=(w>>((u&3)*4))&0xF` | `:37` identical |
| 8bpp (mode 1) | `:239` `w=*vram(tpx+(u>>1),tpy+v); idx=(u&1)?(w>>8):(w&0xFF)` | `:38` identical |
| 15bpp (mode 2)| `:237` `*vram(tpx+u,tpy+v)` | `:39` identical |
| CLUT fetch | `:241/245` `*vram(clut_x+idx, clut_y)` | `:37/38` identical |

`set_texpage` (`:498-503`) and `set_clut` (`:505`) decode the standard PSX bitfields correctly:
`tp_x=(tp&0xF)*64`, `tp_y=((tp>>4)&1)*256`, `mode=(tp>>7)&3`; `clut_x=(cl&0x3F)*16`,
`clut_y=(cl>>6)&0x1FF`. The texture-window register `0xE2` decode (`:1125`) is the standard layout
(`mask_x=w&31, mask_y=(w>>5)&31, off_x=(w>>10)&31, off_y=(w>>15)&31`). **None of bpp-stride, CLUT base,
or texpage base is wrong** — the hypothesis "wrong bpp stride / CLUT base / texpage misdecode" is ruled
out by this equivalence (the tex stride differences `u>>2` vs `u>>1` are present and correct).

The defect therefore lives **upstream of the sampler, in what U is asked to sample** — the U coordinate
range and the texture-window state that should confine it.

---

## 3. ROOT CAUSE — the effect prim samples beyond its texpage cell because the
##    PSX texture-window confinement is missing/ineffective for it

PSX texture sampling is confined to a cell by the **texture window** (GP0-0xE2): U/V are masked so a
small repeated/clamped pattern (e.g. a 16- or 32-texel dust/impact cell) wraps inside its own region
rather than marching into neighbouring VRAM. The wrap formula is implemented and correct (§2). The bug is
that, **for these effect prims, the confinement does not actually take effect**, so the affine U over
`[u0, u0+w]` (sprite path, `gpu_native.cpp:978`) — or oversized source UVs — runs off the cell's right
edge and re-reads adjacent VRAM / repeating CLUT indices = vertical bars. Concretely, one of:

1. **The texture window is zero (mask=0) when the effect prim draws.** With `s_tw_mx==0`,
   `u & ~(0*8) | …` is a NO-OP (`:234`/`tritex.frag:30`): U is unconfined and runs to `u0+w`. If the
   real game RELIES on a non-zero E2 window to keep the dust/impact cell tiling in place, and that E2 is
   either (a) not being applied to the effect's packets in our DRAW-ORDER replay, or (b) emitted by a
   path we don't honor, the prim samples off-page. This is the prime suspect for #8 (a small dust cell
   wrapped by a texture window) — the bars are the un-windowed atlas columns to the right of the cell.
   *Check live:* `provat x y` on a bar pixel → read the prim's `clut/texpage/node`; the live
   `[polydump]`/`[sprnode]` (`gpu_native.cpp:918`, objz channel) prints `tp=(…)` and the sprite's
   `uv0`/`w` — read whether `s_tw_mx`/`s_tw_my` are 0 at that prim (they are carried into the draw call
   args `:995-1003`).

2. **`u0 + w` is the wrong U-span for the sprite (screen width ≠ texel width).** The sprite quad uses the
   *screen* `w` as the U extent (`qu = {u0, u0+w, …}`, `:978`). For a 1:1 sprite that is correct, but if
   an effect sprite is drawn SCALED (screen `w` ≠ source texel width) the U range overshoots the cell.
   The PSX sprite primitive samples `w` *texels* (always 1:1 — PSX sprites can't scale), so `u0+w` is the
   right texel span ONLY because PSX sprites are unscaled; a poly/`gpu_draw_world_quad` billboard with
   stretched UVs is the place this breaks for #9's larger impact prim. Confirm whether the impact effect
   is a sprite (1:1, so #1 is its cause) or a stretched GT4 quad (then the source UVs themselves exceed
   the cell — an atlas-coordinate issue).

3. **The `2a11b4f` un-clamp made an unwindowed prim worse, not better.** Before that commit, `u&=255`
   at least re-wrapped a runaway U back inside the 256-texel page (visually a tiled repeat — still wrong
   but bounded). After it, a runaway U reads straight into the neighbouring texpage/CLUT region
   (`vram_at` only masks to the 1024×512 VRAM, `tritex.frag:14`), which is the *current* striped look. The
   real fix is neither `&255` nor no-clamp; it is **confine U to the cell the PSX way (the texture window),
   and make that window state reach the effect prim.** Removing the clamp without restoring the window is
   the bandaid that left the issues open.

**Named root cause:** the effect-sprite/billboard prims sample outside their texpage cell because the
texture-window confinement (GP0-0xE2 mask) that the PSX uses to tile/clamp the small effect cell is
**not in force on those prims at draw time** (mask=0 or not applied in our draw-order replay), and the
affine U over `[u0, u0+w]` then marches into adjacent VRAM/CLUT — producing vertical bars. The shader
`&255`/no-`&255` choice is a symptom-level knob, not the cause.

---

## 4. PC-native FIX PLAN (engine owns its render — confine sampling correctly, no magic offset)

The fix is to make sampling respect the cell, the way a correct renderer does — NOT to add a clamp
constant that happens to hide one scene.

1. **Establish, live, what state the bar-prim actually carries.** Drive to the dust/impact scene
   (§6), then on a bar pixel: `provat x y` → prim op / clut / texpage / node; and read `s_tw_mx,s_tw_my`
   (texture-window mask) + `u0` + `w` for that prim via the `objz`/`[polydump]` dump
   (`gpu_native.cpp:918`). Three outcomes decide the fix:
   - **mask==0 and the cell should tile** → the E2 window is missing for this prim. Find where the game's
     effect renderer sets GP0-0xE2 (texture window) for the effect's texpage and ensure our DRAW-ORDER
     GP0 replay applies it *before* the effect's packets (the comment at `gpu_native.cpp:1612-1617` notes
     E1/E2 must be applied in draw order — verify the effect's E2 isn't being dropped/re-ordered). The
     engine-owned path then carries the correct `tw_*` into `gpu_gpu_draw_semi`/`draw_tritri` and the
     existing wrap (`tritex.frag:30-31`) confines U to the cell — no clamp needed.
   - **mask!=0 but the bars persist** → the window IS set but our wrap or the `u0/w` span is wrong for
     that mode; re-derive the cell size from the window mask and clamp the sprite's U extent to the cell
     (`u0 + min(w, cell_w)`), matching PSX sprite semantics (sprites sample exactly `w` texels, never
     scaled).
   - **it's a stretched GT4 quad with source UVs already > cell** → the atlas UVs the effect record
     supplies are the issue; this is content data, and the engine must sample them with the cell's
     texture window in force (same E2-application fix) so the over-range U tiles instead of marching.

2. **Do NOT "fix" this by re-adding `u&=255` (or any fixed clamp) to `tritex.frag`.** That is the bandaid
   the prior commit removed for a reason (it broke genuine cross-page sprites elsewhere). The cell
   confinement must come from the real texture-window state, applied in draw order, so it is correct for
   ALL prims (cross-page backgrounds keep working; effect cells stop marching). Keep SW (`sample_tex`)
   and VK (`tritex.frag`) in lock-step on whatever rule is adopted (they are equal today — keep them so).

3. **If the effect renderer is still recomp content:** own its submit walk PC-native exactly like the
   aux-walks added for #4 (`engine_submit.cpp:837+`), reading the effect's texpage + texture-window +
   per-cell UV from the SCENE data and emitting the billboard via `gpu_draw_world_quad` with the cell's
   `tw_*` — so the confinement is the engine's own decision, not inherited from a possibly-dropped E2.

**USER must eyeball (agent cannot self-verify a render):**
- #8: walk Tomba on flat ground until the dust puffs appear under his feet — confirm soft cloud puffs,
  NOT gray vertical bars, and confirm they fade/blend (semi-transparent) correctly.
- #9: trigger the forest burst/impact effect — confirm the bright white-blue radial renders as a clean
  effect with no blue striped bar (top-middle) and no gray striped bar (right-middle).
- Regression watch: cross-page backgrounds (sea/sky tilemap), HUD icons, and any wide/ires run must be
  unchanged (the prior un-clamp was made for cross-page sprites — do not reintroduce their bug).

---

## 5. Are #8 and #9 ONE bug or two? — ONE family, justified

**One root-cause family.** Both are textured *effect* prims whose U samples beyond the texpage cell,
yielding the identical "vertical bars" signature (the off-page march reads adjacent VRAM columns / a
repeating CLUT-index range, which is inherently vertical). Both flow through the same sampler
(`raster_sprite`/`tritex.frag`) and the same un-confinement (texture-window mask not in force →
`u0+w`/oversized U marches off the cell). The issues themselves note the shared signature, and the prior
single commit (`2a11b4f`) tried to fix both at once — consistent with one cause.

**Caveat (could split into two sub-cases at the U-source):** #8 is most likely a 1:1 **sprite** whose
cell needs the texture window (case §3.1); #9, being a larger/brighter radial, could instead be a
**stretched GT4 quad** whose source UVs already exceed the cell (case §3.2). The CONFINEMENT fix (apply
the texture window in draw order, §4.1) addresses both; but the live `provat`/`objz` read in §4.1 must
confirm which U-source each uses, because if #9 is stretched-UV content the additional work is ensuring
the cell window is applied to that quad, not just the sprite path. Treat as one family, verify both.

---

## 6. Reachability — HONEST note (NOT cleanly headless-reproducible this session)

Both effects are gameplay-triggered (dust = sustained walking; impact = a combat/forest event) and are
**animation-frame-gated** — they appear only on specific frames of the walk/impact animation.

What I could do headless (binary `scratch/bin/tomba2_port`, arg = `scratch/bin/tomba2/MAIN.EXE`; the
`.chd` path needs `SCUS_944.54` beside it, so the pre-extracted MAIN.EXE is the working driver):
- Reached the "Burning House" village field (`newgame; skip 650; run …`) and captured Tomba mid-stride
  (`scratch/screenshots/dust_w2.png`, `dust_w3.png`, `walk1.png`, `atk1.png`).
- `debug objz`/`ndepth` channels active; `PSXPORT_PRIMDUMP=<frame>` CSVs written to
  `scratch/logs/prims_f*.csv`.

What I could NOT do: **catch the dust/impact prim itself.** Blind `press right`/`AUTO_WALK` did not
produce sustained walking on this terrain (Tomba reached an obstacle; the dumped frames show only the
static banner tiles + HUD icons, no dust sprite). So the live texpage/texwindow/UV read of an ACTUAL bar
prim (§4.1) was not obtained this session — the §3 root cause is grounded in the static data-flow +
the decode equivalence (§2) + the prior-fix analysis (§0), and §4.1 names exactly the live read the user
(or a session that can sustain walking) must take to choose between §3.1/§3.2.

**Live repro the USER should run** (windowed + debug server):
```
PSXPORT_GPU_WINDOW=1 PSXPORT_DEBUG_SERVER=1 scratch/bin/tomba2_port scratch/bin/tomba2/MAIN.EXE
# walk Tomba on flat ground until dust puffs (#8) / trigger the forest impact (#9), then on a BAR pixel:
tools/dbgclient.py debug objz                 # per-prim op/clut/texpage dump ([sprnode]/[polydump])
tools/dbgclient.py provat <x> <y>             # which prim/op/clut/texpage/node wrote the bar pixel
tools/dbgclient.py vkvram <tpx> <tpy> 256 256 scratch/screenshots/atlas.ppm   # verify the cell layout
tools/dbgclient.py shot scratch/screenshots/issue8.ppm
```
Read, for the bar prim: its `tp=(tpx,tpy)`, `clut`, `mode`, `u0`, `w`, and whether `s_tw_mx/my` are 0.
mask==0 ⇒ §3.1 (missing texture window); mask!=0 but bars ⇒ §3.2/oversized-U; the `vkvram` dump shows
whether the bars are the atlas columns just right of the cell (off-page march, confirming §3).

---

## Key file:line index
- `runtime/recomp/gpu_native.cpp:899-1006` — op 0x60..0x7F textured rect/sprite decode + VK tee (effect
  sprite path); **:905** CLUT from uv word, **:978** quad UV `{u0,u0+w,…}` (the affine U span), **:980**
  `mode = textured?s_tp_mode:3`, **:995-1003** `draw_semi`/`draw_tritri` carrying `tw_*`.
- `runtime/recomp/gpu_native.cpp:233-246` — `sample_tex` SW reference: texwin wrap (`:234-235`) + 4/8/15bpp
  + CLUT (`:237-245`). **No U-clamp to cell** (the un-confined sampling).
- `runtime/recomp/gpu_native.cpp:498-505` — `set_texpage`/`set_clut` (decode is correct).
- `runtime/recomp/gpu_native.cpp:659-682` — `raster_sprite` (SW), `u0+dx` → `sample_tex` (the U span source).
- `runtime/recomp/gpu_native.cpp:1125` — GP0-0xE2 texture-window decode (correct layout).
- `runtime/recomp/gpu_native.cpp:1612-1617` — note: E1/E2 (texpage/texwindow) MUST be applied in draw order.
- `runtime/recomp/shaders_vk/tritex.frag:29-39` — VK texel decode (matches SW exactly); **:30-35** the
  prior-fix comment + removed `u&=255` (the bandaid that left #8/#9 open).
- `runtime/recomp/shaders_vk/tritex.vert:14,27` — UV affine (`noperspective`) interpolation.
- `engine/engine_submit.cpp:300-340` — `submit_poly_gt4_native`: per-corner UV/CLUT/texpage, the
  GT4 billboard path (#9-stretched-quad candidate).
- `runtime/recomp/gpu_gpu.cpp:1464-1485` — `draw_tritri`/`draw_semi` → `tex_emit` (carry `tw_*` to vertex).
- prior commits: `2a11b4f` (the un-clamp "fix" that did not close #8/#9), `ca01e70` (tex_export
  `--frames` CLUT sprite-sheet decoder, built to RE the dust atlas).

## Caveats
- The actual bar prim's live state (texture-window mask, U span) was NOT captured this session — dust/
  impact are animation-gated and sustained walking was not achievable headless. Root cause is grounded in
  static data-flow + SW↔VK decode equivalence + prior-fix analysis; §4.1 names the exact live read to
  disambiguate §3.1 (missing texture window) vs §3.2 (oversized source UV) and must be done on the running
  game (USER, or a session that can sustain walking) before coding the fix.
- Do NOT re-add a fixed `u&=255` clamp — that is the bandaid `2a11b4f` deliberately removed; it breaks
  genuine cross-page sprites. The fix is real texture-window confinement applied in draw order.
