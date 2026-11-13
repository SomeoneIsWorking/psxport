# 0207 — Outline ("SVG") fonts for game text

User, 2026-11-13: "Also probably should add xbox glyphs and svg fonts". An "SVG font" is an outline
font rendered crisp at any size (TTF/OTF through SDF or MSDF), not the SVG file format.

## What

The game's own text (dialogue, menus, HUD numbers, level names) draws from an outline font at the
sink's resolution instead of the guest's bitmap font scaled up, so it stays sharp at 4K. Layout,
timing and colours are the game's.

## Design

- One framework owner, `runtime/psx/present/outline_text`, keeps an MSDF atlas of the title's font
  page; the record rasterizer samples it instead of VRAM for the primitives that drew font cells, so
  each glyph stays in its ordering-table slot, widescreen place and 60 fps replay.
- Each title declares its font: the texture page and cell grid of its bitmap font and the
  cell-to-codepoint table, RE'd from its text drawer and recorded in its decomp. The title's text
  drawer is the seam: it names the packets it linked; the guest still builds its primitives.
- Fonts are permissive (SIL OFL or more permissive), one per title chosen to match the original's
  shape, shipped with the framework; no game font is redistributed.
- The port's own UI already uses outline fonts (RmlUi with Fira Sans).

## Phases

1. Spyro 2/3's 2D font (one shared font owner in psx-spyro `game/render/text_font.h`): built. Spyro 1 has
   nothing to replace: its text is mesh letters the record rasterizes at the internal resolution
   (`spyro1.text-glyphs`, open-spyro).
2. Next, in this order:
   a. Glyph-rectangle fonts: `OutlineFont` names each glyph's rect on its page (u, v, width, height) with its
      codepoint or button, and the fixed grid becomes one way to fill that table. A title names several fonts;
      `OutlineGlyphs` tags each packet with the font it drew from, and the atlas holds every font's cells.
   b. Tomba 2's dialogue and menu font on that model.
   c. Spyro 3's Atlas book font (spyro-3 `spyro3.atlas-font`): 12x9 semi-transparent quads (0x2E), a shadow
      pass modulated 0x202020 then the text modulated 0x78898C, from DR_TPAGE 0x0009 (x 576 y 0, 4bpp)
      through CLUT 0x09F2; glyphs packed at varying u/v with the Triangle, Square, Circle and Cross icons
      below them. Still to RE: the Atlas text drawer and its glyph table.
   d. The HUD counters: Spyro 3's value emitter 0x800291B8 drawing each digit through the icon emitter
      0x800289C8, Spyro 2's HUD counter and sprite sites 0x8005251C and 0x800520CC; their digit pages and
      tables are still to RE.
   e. Last, Spyro 2's pre-rendered string images (spyro-2 `spyro2.title-strings`): the title screen's
      memory-card and language messages are whole strings in 8bpp pages at (512,256) and (640,256), drawn as
      160x104 sprites (0x64, DR_TPAGE 0xE1000498/0xE100049A); neither text layout runs there. Each needs a
      table of which rect shows which string (the message tables are at 0x80063B74 and 0x80061B70), and the
      outline lays the string out in that rect.

## As built

- `runtime/psx/present/outline_text` owns it. A title's `GameRuntime::outlineFont()` names its page (texture
  page, cell size and grid, the cell-to-codepoint table, the text face and its weight, the shadow's CLUT
  indices and offset); `outlineGlyphs(core)` names the ordering-table packets its text drawer filled with
  cells. At seal, `markGlyphs` flags those textured primitives (`DrawPrimitive::outline`); a replayed
  in-between carries the same flags through `Capture::glyphPackets`.
- A frame that drew flagged packets brings the atlas up to date: each cell's bitmap is read from VRAM, and a
  cell whose bitmap or codepoint changed gets a new field: the codepoint's outline (the title's face, or
  PromptFont for a button cell, whose codepoint the pad in hand picks through `GlyphSet`) fitted to the box of
  the cell's glyph texels, as an MSDF in RGB and the shadow's SDF, moved by the shadow offset, in A, eight
  field pixels a texel. Fields take their sign from the outline's nonzero fill, then msdfgen's error
  correction (its scanline pass): variable faces keep overlapping contours.
- Colours stay the guest's: the index texture holds per cell row the CLUT index most of the row's glyph
  texels use (R) and its shadow's (G), so the glyph shades down the cell as the bitmap does; the packet's CLUT,
  modulation and blending apply as for any texel. A button cell's B names its slot in `OutlineAtlas::buttons`;
  at draw time `buttonFaceColours` asks `GlyphSet::faceColour` per slot and the glyph takes that colour, keeping
  its CLUT texel's semi-transparency bit: Xbox prints A green, B red, X blue, Y yellow; PlayStation, Nintendo
  and the keyboard keep the game's colours.
- The record op of a flagged primitive (`kOpOutline`, set only at internal scale above 1) samples the atlas
  instead of VRAM in `record.frag`, so it keeps its ordering-table slot, crop UI place and replay. At scale 1
  the bitmap draws.
- Check: Spyro 2 Summer Forest pause and Spyro 3 Sunrise Spring pause and Help at `ires=0`, 3840x2160
  (`warp 0/0/0`, start, down, cross): letters and button glyphs sharp, shaded and shadowed as the bitmap; Help
  under `gamepad make xbox` and `gamepad make playstation` for the face colours.
