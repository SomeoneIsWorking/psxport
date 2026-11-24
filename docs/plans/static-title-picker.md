# Static title picker

The selector shows each title as a still picture decoded from the player's own disc. Nothing runs until a title is
picked. This replaces the live stage, where every title booted and the focused one played its attract demo.

## What the player sees

- Every available title has a panel, side by side across the window, split by slanted seams (Spyro's three titles
  are three panels). The selected panel is wider and in colour; the others are grey.
- A panel's picture is the title's own art (Tomba! 2: its title-screen illustration). Every panel draws its art at
  the same whole-texel scale, the largest at which the tallest art fits the window's height. Selection changes a
  panel's width and colour, never its scale: the largest whole scale at which every art fits the window's height and
  the selected panel's clear width. Art is centred in its panel and cut by the seams.
- A title with no art yet shows a plain tile; the bottom band names every title.
- Picking slides the seams off as now, then boots the picked title. Returning ends that title's session and rebuilds
  the selector.

## Owners

- `GameRuntime::panelArt(psx::cd::Disc &)`: the title reads its art from its disc (`readDiscFile`/`readDiscFileRange`
  and the title's own codec) as an `ArtScene`: VRAM uploads and the raw-texture sprites its title screen draws. `nullopt`
  when it has none. The decomp repository records where the art lives.
- `present/art_scene`: draws an `ArtScene` with gpu.c on a scratch device; unwritten pixels are transparent.
- `PanelArt` (replaces `PanelLogo`): the drawn image and its texture, read from the disc each time the selector starts.
- `PickerSession`: panels from `PickerContent`, layout from `PickerLayout` over all panels, input, the pick
  animation. On a pick it returns the slug; `ProductHost` creates and boots that title's `TitleSession`.
- `PickerPanes`: each panel's art at the shared scale, or a tile, and the dividers.

## Removed

`PanelSessions`, `StageSides`, `TitleSession` hand-over/take-back and its panel role, `Game::session.preview()` and the
title-side preview skips (psx-spyro `field_owner`/`transition_skip`, psx-tomba `boot_cards`/`screen_skip`, psx-spider-man
`screen_skip`), `SessionControl::preRolling`, the live logo read (`panelLogo`, `logoTextures`, `showsOwnLogo`,
`present/drawn_logo`) and the debug server's per-panel verb routing.

## Titles

| title | art |
|---|---|
| Tomba! 2 | `TOMBA2.IMG` set 2, the 320 x 240 title illustration through palette row 511 (tomba2-decomp `docs/notes/texture-groups.md`) |
| Tomba! 1 | the title screen's `A0001*.GAM` pages and sprites (tomba1-decomp `docs/notes/title-screen.md`) |
| Spyro 1/2/3 | the wordmark from `WAD.WAD` (each fork's `docs/notes/title-wordmark.md`) |
| other multi-title products | tile until found |

## Proof

The selector at the player's settings (ires auto, 3840x2160 sink) for psx-tomba and psx-spyro: every panel shows its
art at one scale, moving the selection changes only widths and colour, a pick boots the title, a return comes back
to the selector. Two runs give the same pictures.
