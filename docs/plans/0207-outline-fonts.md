# 0207 — Outline ("SVG") fonts for game text

User, 2026-11-13: "Also probably should add xbox glyphs and svg fonts". An "SVG font" is an outline
font rendered crisp at any size (TTF/OTF through SDF or MSDF), not the SVG file format.

## What

The game's own text (dialogue, menus, HUD numbers, level names) draws from an outline font at the
sink's resolution instead of the guest's bitmap font scaled up, so it stays sharp at 4K. Layout,
timing and colours are the game's.

## Design

- One framework owner, `runtime/psx/present/outline_text`, renders a run of codepoints at a guest
  screen position, scale and colour as MSDF glyphs in the record's own ordering-table slot, so it
  layers exactly where the bitmap text did and follows widescreen anchoring and 60 fps blending.
- Each title declares its font: the texture page and cell grid of its bitmap font and the
  cell-to-codepoint table, RE'd from its text drawer and recorded in its decomp. The title's text
  drawer is the seam: it reports each string it draws; the guest still builds its primitives, and the
  presenter replaces those cells with outline glyphs.
- Fonts are permissive (SIL OFL or more permissive), one per title chosen to match the original's
  shape, shipped with the framework; no game font is redistributed.
- The port's own UI already uses outline fonts (RmlUi with Fira Sans).

## Phases

1. `outline_text` and Tomba 2's dialogue and menu font.
2. Spyro 1/2/3 (one shared font owner in psx-spyro `game/`), then each title as it is worked.
