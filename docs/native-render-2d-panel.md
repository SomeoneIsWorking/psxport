# Native 2D UI panel family — port specs (RE'd 2026-07-15, map-first)

The dialog/menu/HUD box PANEL emitter chain. Three UNOWNED builders (codemap-confirmed) → one native
owner `game/ui/panel.cpp` with `panelBuild` / `panelFill` / `borderTiles` + a shared `decodeAttr()`.
Shared leaf `FUN_80083DE0` (DR_MODE/tpage packet header) is LIVE-owned (`game/render/wide_re_libgpu_leaves.cpp:279`
`func_80083DE0`) — do NOT re-derive; the native path just supplies the tpage to the emitted quads.

Native emit target: `RenderQueue::push2dQuad(...)` (opaque) / `emitOrQueue(...)` (semi), layer `RQ_HUD`,
order `RQ_OM_2D_FG`. Coord rules: xs/ys = PSX draw coords + `s_off_x/s_off_y`; us/vs = texel within
texpage; tp_x=(tpage&0xF)*64, tp_y=((tpage>>4)&1)*256, mode=(tpage>>7)&3; clut_x=(clut&0x3F)*16,
clut_y=(clut>>6)&0x1FF. Under pc_faithful/SBS the substrate still runs these builders' guest writes
(byte-exact); the native path is the read-only picture overlay (wiring model TBD by the title plan).

## Shared attr bit-decode (FT4 fill + corner sprite + border tile use it identically)
For 8-bit `attr`:
- `0x80` → semi-transparent (prim code `|0x02`; use emitOrQueue not push2dQuad).
- `0x40` → CLUT x-nibble 0x3F (VRAM x=1008) else 0x3E (x=992).
- `0x20` set → RGB=(0x40,0x40,0x40), color-modulated (panel darkening). Clear → prim code `|0x01` (raw texel, RGB ignored).
- `0x1F` → CLUT y = (attr&0x1F)+0x1F0 (496..527).
- ⇒ `clut = (((attr&0x1F)+0x1F0)<<6) | ((attr&0x40)?0x3F:0x3E)`.

