# 0203 — Crop widescreen (optional)

User, 2026-11-12: an optional widescreen mode for every title that crops the picture's height to 16:9
instead of extending the sides, with the UI moved to fit. It keeps the original logic and culling; the
extended-sides widescreen stays the default.

Prior art (2026-11-12): DuckStation's crop modes trim overscan only and its "Stretch to Fill" scales the
GTE aspect; neither crops gameplay height to 16:9 and relocates the HUD
([commit b180b26](https://git.morozoff.pro/aiden/duckstation/commit/b180b26728aa8d98a303ef992fc483073826d914)).

## Design

- **One setting**, owned by the config owner: widescreen `extend` (default) or `crop`. In `crop` the guest
  runs exactly as at 4:3: no canvas widening, no projection, cull or emitter change, so every widescreen
  override is off and the guest's draws are byte-identical to 4:3.
- **No viewport changes during play** (user, 2026-11-12). The screen is always 16:9 and every recorded
  primitive is rasterised with the mapping of its layer:
  - **world**: the 4:3 picture's band of full width and `width * 9 / 16` height mapped to fill the 16:9
    screen, rasterised at the sink's resolution (not an upscaled image);
  - **ui**: HUD, dialog, menu and full-screen menu draws mapped to a 4:3 box centred on the screen, exactly
    their 4:3 size and place, over the world layer;
  - **screen**: full-screen fades, flashes and letterbox bars stretched over the whole 16:9 screen.
- **Layer classification** is the title's, from what it already declares: draw-site tags and
  `render::callSiteOwner` for HUD and menu sites (the same tags its extend-mode HUD anchoring uses), OT bucket
  ranges where a title's UI owns whole buckets. Untagged draws are world. One owner per title maps owners to
  layers; the framework owns the mappings.
- **Band placement.** Centred by default. A title may supply a vertical anchor from RE'd state (the camera's
  focus row or the player's screen Y), smoothed by the 60 fps replay like any other presentation input, so
  the band never jumps. No pixel-based tracking.
- **Movies** map like the ui layer (4:3, centred) unless the title declares a movie safe to crop.
- **Interpolation** is unchanged: the replay produces the 4:3 record, and the layer mappings apply to each
  present.
- **Risk:** UI that the guest blends with the world beneath it (semi-transparent panels) blends with the
  world pixels now under it, which differ from 4:3; reviewed per screen.

## Rasterisation

The record rasteriser today draws every plane at one integer scale S in VRAM space: plane 0 is the VRAM image,
each canvas is a displayed buffer plus its margins (`RecordCanvas`), and the present scales the canvas into
the present image. Crop cannot be that: the world band (320 x 180 native for a 320-wide buffer) must fill the
sink at sink resolution, so its factor is 2160 / 180 = 12 at 4K and 10.43 for a 368-wide buffer, and an image
upscaled from S blurs or steps.

- **The VRAM image stays authoritative.** Plane 0 is rasterised exactly as now, at S. Textures, CLUTs, copy
  sources, uploads, fills, `StoreImage` reads and `recordcheck` read and write only plane 0. Nothing a guest
  can read ever comes from the crop target.
- **A crop target per displayed buffer**, sized to the present image (the sink), RG8 1555 like every plane,
  with its own depth texture and its own bit 15 for mask test and set. It is a presented plane, never a
  texture source. It is seeded from plane 0 through the layer mappings when made and after a resync, and kept
  across records like a canvas, since a title draws incrementally into a buffer it shows later.
- **Per-layer affine, rational.** Each layer maps buffer-relative native coordinates to target pixels by
  `t = (n - origin) * num / den` per axis, with integer `num` (target size) and `den` (native size): world
  `(W_t / w, W_t / w)` from the band's top row, ui `(H_t / h, H_t / h)` centred, screen `(W_t / w, H_t / h)`.
  `record.frag` evaluates gpu.c's integer edge functions in units of `1 / num` native pixel, so coverage, the
  top-left rule and interpolants stay exact integer arithmetic at a non-integer factor; no float divide
  decides which native pixel a target pixel belongs to (the S = 3 floor-division lesson). A native pixel covers
  target columns `[ceil(x * num / den), ceil((x + 1) * num / den))`, so sprites, fills, lines and texel blocks
  tile without gaps or overlaps at widths `k` and `k + 1`.
- **One planner path.** The planner already plans each entry once per plane. A plane's description grows from
  (origin, S) to a mapping per layer; plane 0 and extend-mode canvases carry one mapping, `(S, 1)` from their
  origin, and plan exactly as today. The crop target carries three and picks one per entry by the entry's layer.
  Quads per primitive stay constant; target fragment work is the sink's pixels, independent of S.
- **What reaches the target:** every entry that lands in the displayed buffer: draws (clip = the draw area
  through the entry's mapping), fills, and copies and uploads into the buffer, drawn as a resample of plane 0's
  pixels at their native blocks through the layer mapping (an upload as at 1x, a copy at S detail).
- **Present** copies the target 1:1 into the present image; the overlay draws over it as now. An in-between
  draws into a copy of the target, as it does into a canvas copy today.

## How it sits on the per-record margins design

The margins rework (owned by the Mega Man X4 agent until it reports) carries each record's margins in the
frame record and keeps one canvas per buffer and aspect. Crop builds on that, not beside it:

- The frame record carries its presentation: extend with margins L/R, or crop with the band's top row (and the
  smoothed anchor). Crop records have zero margins: the guest runs at 4:3, so no canvas exists and plane 0 is
  the 4:3 picture, as at M = 0 today.
- The crop target is keyed like a canvas, by displayed buffer and presentation; a change of mode, band or sink
  size retires it as a margin or width change retires a canvas.
- `RecordCanvas` and the crop target become one plane description (VRAM-space origin plus layer mappings), so
  `drawAreaSpansBuffer`, `addCanvasMargins` and the read-back capture keep one owner. Crop code waits for that
  rework to land in `record_rasterizer`, `record_raster_setup`, `frame_presenter` and
  `guest_widescreen_projection`.

## What the split cannot express

- **A guest reading back the presented picture** (`StoreImage` of the displayed buffer, converted on the CPU and
  loaded as a texture: Spyro 1's pause backdrop) reads plane 0, the 4:3 picture at S. A declared read-back page
  (`present::ReadBackTexture`) is captured from the crop target instead, through the world mapping, and a sprite
  mapping it one texel per pixel samples that capture in the world layer; an undeclared one shows plane 0's 4:3
  band at S detail.
- **Render-to-texture from the displayed buffer** (a draw that samples what the record drew there) samples plane
  0: right in place, S detail, not sink detail.
- **A copy whose source is the displayed buffer** (a guest scrolling or shaking its own picture) copies plane 0's
  pixels, so the moved region shows S detail until redrawn.
- **Interlace**: the target draws every row; the field weave stays in plane 0.
- **UI blended over the world** blends with the cropped world under it (the risk above).

## Steps

1. Config: the widescreen mode value, its UI row and the settings migration.
2. Record rasteriser, after the margins rework lands: the plane description with per-layer rational mappings,
   the crop target per displayed buffer, a per-entry layer in the frame record, and the 1:1 present.
3. A title hook mapping its draw owners to layers, and the band anchor; Tomba 2 and Spyro first.
4. Docs: `presentation.md` (Widescreen) and each title's state row.

## Proof

Tomba 2 and Spyro in crop mode: guest RAM after a warp equals the 4:3 run's; the world layer shows exactly the 4:3
frame's band; HUD, dialog and menus sit at their 4:3 size and place in every screen with no viewport
change; 60 fps in-betweens stay halfway. Cost in `perf-units`: crop adds the target's quads (a constant per entry in the
buffer) and its sink-sized fragment work, and nothing on the CPU grows with S or the sink. Extend mode
and 4:3 are byte-identical to before.
