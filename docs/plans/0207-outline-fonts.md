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
2. Spyro 2/3 (one shared font owner in psx-spyro `game/`), then each title as it is worked. Spyro 1 has
   nothing to replace: its text is mesh letters the record rasterizes at the internal resolution
   (`spyro1.text-glyphs`, open-spyro).

## Spyro 2/3 seam

The text layouts (S2 `0x8004A374`/`0x8004A59C`, S3 `0x8002E748`/`0x8002E970`) already run as overrides
(`game/render/overlay_pair`). Each reports the packets it wrote with their character code; the record op for
such a packet samples the MSDF atlas instead of VRAM, as `kOpReadBack` samples the read-back capture, so
it stays in its ordering-table slot, its widescreen place and its replay. Glyph metrics come from the guest
cell (12x9 at the cell's place), colour from the packet; MSDF generation is msdfgen (MIT) over the
FreeType the UI already links.
