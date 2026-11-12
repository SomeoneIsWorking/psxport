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
- **The crop** is a presentation step on the sealed frame: a band of the 4:3 picture, full width and
  `width * 9 / 16` tall, scaled to the sink. `present::fitPicture` (`viewport_layout.h`) already centre-crops
  a pane's image; extend it as the one crop owner rather than adding a second.
- **Band placement.** Centred by default. A title may supply a vertical anchor per screen from RE'd state
  (for example the camera's focus row or the player's screen Y) so the action stays in the band; the anchor
  is smoothed by the 60 fps replay like any other presentation input. No pixel-based tracking.
- **UI.** HUD, dialog and menu draws that fall outside the band move into it. Each title owns this, through
  the same draw-site tags it already uses for widescreen HUD anchoring: a tagged 2D draw is offset at present
  time by the rows its anchor edge lost (top-anchored down, bottom-anchored up). Untagged 2D stays where the
  guest put it; screens whose UI would be cut (full-screen menus, text pages) present uncropped at 4:3 with
  pillarbox bars, declared per screen by the title (user, 2026-11-12: Mega Man X4's pause screen and Tomba 2's
  inventory show normally at 4:3, unadjusted).
- **Movies** present uncropped (pillarboxed) unless the title declares a movie as safe to crop.
- **Interpolation** is unchanged: the replay renders the 4:3 frame and the crop applies to each present.

## Steps

1. Config: the widescreen mode value, its UI row and the settings migration.
2. `fitPicture` takes the crop band; the presenter applies it on the shown image in crop mode.
3. A title hook for the band anchor and for screens that opt out of the crop.
4. Draw-site UI offset for tagged 2D in crop mode; Tomba 2 and Spyro first, each title's HUD tags reviewed.
5. Docs: `presentation.md` (Widescreen) and each title's state row.

## Proof

Tomba 2 and Spyro in crop mode: guest RAM after a warp equals the 4:3 run's; the shown band matches the 4:3
frame's rows exactly; HUD, dialog and menus are fully visible; 60 fps in-betweens stay halfway. Extend mode
and 4:3 are byte-identical to before.