## Spec 1 — FUN_8005019C `panelBuild(short* rect{x,y,w,h} r4, u16 style r5, u8 shadow r6, int otBucket r7)`
9-slice textured panel. Callers: FUN_8007D594/FUN_8007DC38 (dialog, bucket 2), FUN_800738B0/FUN_800737F8
(HUD/menu), FUN_8004FB4C (bucket 3). ABI: frame 64; spills r16→sp+32,r17→36,r18→40,r19→44,r20→48,
r21→52,r31→56; MIRROR the frame. attr: `if(style&0x40) attr=style+0x0D; else { attr=style+6; if(shadow) attr|=0x80; }`.
Emits (all tpage 0x5F → tp_x=960,tp_y=256,mode=0; clut per decode; opaque unless attr&0x80):
1. 4 corner SPRT_8 8×8, v=136: TL pos(x-8,y-8) u=184 · TR pos(x+w,y-8) u=200 · BL pos(x-8,y+h) u=232 · BR pos(x+w,y+h) u=248.
2. DR_MODE tpage 0x5F (owned leaf — no packet natively).
3. 5× panelFill(rect', uvIndex, attr, bucket): 0 top(x,y-8,w,8) · 1 bottom(x,y+h,w,8) · 2 left(x-8,y,8,h) · 3 right(x+w,y,8,h) · 4 center(x,y,w,h).

## Spec 2 — FUN_8004FFB4 `panelFill(short* rect r4, int uvIndex r5, u16 attr r6, int otBucket r7)`
One POLY_FT4 over rect, sampling a fixed patch. LEAF, frame 0 (no mirror). code 0x2C `|0x02`(semi) `|0x01`(raw if !attr&0x20 else RGB 0x40).
tpage 0x5F. Verts = rect corners v0(x,y) v1(x+w,y) v2(x,y+h) v3(x+w,y+h). UV by uvIndex (v0=uL,vT / v1=uR,vT / v2=uL,vB / v3=uR,vB):
| idx | uL | uR | vT | vB |
|---|---|---|---|---|
|0 top|192|200|136|144|
|1 bottom|240|248|136|144|
|2 left|208|216|137|143|
|3 right|224|232|137|143|
|4 center|216|223|136|143|

## Spec 3 — FUN_8007CC00 `borderTiles(DialogBox* box r4)` — ✅ OWNED via tap (`Panel::pushDialogGlyphs`)
NOTE: this emitter draws the per-glyph textured SPRTs (op 0x65, font atlas tpage 0x1F) — i.e. the visible
dialog TEXT. Owned 2026-07-16 as a substrate-mirror tap in game/ui/panel.cpp (gen body byte-exact + host
push from the box pointer), INCLUDING the highlight path (box+0x47==1 && box+3==1 → CLUT 0x7CBE pinned
for the row) that the interim flat-list producer `Render::dialogTextNative` could not see — that producer
is retired. Confirmed field widths from the live decompile: y@2 is UNSIGNED u8 (emitter reads
`(ushort)pbVar4[-1]`); CLUT uses `char & 0x7f`. Below is the original RE spec:

Per-glyph text-background SPRT (op 0x65). ABI: frame 32; r31→sp+28,r16→sp+24; mirror. Reads box+0x47 & box+3
(both==1 → highlight palette 0x12/clut 0x7CBE). Glyph list @0x800ECB88, 8B/entry: x@0(u16) y@2(u8) char@3(u8;
0x80=double-width) u@4(u8) v@6(u8); count=(s16)*0x1F80017E (built by FUN_8007C940 — must be readable). Per glyph:
SPRT tpage 0x1F (tp_x=960,tp_y=256,mode=0), pos(x,y), w=(char&0x80?16:8), h=16, uv=(u,v),
clut=((char+0x1F0)<<6)|0x3F when char≠0x12. DEP: needs FUN_8007C940's glyph list owned/readable.

## Title/menu box-quad emitter — FUN_8007e1b8 family (RE'd 2026-07-15, separate from the 9-slice panel)

The DEMO/title menu box + cursor-highlight (the 3 op-0x2C/0x2D FT4 quads the otattr attributed to
`0x8007E2F8`/`0x8007E620`). DEFINITIVE: the menu TEXT ("New Game"/"Load Game") is the native Font path
(`Font::drawText`); this family draws ONLY box/cursor quads. `0x8007E2F8`/`0x8007E620` are jump-table
labels INSIDE `FUN_8007e1b8` (the FT4 emitter, frame 0), reached via `@0x8001728c`. `FUN_8007e6dc` = SPRT
variant. `FUN_8007e998(x,y,bucket=1) = FUN_8007e8dc(x,y,1,templIdx=0x98)` = the cursor-highlight quad.

DATA-DRIVEN — a read-only `titleMenuNative()` reads the SAME guest tables (do not hardcode texels):
- `PTR_DAT_80017334[idx]` (u32 selector) · template DATA base `PTR_DAT_800ecf58`. selector word at
  `base+sel*4`: `count=*(s16*)(base+sel*4)`, `dataPtr=base+*(u16*)(base+sel*4+2)`; emits `count` quads,
  stride 0x10 entries at dataPtr.
- 16-byte template entry: +0x00 u32=(CLUT<<16)|u0v0 · +0x04 u32=(TPAGE<<16)|u1v1 · +0x08 u16 u2v2 ·
  +0x0A u8 W, +0x0B u8 H · +0x0C u16 u3v3 · +0x0E s8 xoff, +0x0F s8 yoff.
- geometry (op-class 0, axis-aligned): W=wOverride>0?wOverride:(wOverride==0?templW:templW+wOverride);
  H likewise; vx=x+xoff, vy=y+yoff; v0(vx,vy) v1(vx+W,vy) v2(vx,vy+H) v3(vx+W,vy+H).
- code 0x2C; |0x02 if desc_u16@2&0x8000 (semi); |0x01 if (descByte0&0xF0)==0 (RAW, RGB ignored) else RGB=descByte0.
  clut = (desc_u16@2&0x7FFF) ? that : template entry+0x00 high half. tpage = entry+0x04 high half.
  Title's 3 quads = descByte0=0, flags=0 → code 0x2D raw opaque, baked CLUT.
- cursor: templIdx 0x98 at (xSel,ySel) derived from selected-index = **sm[0x68]** (sm=*(u32*)0x1F800138)
  for the title s2 (pause menu uses DAT_800bf808). Row pitch/base: verify on a live title dump (the exact
  xSel/ySel caller lives in the DEMO overlay 0x80106xxx, not traced — pattern from pause-menu FUN_8007eae4).
- emit: push2dQuad(RQ_OVERLAY, order_2d_fg=1, …) opaque / emitOrQueue(semi). tp_x=(tpage&0xF)*64,
  tp_y=((tpage>>4)&1)*256, mode=(tpage>>7)&3; clut_x=(clut&0x3F)*16, clut_y=(clut>>6)&0x1FF.
- ABI: FUN_8007e1b8 frame 0 (no mirror needed for a read-only producer). Emitter is unowned; no game/ui draft.

## Notes
- These are geometry builders (rect/list → quads); the reusable native primitive is `uiEmitFT4(rect,uvIndex,attr,tp)`
  / `uiEmitSprite8(pos,uv,attr,tp)`. WIRING (who calls panelBuild in pc_render — a native UI pass vs override)
  is decided by the title/2D ownership plan (docs/native-render-rebuild.md #2).
- Skin-texture atlas at VRAM (960,256) — eyeball-verify once drawn; texel u/v above are byte-exact from packets.
