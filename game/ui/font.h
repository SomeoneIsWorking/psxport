// class Font — PC-native engine FONT / TEXT init subsystem.
//
// PROPER OOP: one instance per Core, owned by Engine (`c->engine.font`). Callers reach the
// init entry as `c->engine.font.init()`. Back-pointer `core` wired at Core construction time
// (same pattern as ScreenFade / GraphicsBind / etc.).
//
// SCOPE: the FUN_80075130 boot-time font/text init orchestrator + its three engine-state
// leaves it directly owns (font-bank select, font-bank2 store, glyph-class table fill). The
// libgpu/sound leaves (draw-env / FntLoad / FntOpen setup, sound bring-up) stay dispatched
// through rec_dispatch in-context — see engine_font.cpp for the full call chain.
//
// Was the free functions ov_font_init / ov_font_bank_select / ov_font_bank2_store /
// ov_font_glyphclass_fill in engine_font.cpp — each taking its argument via MIPS taxi
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
};
