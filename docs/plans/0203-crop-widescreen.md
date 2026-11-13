# 0203 — Crop widescreen (optional)

User, 2026-11-12: an optional widescreen mode for every title that crops the picture's height to 16:9
instead of extending the sides, with the UI moved to fit. It keeps the original logic and culling; the
extended-sides widescreen stays the default. No viewport changes during play (user, 2026-11-12).

Prior art (2026-11-12): DuckStation's crop modes trim overscan only and its "Stretch to Fill" scales the
GTE aspect; neither crops gameplay height to 16:9 and relocates the HUD
([commit b180b26](https://git.morozoff.pro/aiden/duckstation/commit/b180b26728aa8d98a303ef992fc483073826d914)).

## Shape

- **Setting.** `widescreen` (`PSXPORT_WIDESCREEN`, the Widescreen row): `Extend` (default) or `Crop`, offered only to
  a title that names its UI (below); for any other the row is unavailable and the setting resolves to Extend
  (`Mods::crops`), the saved value kept. With Crop and a
  wide aspect, `guestProjectionPlan` keeps the widening target at 4:3, so no canvas, projection, cull or emitter
  changes: the guest runs exactly as at 4:3 and every extend-mode override sees no margin.
- **Band** (`present::CropBand`, `crop_band.h`): the 4:3 picture's full width over `p/q` of its rows, centred.
  16:9 keeps 3/4, 21:9 4/7, Auto the nearest `p/q` with `q <= 8` to `4 * sinkH / (3 * sinkW)`. Rows are
  `floor(h * p / q)`.
- **Two layers, two whole-number scales.** With the image at scale S, the unit is `m = ceil(S / p)`: the world draws at
  `Kw = q * m` from the band's top row, the UI at `Ku = p * m` over the whole display, centred. At 16:9 and S = 8 that is
  12 and 9: the band's 180 rows and the display's 240 both fill 2160 target rows. No rational mapping: both layers are
  integer scales, so `gpu.c`'s coverage and interpolants stay the planner's own at that scale.
- **Classification is the title's,** from RE'd identity, never from where a primitive lands: its
  `present::CropUiLayers` (`GameRuntime::cropUiLayers`, its presence the title's declaration of Crop) names the
  ordering-table packets of the frame just drawn that are UI. The frame presenter asks at the seal
  (`classifyCropLayers`), the replay asks inside each in-between's draw (`ReplayFrame::Capture::uiPackets`), since
  in-between primitives carry no slot or owner. `DrawPrimitive::packet` is the OT node that carried a primitive
  (`Gp0Origin::packet`, `decodePacket`).

## Rasterisation

- **The VRAM image stays authoritative.** Plane 0 is drawn exactly as in extend at S. Textures, CLUTs, copy sources,
  uploads, read-back and `recordcheck` read only plane 0.
- **A crop canvas per displayed buffer** reuses the canvas slots (`gpu::cropCanvas`, `RecordCanvas::crop()`): `buffer`
  is the band at `worldScale`, `uiBuffer` the whole display at `uiScale` from `uiOrigin`. Its `Plane` has its own
  scale (`Plane::scale`), textures and depth. A new one is seeded by a copy from the image (`planCropSeed`); a resync's
  VRAM upload lands in it as world.
- **Two ops per primitive.** A primitive planned with crop canvases present is planned first at its layer's scale
  onto the crop canvases (`Builder::Pass::Crop`) and then at S onto the image, so the crop op's reads see the batch
  as the image op's do. Crop ops draw every row (no field weave), never sample a read-back capture, and the UI layer
  never meets the world's depth. Quads carry their mapping (`RecordQuad::mapping`, `kMappingCropWorld/Ui`);
  `record.frag` takes S and the origin from it and fetches VRAM at the image's scale (`Params.plane.w`).
- **Transfers:** fills and copies into the band are world; an upload smaller than VRAM (a movie frame) is UI. A copy
  into a crop canvas takes whole native pixels from the image at S.
- **Present:** the crop canvas whole, at its scale, as a `4h : 3 * rows` picture (`PresentInputs::picture_w/h`).

## Limits

- A guest read-back (Spyro 1's pause backdrop) shows the 4:3 page at 1x in the band.
- The UI is clipped where `h * Ku` exceeds the target's rows (`h * p % q != 0`).
- Memory: a crop canvas is `w * Kw` by `rows * Kw` (6144 x 2160 for 512 wide at 16:9, S = 8), four RG8 textures and a
  depth texture, ~160 MB, one per displayed buffer, beyond the ires budget.
- Cost: the band at Kw on top of the image at S. Spyro 1 Artisans at 16:9, S = 8: GPU pixels p50 187 Mpx/present
  (extend 157, 4:3 67), CPU present p50 68 Minsn (extend 68).

## State

- Done: setting, band, planner, rasteriser, present, classification hooks; Spyro 1/2/3 (`psx-spyro`
  `game/render/crop_layers.*`, `game/render/overlay_pair.*`). Guest RAM equals the 4:3 run's in Spyro 1/2/3
  and Tomba 2.
- Titles without a `CropUiLayers` (Tomba 2 and the rest today) are not offered Crop.
- Open: Tomba 2's classification (UI through native emitters and guest leaves across buckets 1..4, 0x7FF and
  attribute-chosen ones); the other titles; a band anchor from RE'd camera state.
