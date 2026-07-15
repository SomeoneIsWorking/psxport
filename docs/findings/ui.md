# Findings — UI subsystem (game/ui/*)

## DRAFTED (UNWIRED/UNVERIFIED): dialog/text-box byte-stream advance — 0x8007C0D0 + 0x8007D0D0 (2026-07-08)

- **Task**: wide-RE pass over 0x8007C000-0x8007FDB0 (docs/engine_re.md's "Wide-RE survey" flagged
  this range as the dialog/text-box renderer + pause/quit-menu widget builder). Focus this session:
  the dialog-box byte-stream cursor advance, the smallest fully self-contained piece.
- **RE**: `FUN_8007C0D0(obj, mode)` walks the dialog script byte at `obj+0x14` (the cursor): bytes
  `<0xC0` are glyphs, `0xC0-0xD0` and the plain-glyph case both arm a render-mode timer via
  `FUN_8007D0D0` when `mode==1` (a "committing" pass) and are no-ops on a `mode==0` "peek" pass;
  `0xD1-0xF7` are unhandled (skip); `0xF8/0xF9` are per-subtype "mode-marker" bytes (dispatched via
  a 6-entry table keyed by `obj+3`, unconditionally regardless of `mode`); `0xFC` commits a fixed
  countdown (`obj+0x40=0xF`) advancing the cursor WITHOUT bumping the column counter (`obj+0x2A`,
  unlike every other commit path); `0xFF` terminates the box (`obj+5=2`, `obj+0x42=0xFFFF`, return 0).
  `FUN_8007D0D0(obj)` sets the render-mode timer (`obj+0x40`) from `obj+3` (subtype) crossed with
  `DAT_800bf8a3` (text-speed/language byte): subtype 2-5 -> fixed 1; subtype 0/1 -> 3/2/1 keyed by
  the language byte; subtype >=6 -> no-op.
- **FINDING (recompiler limitation, keep this — avoids re-deriving)**: `gen_func_8007C0D0`'s
  0xF8/0xF9 handling reads a 6-entry pointer table at `0x80016EBC` (indexed by `obj+3`) and calls
  `rec_dispatch(c, addr); return;` UNCONDITIONALLY with no frame unwind — which LOOKS like a genuine
  indirect call to a separate function. It is NOT: the sibling `gen_func_8007D0D0` has the exact same
  shape (a 6-entry table for its own subtype switch), and there the recompiler DID statically resolve
  2 of its 6 entries to LOCAL `goto`s within the same function body (`case 0x8007D100u: goto
  L_8007D100;` / `case 0x8007D13Cu: goto L_8007D13C;`), proving the table holds in-function case
  labels, not separate callable functions. Ghidra's high-level decompile of `FUN_8007C0D0`
  independently shows the same 6 per-subtype effects as a plain switch with no call at all —
  corroborating evidence. Root cause: a MIPS jump-table-compiled switch is indistinguishable from a
  genuine computed call at the raw-instruction level the recompiler works from; it only recognizes
  "local" when a table entry's address happens to also appear as an in-function branch target
  elsewhere, which it does for only 2 of `FUN_8007D0D0`'s 6 entries and (apparently) none of
  `FUN_8007C0D0`'s. **Consequence for future wiring**: do not `rec_dispatch` this specific table read
  — there is no real registered function at those raw addresses to land on. Reproduce the per-subtype
  effect directly (done in the draft below). Watch for the same shape (opaque `rec_dispatch` right
  after an `obj+3 < N` bounds check reading a small aligned table) elsewhere in this 0x8007xxxx
  region — it is very likely more of the same local-jump-table pattern, not real dispatch.
