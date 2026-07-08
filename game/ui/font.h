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
};
