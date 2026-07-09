// class Font — PC-native engine FONT / TEXT init subsystem.
//
// PROPER OOP: one instance per Core, owned by Engine (`c->engine.font`). Callers reach the
// init entry as `c->engine.font.init()`. Back-pointer `core` wired at Core construction time
// (same pattern as ScreenFade / GraphicsBind / etc.).
//
// SCOPE: the FUN_80075130 boot-time font/text init orchestrator + its three engine-state
// leaves it directly owns (font-bank select, font-bank2 store, glyph-class table fill). The
// libgpu/sound leaves (draw-env / FntLoad / FntOpen setup, sound bring-up) stay dispatched
// through rec_dispatch in-context — see font.cpp for the full call chain.
//
// Was the free functions ov_font_init / ov_font_bank_select / ov_font_bank2_store /
// ov_font_glyphclass_fill in font.cpp — each taking its argument via MIPS taxi
// parameter c->r[4]. Now real instance methods with explicit typed arguments.
#pragma once
#include <stdint.h>
class Core;

class Font {
public:
  Core* core = nullptr;

  // init: FUN_80075130 — font / text system init orchestrator (no args, no return). Called
  //   once from native_boot.cpp's game_init prefix. Owns the direct engine-state writes +
  //   the 3 engine leaves; rec_dispatches the 8 libgpu/sound leaves in-order, in-context.
  //   The stack-struct dance (sp -= 48, sp+16..sp+26 populated for the FntOpen dispatches)
  //   is preserved verbatim — the dispatched callees read that struct.
  void init();

  // bankSelect(bank): FUN_800963a0 — font-bank selector. If ((bank-1)&0xff) < 24, stores the
  //   bank byte at 0x80105cec and returns the sign-extended low byte; otherwise returns -1.
  //   Returned via v0 (c->r[2]) so retained-PSX callers still work.
  void bankSelect(uint32_t bank);

  // bank2Store(bank): FUN_80096370 — font-bank2 store (single sb to 0x80105d28). Leaf; the
  //   recomp body did not set v0, so this doesn't either.
  void bank2Store(uint32_t bank);

  // glyphClassFill(cls): FUN_800752b4 — glyph-class table fill. Writes an exclusive cascade
  //   (i>=24-cls -> 4; i>=16-cls -> 1; i>=12-cls -> 3; i>=8-cls -> 2; else 0) into 24 entries
  //   at 0x800be238 + i*12 + 8 (u8). Returns the loop-exit count in v0 (caller ignores).
  void glyphClassFill(int32_t cls);

  // measureLineWidth(str): FUN_80073750 — string measurer. Walks a NUL-terminated guest C-string
  //   counting a "prefix" length (chars before the FIRST '\n') and a "suffix" length (the first
  //   '\n' itself + every char after it, cumulative — NOT reset on any later '\n'). If no '\n' was
  //   ever seen, returns the plain length (prefix count) as a POSITIVE value. If a '\n' was seen,
  //   returns -(max(prefix, suffix)) — a NEGATIVE value the caller (e.g. game/ai/
  //   beh_cube_text_spawn.cpp's "cube letters" text actor) uses as a multi-line signal.
  //   ABI NOTE: the guest body's loop cursor IS its a0 parameter (`addiu a0,a0,1` in the loop),
  //   so on return a0 == the address of the string's NUL terminator — a register LEFTOVER the
  //   caller relies on for its next call's implicit a0 (beh_cube_text_spawn.cpp's "GOTCHA" comment,
  //   FUN_8007aae8 carries a0 through unset). This native version reproduces that leftover by
  //   writing c->r[4] = end-of-string, exactly matching what a dispatched call would leave.
  //   Body from disas 0x80073750..0x80073798 (no sub-calls; a plain byte scan).
  static int32_t measureLineWidth(Core* c, uint32_t strAddr);

  // drawText(x, y, w, str, color): FUN_80079374 — WIDE-RE TIER DRAFT (2026-07-09), UNWIRED /
  //   UNVERIFIED (docs/fleet-workflow.md §6/§9 — no override registration, no SBS run; a faithful
  //   hand-transcription that must be diffed line-by-line against the gen body again before it is
  //   trusted/wired). One of the two hottest unowned leaves in the game (~4235 dispatches / 600
  //   frames of free-roam). A thin arg-packing wrapper around the still-unowned font/glyph emitter
  //   FUN_80078CA8 (see docs/engine_re.md "FUN_80078ca8" — full string-draw engine with cursor
  //   state at scratchpad 0x1F800000..0x1F80001F; NOT ported here, only reached via rec_dispatch,
  //   same as font.cpp's other KEPT libgpu/sound callees).
  //   Disas 0x80079374..0x800793C0 (tools/disas.py 0x80079374 --all 40) / gen_func_80079374
  //   (generated/shard_7.c:11490):
  //     a0' = (int16)x | (y << 16)              — packed {y:hi16, x:lo16, x sign-extended} vertex
  //     a1' = 0x00100008                          — CONSTANT arg (original a1/w is DISCARDED — the
  //                                                  guest code never uses the caller's w for this
  //                                                  slot; confidence: CONFIRMED from the gen body,
  //                                                  the incoming a1 is entirely overwritten before
  //                                                  use — semantic ROLE of 0x100008 unconfirmed,
  //                                                  likely a packed 16/8 draw-attribute pair)
  //     a2' = (int16)w | (h << 16)                — packed {h:hi16, w:lo16, w sign-extended}
  //     a3' = str (unchanged)
  //     stack[16] = color (5th arg, passed on the caller's stack at +48)
  //     *0x1F800180 (u16) = 32                    — scratchpad write BEFORE the call (role
  //                                                  unconfirmed; a fixed constant, not derived
  //                                                  from any argument)
  //     tail-calls FUN_80078CA8(a0', a1', a2', a3', color) via rec_dispatch, ra = 0x800793B4
  //   x/y/w/h are taken as raw guest register values (already packed by the CALLER into low16 of
  //   each 32-bit arg — this wrapper does NOT decode them into separate ints, it reproduces the
  //   exact bit operations the guest performs, since sign-extension order matters for negative
  //   on-screen coordinates).
  static void drawText(Core* c, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t str, uint32_t color);
};
