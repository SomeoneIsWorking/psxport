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
   a. Glyph-rectangle fonts: built (see As built).
   b. Tomba 2's text fonts on that model: built (psx-tomba `game/ui/text_fonts.h`, tomba2-decomp
      `docs/notes/text-fonts.md`): the 8x16 and 8x8 letter fonts and the 16x16 button icons of page (960, 256), named by
      `Font::glyphEmit` (drawText, drawTextSmall), `Font::iconGlyphEmit`, `DialogGlyphs::emit` and `prompt`.
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

## Next steps

Phases 2a and 2b are built and shown (Spyro 2/3 pixel-identical before and after; Tomba 2 pause menu and opening
narration at `ires=0`, moving halfway on in-betweens). Continue with 2c, 2d and 2e in that order, each on the
glyph-rectangle model: RE the drawer and its glyph table into the title's decomp first, then a title font table
(`OutlineFont` with `OutlineGlyph`s, `OutlineGrid` where cells are equal) and the drawer naming each glyph packet's
font through the title's `OutlineGlyphs`.

- 2c, Spyro 3's Atlas font: RE'd (spyro-3 `spyro3.atlas-font`): the Atlas string drawer 0x80076228 draws each
  glyph through the icon emitter 0x800289C8 from the SpriteData table *0x80070108 (indices 0x42-0xA2, 12x9 rects,
  CLUT 0x09F2), so the font table is that index list and the seam is the drawer's call of 0x800289C8. The glyphs lie
  on five 4bpp pages (DR_TPAGE 0x09-0x0D, VRAM x 576-831); the packed atlas (As built) takes them. The 0x202020 colour
  earlier read as a shadow pass is the Contents caller's colour choice (0x80076958); check for a real second pass
  when naming packets.
- 2d, the HUD counters: Spyro 3's value emitter 0x800291B8 draws each digit through the same icon emitter 0x800289C8
  and SpriteData table as the Atlas, so the packing above serves both.
  Spyro 2's 0x8005251C/0x800520CC are still to RE.
- 2e, Spyro 2's pre-rendered string images: whole strings, not glyphs; this needs a layout of the string into its
  rect rather than an `OutlineGlyph` per cell, so it is new framework work.
- Tomba 2 leftovers (tomba2-decomp `docs/notes/text-fonts.md`): the token strings are the four buttons, already
  named; the v 152 font has no drawer. The reference mark (cell 0x0A) and the symbol cells 0x64-0x98 the token table
  names need a face none of Fira Sans, Fredoka and PromptFont has (※ ○ ◇ □ △ ♪ 『』 and the rest): a JIS-complete
  OFL face such as Noto Sans JP or M PLUS 1 covers every token, pending review. Most symbols are two-coloured (a fill
  in indices 3/4), which one index a row cannot carry.
- Multicoloured glyphs: the A channel of an unshadowed glyph could hold the field of its silhouette (the outer contour
  filled), so a button's symbol, cut from PromptFont's disc, takes the symbol's own CLUT index from the bitmap under
  it. Shadowed glyphs (Tomba 2's 8x8 buttons) have no channel left for it.
- psx-tomba carries the `padArt` marks on its button glyphs (`game/ui/text_fonts.h`), uncommitted until the psxport
  commit that adds `padArt` is pushed.
- Proof routes: Spyro `ally_text.sh` (Ally, `warp 0/0/0`, start, downs, cross; Spyro 3 Help is three downs);
  Tomba 2 `psx-tomba/scratch/outline-text/tools/` (`drive.py` with the REPL files: title New Game, `warp 8/0`,
  start; the narration dump is mid-route). Tomba 2 Options: `psx-tomba/scratch/outline-text2/options.txt`. Spyro 3
  Atlas locally: `psx-spyro/scratch/atlas-font/atlas.txt` (`warp 0/0/0`, start, down, cross).

## As built

- `runtime/psx/present/outline_text` owns it. A title's `GameRuntime::outlineFonts()` names up to four fonts
  (`kMaxOutlineFonts`). An `OutlineFont` is a texture page and its glyphs, each an `OutlineGlyph`: the rectangle
  (u, v, width, height) its text drawer samples, with its codepoint and face or its PSX button; `OutlineGrid` fills
  them for a font of equal cells. Also per font: the text face and its weight, the shadow's CLUT indices and offset.
  `outlineGlyphs(core)` names, for each ordering-table packet its text drawers filled with glyphs, the font it drew
  from (`OutlineGlyphs::glyphFont`). At seal, `markGlyphs` tags those textured primitives with their font
  (`DrawPrimitive::outlineFont`); a replayed in-between carries the same tags through `Capture::glyphPackets`.
- A frame that drew tagged packets brings each drawn font up to date in the atlas: each glyph's bitmap is read from
  VRAM, and a glyph whose bitmap or codepoint changed gets a new field: the codepoint's outline (the title's face, or
  PromptFont for a button glyph, whose codepoint the pad in hand picks through `GlyphSet`) fitted to the box of the
  glyph's ink texels, as an MSDF in RGB and the shadow's SDF, moved by the shadow offset, in A, eight field pixels a
  texel; a `proportional` font keeps the outline's proportions at the ink box's height instead, centred across it and
  no wider (Tomba 2's serifed I, 1 and l in a sans face). Fields take their sign from the outline's nonzero fill, then msdfgen's error correction (its scanline pass):
  variable faces keep overlapping contours. The fields are packed, one tile a glyph in shelves 2048 pixels wide with a
  one-pixel gutter repeating the tile's edge, so a font whose glyphs spread over several pages costs only its glyphs.
  The index map holds the fonts' page boxes one under another (`OutlineAtlasFont`), two pixels a texel: the CLUT
  indices and button slot, then the field pixel of the texel's top-left in its glyph's tile. A font not drawn keeps
  its last fields. A texel of a font's box that no named glyph with an outline covers (index A 0) draws its bitmap
  from VRAM, so a font names only the glyphs it has faces for.
- A button glyph marked `padArt` is the PlayStation button itself in several colours (Tomba 2's rings and symbols):
  under a PlayStation pad it has no codepoint and draws its bitmap; under any other family its PromptFont glyph.
- Colours stay the guest's: the index texture holds per glyph row the CLUT index most of the row's glyph
  texels use (R) and its shadow's (G), so the glyph shades down its rectangle as the bitmap does; the packet's CLUT,
  modulation and blending apply as for any texel. A button glyph's B names its button's slot in `OutlineAtlas::buttons` (four buttons across all fonts);
  at draw time `buttonFaceColours` asks `GlyphSet::faceColour` per slot and the glyph takes that colour, keeping
  its CLUT texel's semi-transparency bit: Xbox prints A green, B red, X blue, Y yellow; PlayStation, Nintendo
  and the keyboard keep the game's colours.
- The record op of a tagged primitive (`kOpOutline`, set only at internal scale above 1, its font in the two bits
  from `kOpOutlineFontShift`) samples that font's place in the atlas instead of VRAM in `record.frag`, so it keeps its ordering-table slot, crop UI place and replay. At scale 1
  the bitmap draws.
- Check: Spyro 2 Summer Forest pause and Spyro 3 Sunrise Spring pause and Help at `ires=0`, 3840x2160
  (`warp 0/0/0`, start, down, cross): letters and button glyphs sharp, shaded and shadowed as the bitmap; Help
  under `gamepad make xbox` and `gamepad make playstation` for the face colours. Tomba 2's Options screen (New Game,
  `warp 8/0`, start, cross) under both makes for its `padArt` buttons.
