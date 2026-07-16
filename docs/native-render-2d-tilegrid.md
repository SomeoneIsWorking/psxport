# Field 2D tile-grid layer — RE of the op-0x7C sprite drawer (0x8011534C/0x80115598)

Promoted from scratch/decomp/overlay_2d/801401B8.md (RE agent, 2026-07-16). The op-0x7C "60 field
sprites" (native-render-rebuild #3b Track A group 2) are ONE scrolling 16x16 tile-grid background
layer, emitted by the unowned overlay pair below (dispatch case 0x8003D1C4 of
Render::overlayTypeDispatch). NOTE: 0x80083DE0 (the OT/DR_MODE helper this leaf jal's) is ALREADY
owned — wide_re_libgpu_leaves.cpp:279 — do not re-derive.

# RE scout: 0x801401B8 (entityLoop) vs 0x80115364/0x80115598 (tile-grid sprite drawer)

Live-dump method: `PSXPORT_AUTO_SKIP=1 PSXPORT_REPL=1` headless run to frame 200 (free-roam GAME
stage), `dumpram scratch/decomp/overlay_2d/freeroam.ram`, then `tools/disas.py --ram <dump> <addr>
--all N` (this tool already supports raw-RAM disassembly via `psexe.load_ram`, no new tooling
needed). Raw listings: `801401B8.disas.txt`, `80115300.disas.txt` (covers 0x8011534C..80115594),
`80115598.disas.txt`.

## (a) 0x801401B8 boundary + verdict: ALREADY OWNED, and NOT a sprite drawer

`tools/codemap.py --addr 801401B8` returns `OverlayGroundGt3Gt4::entityLoop` (LIVE,
`game/render/overlay_ground_gt3gt4.cpp:363`), wired via `ov_a00_set_override`, SBS-gated. The live
disassembly (`801401B8.disas.txt` lines 2-65, function ends at `80140298`/`801402b4` jr ra) matches
the existing port instruction-for-instruction:
- prologue `addiu sp,-40`, spills s0-s4/ra (matches the port's mirrored 40-byte frame).
- loads CAMERA_GTE_CTRL block (`0x1f8000f8`, 8 words) into GTE ctrl regs 0-7 via `cop2` ops at
  `8014020c-80140248` — matches `entityLoop`'s `gte_write_ctrl` loop.
- walks a `u16` index array at `list+16..list+16+count*2` (count = byte at `list+6`), each index
  scaled `<<2` into a table at `list+12` (`800ed8c8`-derived OT base at `800ed8c8`), then two `jal`s
  at `80140270`/`80140284` to `0x8013fb88` (gt3) and `0x8013fe58` (gt4) — exactly the port's
  `gt3(c)`/`gt4(c)` calls.

**This is a 3D GTE-projected triangle/quad (POLY_GT3/POLY_GT4) emitter, not a 2D op-0x7C sprite
drawer.** It does GTE `RTPT`/`NCLIP`/`AVSZ3`/`AVSZ4` and writes 40-byte (GT3, OT tag len 9) / 52-byte
(GT4, OT tag len 12) packets into the SAME packet-pool bump allocator (`0x800bf544` — same pointer
`OverlayGroundGt3Gt4` already documents as `PKT_POOL_PTR`). No `0x7C`/16x16-sprite command byte
appears anywhere in this function or its two callees. See the file's own extensive banner for the
full packet-field layout (RGB masks, uv2hi quirk, per-mode min/max Z-select) — already correct and
current; nothing to add.

The rest of the disassembled range (`80140544` onward) is a *different, unrelated* function
(prologue `addiu sp,-32` at `8014054c`, calls `0x800519e0`/`8004766c`/`80049674`/`80078230` etc.,
touches `0x1f8001a0-0x1f8001a6` = pad input state) — almost certainly an object/actor init routine
that happens to sit adjacent in the overlay's code region, unrelated to the sprite question. Not
pursued further (out of scope).

**Conclusion for docs/native-render-rebuild.md's Track A note:** the WWATCH attribution "store-pc
0x801401B8 ... 12.6k stores/90s, a direct leaf of the dispatch" for the op-0x7C sprite pool writes
is WRONG / stale — 0x801401B8 never touches an 0x7C command byte. The watch almost certainly
attributed a store elsewhere to the *containing function at the top of the observed call frame*
(entityLoop is the dispatch's case-0 leaf, so any store whose immediate call chain still had
entityLoop's frame live would misattribute this way) rather than the literal store PC. **The real
op-0x7C sprite drawer is 0x80115364/0x80115598 (dispatch case target 0x8003D1C4)** — see below. This
should be corrected in the doc so a future session doesn't re-open entityLoop looking for sprites.

## (b)-(g) — n/a for 0x801401B8 (see the note above); full RE for the real 2D drawer follows

---

# 0x80115364 / 0x80115598 — the real op-0x7C 16x16 tile-grid sprite drawer

## Function boundaries

- **0x8011534C-0x80115594**: one function, `addiu sp,-8` at entry (no register spills — leaf uses only
  `t`/`v`/`a` regs and 8 bytes of scratch at the new sp). Internally has TWO independent tails
  reached by branching on `*a3` (a state byte at the object node's offset 0): value==1 branches to
  `80115480` (a *second* clamp block using scratchpad `0x1f8000f0/f2`), values 0/other fall into the
  `80115368..8011547c` init block. **0x80115364 itself is mid-function** (a `beq`-target inside
  `8011534C`, not a real entry — the REAL leaf entry the dispatch table calls is `8011534C`; RE
  double-checked against `overlay_type_dispatch.cpp`'s literal `rec_dispatch(c, 0x80115364u)` — the
  live dump's function-start scan (back to the nearest `addiu sp,-N`) lands at `8011534C`, and
  `80115364` is simply the fall-through after an early `bne`/`beq` pair at the top, i.e. the same
  function, no separate prologue at `80115364`).
- **0x80115598-0x801158dc**: the real per-tile packet-emitter, `addiu sp,-80`, spills
  s0-s7/fp/ra (10 words) — a genuinely bigger, nested-loop function.
- **0x801158e0 onward**: a third function (`addiu sp,-48`, dispatches on `a0+4`'s state byte,
  reads a per-area table at `0x80146f0c`) — the caller/driver that owns the node and (by the state
  values 0/1/2/3 it switches on) likely re-inits vs. steps vs. draws the tile overlay across frames.
  Not fully RE'd (out of scope — noted for whoever ports this next).

## (b) What 0x8011534C/0x80115364 does — scroll-offset wrap helper

Reads a node (`a0`, aliased `a3`). If state byte `a3[0] == 1`, recomputes an X scroll wrap from
scratchpad `0x1f8000f0` (mult by node field `+0x2e`) and a Y wrap from `0x1f8000f2` (mult by
`+0x2c`), each folded into `[0, tileSize)` via a decrement/increment `bltz`/`slt` loop (classic
modulo-by-repeated-subtraction against the just-computed tile pitch `+0x30`/`+0x32`), then stores the
wrapped X/Y to node `+0x28`/`+0x2a`. If state byte is 0, instead does a ONE-TIME setup: copies a
12-byte source record (`t0 = *0x800ecf84`, a global pointer) field-by-field into the node (`+4..+11`
= 6 halfwords/2 bytes, `+0x2c/+0x2e` = two computed pitch shorts derived from a fixed-point multiply
against constants `0x38e38e39` and `0x08e8`-ish — a reciprocal-multiply divide-by-tile-width/height
idiom), sets `+0x38` (frame countdown?) and `+3`=1 (armed flag). **This is a scroll/animation-phase
computer for a tiled background layer, not a per-object drawer** — it feeds the position fields
`+0x28/+0x2a` that `0x80115598` reads back as the scroll offset.

## (c) 0x80115598 — the packet-pool tile-grid emitter (WHAT OBJECT STATE it reads)

Node fields consumed (`t4` = a0):
- `+16, +17` (u8, u8): grid width, grid height (tile counts) — `s0 = width<<1`, `s0*height` via
  `mult` at `801155dc` = flat tile count, used as a wrap bound for the column index loop.
- `+40, +42` (u16 each): scroll-wrapped X/Y (written by the helper above) — become the base screen
  position (`s5`,`a1` locals), offset `-160`/`-120` (half of 320x240 — screen-space centering).
- `+20` (u32, `lw a0,20(t4)`): pointer to a tile-ID table (one `u16` per tile, indexed
  `row*width+col`, read at `80115770`: `lhu a3,0(a0)`).
- `+6` (u16, `lhu a4,6(t4)` at 801157c8): a texture-atlas/CLUT selector base folded into the UV/CLUT
  computation.
- Global `0x800bf544` (`PKT_POOL_PTR`) — SAME bump-allocator pointer `OverlayGroundGt3Gt4` already
  owns for GT3/GT4; this leaf shares the pool, confirming one process-global packet pool feeds every
  type-drawer.
- Global `0x800ed8c8` (OT base) — same global entityLoop/gt3/gt4 use.

Two nested loops (outer over `s4`=row, `0x100`-scaled i.e. 16px * 16 rows fixed max; inner over
`t1`/`t8`=col, wrapping via the same decrement/increment idiom as the scroll-wrap helper) walk the
tile grid, reading one `u16` tile ID per cell (`a3` at `80115770`) and, PER TILE, emitting one 16-byte
sprite packet (see (d)) unless the row is past the visible bound (`slt v1,v1,t5` gate at `801157f0`,
`t5` = 352 = 320+32, a visible-column cutoff with 2-tile overscan). After the double loop, a
tail block (`80115854-801158d8`) patches the OT slot at a fixed index derived from node `+0` (masked
`&0xff00`) — `800ed8c8`'s table entry `[0x1ffc/4]` i.e. index 0x7ff (the LAST OT bucket, same
"background" convention seen elsewhere in this codebase) — into a 2-word chain-splice
(`jal 0x80083de0`, a link-list splice helper) rather than a per-tile OT insert: the WHOLE tile grid
is submitted as ONE static-depth OT entry (all tiles share one Z/OT slot — a background layer, drawn
once, not depth-sorted per tile). Then the pool pointer is written back to `0x800bf544`.

## (d) Packet layout — CONFIRMED op 0x7C, 16-byte stride

Stride: `a2 += 16` per tile (`801157fc`), pool base `s1` also `+= 16` (`80115788`) — **16-byte
packets**, matching the task's `0x800C5FF8 + 0x10*n` pool observation exactly. Per-tile packet
fields (offsets relative to the tile's own packet base):
- `+0` (u32): OT-link/tag word, written ONE ITERATION LATE (`sw v0,0(a2)` at loop-bottom `8011580c`,
  where `v0 = s1|0x0300` — `s1` is the NEXT tile's packet address, `0x0300` a packed
  length/flags nibble different from GT3/GT4's `<<24` convention — this leaf's own tag encoding).
- `+0x14` (20, u32, `sw a1,20(a2)`): base color/code word, template constant `0x7D808080`
  (RGB=0x808080 gray/white — untinted, texture supplies real color).
- `+0x17` (23, u8, `sb v0,23(a2)` where `v0=0x7c`): **overwrites the top byte of the word just
  written at +20 with 0x7C** — i.e. the FINAL command byte at packet offset +23 (top byte of the
  little-endian +20 word) is `0x7C` = **GPU command Sprite-16x16, textured, opaque** (or its
  semitransparency variant per the GPU command table). This two-step
  build-template-then-patch-command-byte is the mechanism: **CONFIRMS the task's op-0x7C 16x16
  sprite premise** (correctly, unlike 0x801401B8).
- `+0x13` (19, u8, const `3`): a fixed flag/size byte — likely padding or a semitransparency-rate
  selector shared by every tile (not object-state-dependent).
- `+0x18` (24, u32): screen-space XY word — X = `(t1 & 0xfff0) + scrollX`, Y = `t7<<16` (row
  position accumulator) — i.e. the on-screen 16-pixel-grid position of this tile, built from the
  wrapped scroll offset + the loop counters.
- `+0x1c` (28, u16, half only): packed UV pair — `u = (tileID<<4)&0xf0`, `v = ((tileID&0xf0)+8)<<8`
  — a **texture-atlas lookup by tile ID** (4-bit atlas column, byte-granularity U/V into a 16x16-cell
  texture sheet).
- `+0x1e` (30, u16): CLUT word — `((tileID & 0xf00) >> 2) + <node+6 base>` — CLUT bank select folded
  from the high nibble of the tile ID plus a per-node CLUT base.

So the record is a **texture-atlas tile ID → UV/CLUT lookup**, not raw per-tile UV data — the tile
IDs live in the table at node `+20`, and this function does the ID→atlas-cell math itself (no
external UV table read).

## (e) Per-frame dynamic vs static

- **Dynamic per frame**: the scroll offset (`+0x28/+0x2a`, recomputed every call by the
  `8011534C`/`80115364` helper from live scratchpad scroll registers `0x1f8000f0/f2` — this is what
  animates the tile layer, e.g. a scrolling water/lava/cloud background), and the resulting per-tile
  screen XY.
- **Static across frames (until state resets to 0)**: the tile-ID table pointer (`+20`), the
  grid dimensions (`+16/+17`), the tile pitch/reciprocal constants (`+0x2c/+0x2e`), the CLUT base
  (`+6`) — all set up ONCE by the state-0 branch of the helper (copied from the fixed global record
  at `*0x800ecf84`).
- **Always static**: command byte `0x7C`, base color `0x808080`, the `0x0300`/`3` template
  constants.

## (f) Native producer plan (dual-emit)

To port this as a `Render::` class method (paired guest-byte-exact + host-queue emit, matching the
`glyphEmit`/`panel.cpp`/GT3-GT4 convention already in the codebase):

1. **Args from dispatch**: this leaf is reached via `Render::overlayTypeDispatch`'s case
   `0x8003D1C4` — `rec_dispatch(c, 0x80115364u)` with `r[31]=0x8003D1CCu`. Per the dispatch
   contract (see `overlay_type_dispatch.cpp` banner), `a0` (r4) is whatever the CALLER of
   `overlayTypeDispatch` passed — same pass-through convention as entityLoop's `list=a0`. The node
   pointer IS `a0`.
2. **State reads** (host side, read-only): node `+16/+17` (grid W/H), `+20` (tile-ID table
   pointer), `+6` (CLUT base), `+28/+2a` (current scroll X/Y — ALREADY computed by the
   `8011534C` helper each call; the producer should call/mirror that helper first, or read its
   output, not recompute scroll independently).
3. **Per-sprite quad params** the host emit needs per visible tile: screen X/Y (from grid position +
   scroll wrap), a fixed 16x16 size, UV = atlas cell from tile ID (`(id<<4)&0xf0`,
   `((id&0xf0)+8)<<8` byte math — this is NOT a linear atlas index, RE the exact atlas geometry
   before porting so the host texture-atlas lookup matches pixel-for-pixel), CLUT = `((id&0xf00)>>2)
   + clutBase`, color = flat 0x808080 (untinted — texture supplies the picture).
4. **Guest packets**: byte-exact port must still write the same 16-byte packets (tag/color/xy/uv/clut)
   into the SAME `0x800bf544` pool and splice into OT bucket `0x7ff` via the `0x80083de0` helper
   exactly as gen does (this is the guest-byte-exact half of the dual-emit — SBS will catch any
   deviation).
5. **Not yet RE'd, needed before porting**: the `0x800158e0` driver function's 4-way state dispatch
   (0/1/2/3) that decides WHEN this emitter runs across frames, and the `0x80083de0` OT-splice
   helper's exact semantics (a shared primitive likely already used elsewhere — check
   `tools/codemap.py --addr 80083de0` before re-deriving it).

## (g) Shared-template verdict: 0x801401B8 vs 0x80115364/80115598 — DIFFERENT, not a shared template

| | 0x801401B8 (entityLoop) | 0x80115364/80115598 |
|---|---|---|
| Geometry | 3D, GTE-projected (RTPT/NCLIP/AVSZ) | 2D, plain integer screen-space math, no GTE |
| Packet family | POLY_GT3 (40B)/POLY_GT4 (52B), tag `len<<24` | fixed 16x16 SPRITE (16B), tag `X|0x0300` |
| Command byte | none (GT3/GT4 codes embedded per-vertex color word) | `0x7C` patched into a color-word template |
| Source data | per-object index list -> pointer table -> inline GT3/GT4 vertex records | per-tile ID table walked as a W x H grid |
| OT placement | per-record OTZ-sorted bucket (depth-based) | ONE fixed bucket (background layer, no depth sort) |
| Shared with pool | `0x800bf544` bump pointer, `0x800ed8c8` OT base | same `0x800bf544`, same `0x800ed8c8` |

The only shared infrastructure is the process-global packet-pool bump allocator and OT-base pointer
— every type-drawer in the 0x8003D0BC dispatch table funnels into that one pool, but each leaf has
its own bespoke packet shape, own math, and (for the GT3/GT4 pair) its own GTE usage. There is no
common "sprite-emit" template to factor out between these two; each of the ~20 dispatch leaves is
its own bespoke emitter, consistent with `overlay_type_dispatch.cpp`'s own note ("Each type-drawer
emits its object type's sprites INLINE — no shared sprite-emit leaf to tap").

---

# 2026-07-16 addendum — entry resolution, driver RE, native port (implemented)

Ported to `game/render/tile_grid_layer.{h,cpp}` (full RE trace + rationale in that .cpp's banner —
this section is a summary; treat the .cpp as the source of truth where the two disagree).

## Entry resolution (STEP 0) — 0x80115364 is dead for this feature; real entry is 0x8011534C

Ground-truth checked three ways (live disas of the resident overlay, the recompiler's own A00
function-splitter tables, and `generated/` cross-referencing):
- 0x80115364 is the delay-slot instruction (`addiu sp,sp,-8`) of a branch INSIDE 0x8011534C, not a
  separate function. `generated/ov_a00_disp.c` has real dispatch cases for 0x8011534C/0x80115598/
  0x801158E0 and **no case anywhere for 0x80115364** — `rec_dispatch(c, 0x80115364u)` while A00 is
  resident would fail-fast.
- `Render::overlayTypeDispatch`'s case 0x8003D1C4 really does contain that literal call
  (`generated/shard_7.c:4639`, confirmed) — a genuine ROM artifact — but this AREA_TYPE case is
  unrelated to the field's own AREA_TYPE (0): `Engine::areaModeDispatch`'s mode-idx-0 handler (the
  SAME 0x800BF870 selector byte) is the REAL, different, literal address 0x8011534Cu
  (`game/core/engine.cpp:2419`). This call path was never chased further — it doesn't matter for the
  field tile-grid, and per CLAUDE.md ("own it only if reachable") isn't worth force-fixing here.
- The REAL live callers (found by grepping for existing native call sites, not by walking the
  dispatch tree): `Engine::areaModeDispatch`/`areaModeDispatchFaithful` (mode idx 0 -> rec_dispatch
  0x8011534C, a0=0x800ED018, per-frame STEP) and the still-unowned `FUN_8003DF04` render-state
  dispatcher (state 0 -> rec_dispatch 0x80115598, a0=0x800ED018, per-frame EMIT) — both already
  independently referenced by the EXISTING native host-side producer `Render::backdropRender`
  (`game/render/render_walk.cpp`), which this session did not need to re-derive.

## Driver 0x801158E0 (STEP 1) — RE'd, but NOT the caller of the scroll/emit pair; not owned

4-way dispatch on node+4: ==1 -> 0x80115aa4 (not traced), ==0 -> an entity/particle SPAWN block
(gated on a global style byte @0x800BF9E0, walks a 28-entry area-record table @0x80146f0c, calls
`jal 0x8003116c` per matching record), ==2/3 -> 0x80115b68, >=4 -> 0x80115b70 (continues into an
unrelated per-frame animation-countdown block touching a DIFFERENT table @0x80147d84). Its body
contains **no call to 0x8011534C or 0x80115598 anywhere**, and grepping every generated shard (MAIN
+ A00's own) finds **no caller of 0x801158E0 at all** while A00 is resident. The original doc's
guess that this is "the driver that decides when the emitter runs" is wrong — it's a separate
object-type's per-frame state machine that happens to sit adjacent in the A00 code layout. Left
unowned/unwired; not part of this feature.

## Packet-pool tail — CORRECTED: 0x80083DE0 is not an OT-splice helper

Section (c)/(f) above guessed `jal 0x80083de0` performs the OT bucket link. Ground truth (cross-
checked against the leaf's own independent RE in `wide_re_libgpu_leaves.cpp`): 0x80083DE0 only fills
in a trailing packet's DR_TPAGE mode word + texture-window word (called here with rgbBitsSrc=0,
modeFlag=0, texWinSrc=0 -> a plain 0xE1000000 reset, no texwin). The REAL OT[0x7FF] splice is
separate inline code immediately around that call: every tile's own tag chains forward to the very
next tile's address (not "one iteration late" — each tile sets its own tag using the
already-bumped pool pointer); after the loop the LAST tile's stray forward pointer is patched to the
pre-existing OT[0x7FF] head; then a new trailing header packet is built (tag = first-tile address |
0x02000000) and OT[0x7FF] is overwritten to point at that header — i.e. one mode-reset packet +
the whole tile batch is prepended onto whatever was already queued in that bucket.

## Native port

`TileGridLayer::scrollStep` (FUN_8011534C) and `TileGridLayer::emit` (FUN_80115598), wired via
`engine_set_override_a00` (same mechanism as `OverlayGroundGt3Gt4`'s A00-local leaves), installed
from `runtime/recomp/boot.cpp`. Guest half reproduces every write (node fields, 16-byte op-0x7C
packets, OT splice, trailing DR_TPAGE packet via the already-RE'd but still-substrate 0x80083DE0,
called through `rec_dispatch` exactly like `Font::glyphEmit`'s own tail). Host half
(`TileGridLayer::emit`'s inline `push2dQuad(RQ_BACKGROUND, ...)`) is gated
`oracle || mRender->mode.psxRender()` — OFF during normal pc_render, since
`Render::backdropRender` already owns that picture unconditionally; this leaf's host push exists
only so the psx_render/oracle legs see a consistent picture. Row/col wrap-window math is reused
verbatim from `backdropRender` (already RE'd + battle-tested there) rather than re-derived.
`tileq` debug channel added (docs/config.md), rate-limited every 512th host push.

Verified live: `PSXPORT_DEBUG=tileq` under `PSXPORT_RENDER_PSX=1` shows the guest override firing
continuously during free-roam (>1000 emits/run); silent under default `pc_render` by design (the
host-half gate). Screenshots at `scratch/screenshots/tilegrid_pc.png` /
`tilegrid_ref.png` (pc_render vs psx_render, free-roam) show a consistent scrolling backdrop.
