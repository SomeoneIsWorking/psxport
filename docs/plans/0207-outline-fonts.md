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
   e. Spyro 2's title-screen strings: built for the one-colour lines (see Next steps).

## Next steps

Phases 2a to 2e are built and shown. Phase 2 is done.

- 2c, Spyro 3's Atlas font: built (psx-spyro `titles/spyro3/render/sprite_fonts.h`, spyro-3 `spyro3.atlas-font`). The icon
  emitter's override names each packet it drew from entries 0x42-0xA2 of *0x80070108 as the Atlas font, whose glyphs
  lie across DR_TPAGE 0x09-0x0D. The marks are grave, acute, diaeresis, circumflex and tilde; there is no shadow pass;
  the button icons 0x9F-0xA2 are pad art. Face Fredoka 600.
- 2d, the HUD counters: built. Spyro 2's (spyro-2 `spyro2.hud-digits`): `hud_element::draw` names the sprites of the
  digit images 0x8006351C; 24x16 cells at v 45 of the 2D font's page, gold bands over a (1, 1) shadow in indices 2, 7
  and D, Fredoka 500 (`spyro::hudDigitFont`). Spyro 3's (spyro-3 `spyro3.atlas-font` notes): the value emitter
  0x800291B8 reads digits 0-9 and the slash from *0x8006C788 at the halfword 0x800719CC, a table whose entries move
  with the level, so `sprite_fonts::Fonts::follow` reads them each sealed frame into the font (its page the entries'
  top-left page, shadow indices 2, 7 and A) and the icon emitter's override names the packets drawn from them.
- 2e, Spyro 2's title-screen strings: built (psx-spyro `titles/spyro2/render/title_strings.*`, spyro-2
  `spyro2.title-strings`). They are a picture, not glyphs: WAD entry 95 + 0x40800 lands as VRAM (512, 256)..(1024, 512),
  and the boot overlay's image drawer 0x80079878(x, y, image, colour) draws each string as one sprite from its table
  0x80063F2C + 6 * image; no character data for them exists in the game. The fifteen one-colour lines, images 0x3C-0x4A
  on the 4bpp pages (768, 256) and (832, 256) ('Slot 1', 'Create Block', 'Memory Card 1 (Triangle) to Return', ...),
  draw from Fredoka 500 as an `OutlineGlyph` per ink run (159, each line's runs counted against its text), the
  Triangle as a button glyph; no framework change was needed. The drawer's override is installed when the boot overlay's
  read lands (`BootPrefixRuntime::codeLanded`, keyed on its SHA-256) and names the sprite of each such image. Kept as
  bitmaps: the message lines 0x20-0x3B ('Select MEMORY CARD:', ...), anti-aliased through a cream-to-black ramp that
  rims each letter on all sides, which one CLUT index a row over an offset shadow cannot draw; 'press start', the
  language names and the % digits, 8bpp lettering shaded across the glyph. Check: title, Start (Slot 1 / Slot 2),
  Cross (Create Block / Cancel, Memory Card 1 to Return), Cross (New Game / Load Game) at `ires=0` against `ires=1`.
- Tomba 2's symbols: built (psx-tomba `game/ui/text_fonts.h`, tomba2-decomp `docs/notes/text-fonts.md`): cell 0x0A as
  ※ and the token table's one-colour symbol cells from `NotoSansJP-Symbols.ttf` (`OutlineFace::Symbol`). The 8x16
  font's filled arrows, ◇ □ △ ▽ and both fonts' box 0x98 keep their bitmaps, as do the diagonal arrows 0x68-0x6B and
  the nested squares 0x7D/0x7E, which the face has no shape for. Only ■ (the Sound screen's volume bars) is drawn by a
  US string found so far.
- Multicoloured glyphs: the A channel of an unshadowed glyph could hold the field of its silhouette (the outer contour
  filled), so a button's symbol, cut from PromptFont's disc, takes the symbol's own CLUT index from the bitmap under
  it. Shadowed glyphs (Tomba 2's 8x8 buttons) have no channel left for it.
- Proof routes: Spyro `ally_text.sh` (Ally, `warp 0/0/0`, start, downs, cross; Spyro 3 Help is three downs); Spyro 2
  HUD: `warp 0/0/0`, select (the HUD slides in; `debug fps60dump` for its presents); Spyro 3 Atlas: `warp 0/0/0`,
  start, down, cross. Tomba 2 `psx-tomba/scratch/outline-text/tools/` (narration) and Options/Sound (New Game,
  `warp 8/0`, start, cross; down, cross for Sound).

## As built

- `runtime/psx/present/outline_text` owns it. A title's `GameRuntime::outlineFonts(core)`, asked as each frame seals,
  names up to four fonts (`kMaxOutlineFonts`); a font may follow the guest's own tables, and the atlas is laid out
  again whenever a font's page or glyph rectangles change. An `OutlineFont` is a texture page and its glyphs, each an `OutlineGlyph`: the rectangle
  (u, v, width, height) its text drawer samples, u running on across the pages right of the font's page, with its
  codepoint and face (the font's text face, its symbol face or PromptFont) or its PSX button; `OutlineGrid` fills
  them for a font of equal cells. Also per font: the text face and its weight, the shadow's CLUT indices and offset.
  `outlineGlyphs(core)` names, for each ordering-table packet its text drawers filled with glyphs, the font it drew
  from (`OutlineGlyphs::glyphFont`). At seal, `markGlyphs` tags those textured primitives with their font
  (`DrawPrimitive::outlineFont`); a replayed in-between carries the same tags through `Capture::glyphPackets`.
- A frame that drew tagged packets brings each drawn font up to date in the atlas for the glyphs drawn so far: the
  glyphs whose rectangles a tagged primitive's texels overlap (`markGlyphs`, `record_geometry` `texelBox`; every
  glyph of the font through a texture window) join the font's drawn set, which lasts until the atlas is laid out
  again. Each drawn glyph's bitmap is read from VRAM, and one whose bitmap or codepoint changed gets a new field: the
  codepoint's outline (the title's face, or PromptFont for a button glyph, whose codepoint the pad in hand picks
  through `GlyphSet`) fitted to the box of the glyph's ink texels, as an MSDF in RGB, and in A the median of that
  field moved by the shadow offset, four field pixels a texel at any internal scale; only the ink box, its shadow and
  the field range round them are generated, the rest of the tile is outside. A glyph never drawn has no field and
  costs nothing; a `proportional` font keeps the outline's proportions at the ink box's height instead, centred across it and
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
- Cost (2026-11-14, `perf-units`): Tomba 2's narration (New Game, `PSXPORT_NO_FMV=1`) first draws text at frame 716;
  the atlas build there was 12.9 G instructions for 135 fields at eight pixels a texel, past the 3 s watchdog. It is
  now 0.28 G for the 19 glyphs drawn (present phase max 277 M against a 59 M p50 frame and the 510 M load-frame max),
  33 fields across the whole route, and the run passes under the default watchdog. A glyph's first frame still pays
  msdfgen's per-pixel cost (about 8 k instructions a field pixel); baking the fields from the committed fonts at build
  time would remove it but needs fields independent of the VRAM ink box, fitted per glyph at sample time.
- Check: Spyro 2 Summer Forest pause and Spyro 3 Sunrise Spring pause and Help at `ires=0`, 3840x2160
  (`warp 0/0/0`, start, down, cross): letters and button glyphs sharp, shaded and shadowed as the bitmap; Help
  under `gamepad make xbox` and `gamepad make playstation` for the face colours. Tomba 2's Options screen (New Game,
  `warp 8/0`, start, cross) under both makes for its `padArt` buttons.