- **Draft**: `game/ui/dialog_text_stream.{h,cpp}` — `DialogTextStream::advanceByte` (FUN_8007C0D0,
  guest frame MIRRORED per `gen_func_8007C0D0`'s `sp-=32` / s0,s1,ra spill) and
  `DialogTextStream::applyRenderMode` (FUN_8007D0D0, LEAF, no frame). Cross-checked line-for-line
  against `generated/shard_6.c:gen_func_8007C0D0` and `generated/shard_1.c:gen_func_8007D0D0`.
  **NOT registered** in any override table (no `shard_set_override`, no `EngineOverrides` entry, no
  SBS run) — per wide-RE-tier rules (docs/fleet-workflow.md §6), and per §9, MUST get a line-by-line
  re-verify pass before wiring (wide-RE drafts routinely hide bugs even after self-check).
- **NOT drafted, mapped only**: `FUN_8007D14C` (line-restart: snapshots `obj+0x10=obj+0x14`, calls
  `advanceByte(mode=1)`, sets bits on `obj+0x46` from the return value), `FUN_8007D208` (box
  position/size layout — very large constant-heavy function computing `obj+0x54/0x56/0x58/0x5A`
  from `obj+3` subtype + `obj+0x18`/`obj+0xE` + a measured-text-size pair from `FUN_8007CEEC`; too
  many magic layout constants to draft confidently in one pass), `FUN_8007D594` (the BOX'S OWN
  top-level state machine — switches on `obj+5`, states 0-10ish, calls `advanceByte`/`FUN_8007C940`/
  `FUN_8007D14C`/`FUN_8007CDD4` — this is the right next piece to RE once `advanceByte` is verified),
  `FUN_8007C940` (the actual GLYPH-BLIT byte-stream walker: builds draw-list quads via
  `FUN_8007C2A4`/`FUN_8007C3CC`/`FUN_8007C780`/`FUN_8007C8C8`/`FUN_8007C5E4`/`FUN_8007C620`/
  `FUN_8007C654`/`FUN_8007C688` — a distinct, large "glyph layout" cluster from the byte-stream
  cursor advance, worth its own dedicated wide-RE pass).

## MAPPED (not drafted): pause/quit-menu + options-screen widget builders — 0x8007EAE4-0x8007FDB0

Confirmed the existing survey's identification (docs/engine_re.md, "Surveyed, NOT drafted" section)
and extended the family list. All of these are static SCREEN BUILDERS: each measures + lays out a
fixed set of text-button strings via a shared trio of helpers, then returns (no per-frame state of
its own beyond the shared cursor-index global `DAT_800bf808`):

- **Shared helpers** (not independently drafted; signatures RE'd from `scratch/decomp/ui_8007.c`):
  `FUN_800793C4(char** strings, int count, u16* outWidths)` — measures a batch of strings, returns
  the max width; `FUN_80079374(x, y, flags, char* string, ...)` — layout+draw one text button (flags
  bit encodes highlight/selected state, keyed off `DAT_800bf808 == index`); `FUN_8007E998(x, y,
  wide?)` / `FUN_8007E1B8` / `FUN_8007E6DC` — cursor-highlight quad emit (the actual draw-list
  entries); `FUN_80078988` / `FUN_80079324` — single fixed-string draw (headers/footers, no
  measure-then-lay-out step).
- **Pause menu** — `FUN_8007EAE4()`: builds "Options"/"Load data"/"Quit game"
  (`PTR_s_Options_800a2854` etc, confirms the existing survey's ID) as a 3-item vertical list at a
  computed origin, plus a fixed background/border draw-list (the `DAT_800a58e8`-indexed 9-entry loop
  at the end, indices into `PTR_DAT_80017334` — a sprite/quad template table, not independently
  RE'd).
- **Confirm dialogs** — `FUN_8007EE74()`: "Continue"/"Load data"/"Quit game" (in-level pause variant
  with a Continue option); `FUN_8007EF60()`: "OK to quit game?" Yes/No confirm; `FUN_8007ED5C()`:
  "Save" + a 2-item Yes/No-shaped list (save-confirm). All three share the exact same
  measure-then-layout-then-highlight shape as the pause menu, parameterized only by their string
  tables and item count.
- **Options screens** (the "Select Options" -> Messages/Sound/Screen adjust/Controls page and its
  children) — `FUN_8007F104()`: the top-level 4-item Options list; `FUN_8007F250()`: Messages page
  (Speed/Voices sliders + a 3-way numbered choice + more below, truncated in this pass);
  `FUN_8007F498()`, `FUN_8007F73C()`, `FUN_8007F8F8()`, `FUN_8007FC24()`: further options sub-pages
  (Sound / Screen adjust / Controls, by position in the family — not individually confirmed by
  string content this session); `FUN_8007F078()`: a shared "Return"/"Exit" footer draw used by (at
  least) the options pages.
- **Note**: `game/ui/menu.cpp` already REPLACES the in-game Options SUBMENU controller
  (`FUN_8007B45C`, the page-3 handler that would otherwise call into `FUN_8007F104`'s family) with a
  PC-native overlay — so `FUN_8007F104`/`f250`/`f498`/`f73c`/`f8f8`/`fc24` are effectively DEAD when
  that overlay is active, but still reachable (headless / windowless fallback) and still unowned.
  The pause menu itself (`FUN_8007EAE4`, page 1) and its confirm-dialog children are NOT replaced by
  anything — genuinely live, unowned code.
- **Next step**: draft a `PauseMenu` widget-builder class starting from `FUN_8007EAE4` (smallest,
  most self-contained, already string-confirmed) once `FUN_80079374`/`FUN_800793C4`/`FUN_8007E998`
  are RE'd precisely enough to reproduce their draw-list output — not attempted this session (risk
  of drafting against a misunderstood shared-helper contract outweighed the value of an untested
  draft; RE the 3 shared helpers FIRST in a follow-up pass).

## Dialog text-box PANEL emitter chain — RE'd (2026-07-14, for bug #34)

- **symptom driving the RE:** pc_render's 2D-only field OT walk drops the dialog PANEL (dark
  semi-transparent box behind text) while keeping the glyphs — the panel fill is POLYGONS (FT4) and
  the twoDOnly poly branch (runtime/recomp/gpu_native.cpp:885) drops all non-billboard polys
  categorically; the panel's border/corner SPRITES survive via the sprite branch's layer rule.
- **emitter chain (from generated/ shards, byte-verified against the f1413 packet capture):**
  `FUN_8007DA50` (dialog-box object per-frame update; phase>=4) → `FUN_8007D594` (box text SM,
  ~13 states, NO packet writes in the states) → shared tail L_8007DA1C unconditionally calls
  `FUN_8007CC00` (border tiles, op 0x65, walks the glyph-position table `FUN_8007C940` builds) then
  `FUN_8005019C` (4 corner sprites — TL/TR/BL/BR, SPRT_8 8×8, op 0x74/0x76, u=184/200/232/248 v=136;
  CORRECTED 2026-07-15 from "2 corner sprites"; port spec docs/native-render-2d-panel.md — then 5× `FUN_8004FFB4` = the FT4 fill quads,
  op 0x2C/0x2E|1 → the observed 0x2F). All OT bucket 2. Glyphs (op 0x7C, `FUN_80078CA8` =
  native Font::glyphEmit) come from a DIFFERENT path (FUN_8007C940's control-code handlers, called
  earlier from FUN_8007DA50) — so a PktSpanSession wrap of `FUN_8007D594` tags EXACTLY the panel
  packets (span-clean, per-frame re-emit in steady state). Dispatch thunk exists:
  generated/shard_disp.c func_8007D594.
- **correction to docs/engine_re.md wide-RE survey:** the panel packets do NOT come from
  `FUN_8007C940` (it only builds the intermediate glyph-position list that FUN_8007CC00's border
  loop consumes); the emitters are FUN_8007D594's tail calls above.
- **ownership status:** none of 0x8007DA50/0x8007D594/0x8007CC00/0x8005019C/0x8004FFB4 ported yet.
- **fix plan (#34):** guest-transparent observer wrap on 0x8007D594 (oracle-pure like
  render_observer.cpp obs_body) → register the emitted span in a per-frame UI-span registry →
  twoDOnly poly branch keeps UI-tagged spans as RQ_HUD/RQ_OM_2D_FG.
- **refs:** generated/shard_5.c:13091 (8007DA50), shard_4.c:12027 (8007D594) + :11855 (8007CC00),
  shard_3.c:12991 (8005019C), shard_2.c:6001 (8004FFB4); scratch/logs/bug44_ot_decode.log (packet
  capture); game/ui/dialog_text_stream.{h,cpp} (adjacent RE'd text cluster, distinct).
- **FIX LANDED (2026-07-14, same day):** UI-span registry on GpuState (ui_span_add/lookup, per-frame
  reset, presence-only — no depth, no fps60 stamp) + guest-transparent observer wrap on 0x8007D594
  (render_observer.cpp obs_8007D594, oracle-pure via the c->game->oracle gate) + third keep-category
  in the twoDOnly poly branch (gpu_native.cpp ~900: UI-tagged polys → RQ_HUD/RQ_OM_2D_FG). NB the
  wrap installs via raw shard_set_override like its obs_body siblings, NOT engine_set_override_main —
  the thunk's oracle branch keys on psx_fallback, which standalone GATE=1 also sets, and the wrap
  must fire under GATE (verified: panel missing under the thunk install, ovhit native=0). Verified:
  panel renders GATE+pc_render f1413 + default f1401 (bug34fix_*.png); free-roam no world-poly leak;
  SBS-full autonav 0-diff f67860. Signpost dialog (row 9) uses the same emitter chain — expected
  fixed, awaiting user eyeball.
