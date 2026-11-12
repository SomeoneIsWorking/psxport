# 0202 — Crop widescreen (optional)

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

## Steps

1. Config: the widescreen mode value, its UI row and the settings migration.
2. Record rasteriser: a per-primitive layer and one mapping per layer (world band, ui box, screen).
3. A title hook mapping its draw owners to layers, and the band anchor; Tomba 2 and Spyro first.
4. Docs: `presentation.md` (Widescreen) and each title's state row.

## Proof

Tomba 2 and Spyro in crop mode: guest RAM after a warp equals the 4:3 run's; the world layer shows exactly the 4:3
frame's band; HUD, dialog and menus sit at their 4:3 size and place in every screen with no viewport
change; 60 fps in-betweens stay halfway. Extend mode
and 4:3 are byte-identical to before.
