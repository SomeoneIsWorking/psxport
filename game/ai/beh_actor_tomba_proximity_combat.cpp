// game/ai/beh_actor_tomba_proximity_combat.cpp — see the .h for the full state-machine writeup.
// Mechanical 1:1 transliteration of generated/shard_3.c:13494 gen_func_800527C8 (ground truth):
// register temporaries kept as c->r[N] scratch (same convention as game/ai/beh_lift_platform.cpp
// and siblings), s0 (obj, r22) and s1 (Tomba's fixed G-block, r23) promoted to named locals `self`/
// `G` for readability, control-flow kept as goto/labels matching the recompiler's own shape 1:1
// (per actor_melee_engage.cpp's precedent: "follows the recompiler's own control flow exactly
// rather than risking a mis-restructure under time pressure" — this function has ~30 conditional
// edges across 2 jump tables, high transcription risk for a manual restructure).
//
// VERIFIED (this pass): the body logic (every branch polarity, register value, field offset) diffs
// byte-for-byte identical to ground truth once cosmetic zero-representation (`c->r[0]+X` vs
// `(uint32_t)0+X`) is normalized out — NO logic bugs found. The one REAL bug was structural: the
// draft never reproduced the guest frame at all (ground truth descends -72, spills r16-r23+ra at
// c->r[29]+32..+64, restores on exit) — fixed below (see the prologue/epilogue comments). Wired via
// `overrides::install` with no setter (no shard_set_override/ov_a00_set_override dual-wire): no
// static `func_800527C8(c)` call site exists anywhere in generated/ (only the generic rec_dispatch
// switch-case, which the registry's dispatch always checks) — same "install with no setter is
// correct here" shape as game/player/actor_tomba.cpp's 4 postInteractWalk handlers. The real caller
// is presumably a per-object "think" function-pointer slot (see .h banner) reached dynamically
// through rec_dispatch, so the registry intercepts it regardless of which object stamped the pointer.
#include "core.h"
#include "game.h"
#include "override_registry.h"   // overrides::install — the one native-override registry
#include "beh_actor_tomba_proximity_combat.h"
#include "guest_abi.h"   // GuestFrameSpill — named spill-table vocabulary only, see below
void rec_dispatch(Core*, uint32_t);

// READABILITY PASS (2026-07-15, code-quality pilot — see game/render/node_xform.cpp /
// game/audio/sequencer.cpp for the recipe): this file's own banner (top of file) already flags the
// body as a dense MIPS transliteration with ~30 conditional edges across 2 jump tables and "high
// transcription risk for a manual restructure" — the same class of risk sequencer.cpp's dense SPU
// pipelines document (a prior hand-restructure there introduced 2 real re-converging-branch bugs).
// Left fully register-literal per the task's own escape hatch for exactly this shape.
//
// NOT converted to GuestFrame<72,9> RAII despite matching its (size, spill-table) contract exactly
// (see kSpills below, verified against `tools/abi_extract.py 0x800527C8 --contract`): this function
// has TWO early `return;` statements (the jump-table `default: rec_dispatch(...); return;` cases,
// see L_800529AC's and L_80052D70's switches below) that in GROUND TRUTH skip the frame restore
// entirely — a real MIPS `jr` tail-jump out of the function, not a call-return through this
// function's own epilogue (confirmed: abi_extract's epilogue-restores report lists ONLY
// L_80053060, nowhere else). GuestFrame's destructor fires on EVERY scope exit including those two
// `return;`s, which would restore+ascend sp on a path ground truth does NOT — a real behavior
// divergence RAII can't represent here. Kept as manual spill/restore instead; kSpills documents the
// contract for the record without invoking a lifetime model this function's control flow violates.
static constexpr GuestFrameSpill kSpills[] = {
    {22, 56}, {23, 60}, {31, 64}, {21, 52}, {20, 48}, {19, 44}, {18, 40}, {17, 36}, {16, 32}};

void beh_actor_tomba_proximity_combat(Core* c) {  // FUN_800527C8
  // Prologue: descend the guest frame (-72) and spill r16-r23+ra to kSpills' offsets, captured
  // BEFORE self/G below overwrite r22/r23 (i.e. these are the CALLER's live values, matching the
  // real callee-save contract). Restored manually at L_80053060 below — NOT at the two early
  // `default:` returns inside the switches, matching ground truth exactly (see banner above).
  uint32_t saved[9];
  for (int i = 0; i < 9; i++) saved[i] = c->r[kSpills[i].reg];
  c->r[29] -= 72;
  for (int i = 0; i < 9; i++) c->mem_w32(c->r[29] + (uint32_t)kSpills[i].offset, saved[i]);

  uint32_t self = 0, G = 0;

  self = c->r[4] + (uint32_t)0;
  c->r[2] = (uint32_t)32782u << 16;
  G = c->r[2] + (uint32_t)32384;
  c->r[22] = self;  // mirror into the register file: this is what the spill above will restore
  c->r[23] = G;



  c->r[3] = (uint32_t)c->mem_r8((self + (uint32_t)4));
  c->r[2] = (uint32_t)0 + (uint32_t)1;
  { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)((int32_t)c->r[3] < 2); if (_t) goto L_800529AC; }
  { int _t = (c->r[2] == (uint32_t)0);  if (_t) goto L_80052824; }
  { int _t = (c->r[3] == (uint32_t)0); c->r[4] = self + (uint32_t)0; if (_t) goto L_80052838; }
   goto L_80053060;
L_80052824:;
  c->r[2] = (uint32_t)((int32_t)c->r[3] < 4);
  { int _t = (c->r[2] == (uint32_t)0);  if (_t) goto L_80053060; }
   goto L_80053058;
L_80052838:;
  c->r[5] = (uint32_t)0 + (uint32_t)18;
  c->r[2] = (uint32_t)32783u << 16;
  c->r[16] = c->r[2] + (uint32_t)-12456;
  c->r[6] = c->mem_r32((c->r[16] + (uint32_t)20));
  c->r[7] = (uint32_t)32778u << 16;
  c->r[7] = c->r[7] + (uint32_t)17384; rec_dispatch(c, 0x800519E0u);
  { int _t = (c->r[2] != (uint32_t)0); c->r[5] = (uint32_t)0 + (uint32_t)0; if (_t) goto L_80053060; }
  c->r[4] = self + (uint32_t)0;
  c->r[6] = c->r[5] + (uint32_t)0;
  c->r[2] = (uint32_t)32770u << 16;
  c->r[3] = c->mem_r32((c->r[16] + (uint32_t)24));
  c->r[2] = c->r[2] + (uint32_t)-19628;
  c->mem_w32((self + (uint32_t)124), c->r[2]);
  c->mem_w32((self + (uint32_t)60), c->r[3]); rec_dispatch(c, 0x80041718u);
  c->r[2] = (uint32_t)c->mem_r8((self + (uint32_t)4));
  c->r[3] = (uint32_t)c->mem_r8((self + (uint32_t)3));
  c->r[2] = c->r[2] + (uint32_t)1;
  { int _t = (c->r[3] != (uint32_t)0); c->mem_w8((self + (uint32_t)4), (uint8_t)c->r[2]); if (_t) goto L_80052938; }
  c->r[2] = (uint32_t)c->mem_r16((G + (uint32_t)46));
  c->mem_w16((self + (uint32_t)46), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)c->mem_r16((G + (uint32_t)50));
  c->r[3] = (uint32_t)c->mem_r16((G + (uint32_t)98));
  c->r[2] = c->r[2] + c->r[3];
  c->r[2] = c->r[2] + (uint32_t)-160;
  c->mem_w16((self + (uint32_t)50), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)c->mem_r16((G + (uint32_t)54));
  c->mem_w16((self + (uint32_t)54), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)c->mem_r8((G + (uint32_t)2));
  { int _t = (c->r[2] != (uint32_t)0);  if (_t) goto L_800528E4; }
  c->r[2] = (uint32_t)c->mem_r16((G + (uint32_t)320));
  c->r[2] = c->r[2] + (uint32_t)1024;
  c->r[2] = c->r[2] & 4095u; goto L_800528FC;
L_800528E4:;
  c->r[2] = (uint32_t)8064u << 16;
  c->r[2] = c->r[2] + (uint32_t)208;
  c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)2));
  c->r[6] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)10));
  c->r[4] = self + (uint32_t)44; rec_dispatch(c, 0x800782B0u);
L_800528FC:;
  c->mem_w16((self + (uint32_t)86), (uint16_t)c->r[2]);
  c->r[4] = G + (uint32_t)0;
  c->r[2] = (uint32_t)c->mem_r16((self + (uint32_t)50));
  c->r[5] = (uint32_t)0 + (uint32_t)228;
  c->r[2] = c->r[2] + (uint32_t)-60;
  c->mem_w16((G + (uint32_t)50), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)c->mem_r16((self + (uint32_t)86));
  c->r[6] = (uint32_t)0 + (uint32_t)0;
  c->mem_w16((G + (uint32_t)86), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)0 + (uint32_t)30;
  c->mem_w16((self + (uint32_t)64), (uint16_t)c->r[2]); rec_dispatch(c, 0x80054D14u);
  c->r[2] = (uint32_t)0 + (uint32_t)1;
  c->mem_w8((G + (uint32_t)378), (uint8_t)c->r[2]); goto L_80053060;
L_80052938:;
  c->r[2] = (uint32_t)c->mem_r16((G + (uint32_t)50));
  c->r[2] = c->r[2] + (uint32_t)1000;
  c->mem_w16((self + (uint32_t)98), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)c->mem_r16((G + (uint32_t)46));
  c->r[4] = self + (uint32_t)0;
  c->mem_w16((self + (uint32_t)100), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)c->mem_r16((G + (uint32_t)98));
  c->r[3] = (uint32_t)c->mem_r16((G + (uint32_t)50));
  c->r[5] = (uint32_t)0 + (uint32_t)3;
  c->r[2] = c->r[2] + c->r[3];
  c->r[2] = c->r[2] + (uint32_t)840;
  c->mem_w16((self + (uint32_t)102), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)c->mem_r16((G + (uint32_t)54));
  c->r[6] = (uint32_t)0 + (uint32_t)0;
  c->mem_w16((self + (uint32_t)104), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)c->mem_r16((G + (uint32_t)86));
  c->r[3] = (uint32_t)0 + (uint32_t)20;
  c->mem_w16((self + (uint32_t)64), (uint16_t)c->r[3]);
  c->r[2] = c->r[2] & 4095u;
  c->mem_w16((self + (uint32_t)96), (uint16_t)c->r[2]); rec_dispatch(c, 0x80041718u);
  c->r[4] = G + (uint32_t)0;
  c->r[5] = (uint32_t)0 + (uint32_t)228;
  c->r[6] = (uint32_t)0 + (uint32_t)0;
  c->mem_w8((c->r[4] + (uint32_t)1), (uint8_t)(uint32_t)0); rec_dispatch(c, 0x80054D14u);
  c->mem_w8((self + (uint32_t)1), (uint8_t)(uint32_t)0); goto L_80053060;
L_800529AC:;
  c->r[2] = (uint32_t)c->mem_r8((self + (uint32_t)3));
  { int _t = (c->r[2] != (uint32_t)0);  if (_t) goto L_80052D70; }
  c->r[3] = (uint32_t)c->mem_r8((self + (uint32_t)5));
  c->r[2] = (uint32_t)(c->r[3] < (uint32_t)5);
  { int _t = (c->r[2] == (uint32_t)0); c->r[2] = (uint32_t)32769u << 16; if (_t) goto L_80052D1C; }
  c->r[2] = c->r[2] + (uint32_t)23280;
  c->r[3] = c->r[3] << 2;
  c->r[3] = c->r[3] + c->r[2];
  c->r[2] = c->mem_r32((c->r[3] + (uint32_t)0));
  {  switch (c->r[2]) { case 0x800529ECu: goto L_800529EC; case 0x80052B18u: goto L_80052B18; case 0x80052B70u: goto L_80052B70; case 0x80052C10u: goto L_80052C10; case 0x80052CB8u: goto L_80052CB8; default: rec_dispatch(c, c->r[2]); return; } }
L_800529EC:;
  c->r[2] = (uint32_t)c->mem_r16((self + (uint32_t)50));
  c->r[2] = c->r[2] + (uint32_t)-60;
  c->mem_w16((G + (uint32_t)50), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)(int16_t)c->mem_r16((self + (uint32_t)64));
  c->r[3] = (uint32_t)c->mem_r16((self + (uint32_t)64));
  { int _t = (c->r[2] != (uint32_t)0); c->r[2] = c->r[3] + (uint32_t)-1; if (_t) goto L_80052B10; }
   rec_dispatch(c, 0x80042728u);
  { int _t = (c->r[2] == (uint32_t)0); c->r[4] = self + (uint32_t)0; if (_t) goto L_80052D1C; }
  c->r[5] = (uint32_t)0 + (uint32_t)2;
  c->r[6] = (uint32_t)0 + (uint32_t)4;
  c->r[2] = (uint32_t)0 + (uint32_t)256;
  c->mem_w16((self + (uint32_t)68), (uint16_t)c->r[2]);
  c->mem_w16((self + (uint32_t)74), (uint16_t)(uint32_t)0); rec_dispatch(c, 0x80041768u);
  c->r[16] = (uint32_t)8064u << 16;
  c->r[16] = c->r[16] + (uint32_t)280;
  c->r[4] = c->r[16] + (uint32_t)0;
  c->r[21] = (uint32_t)8064u << 16;
  c->r[20] = c->r[21] + (uint32_t)192;
  c->r[5] = c->r[20] + (uint32_t)0;
  c->r[19] = (uint32_t)8064u << 16;
  c->r[19] = c->r[19] + (uint32_t)20;
  c->r[6] = c->r[19] + (uint32_t)0;
  c->r[2] = (uint32_t)0 + (uint32_t)20;
  c->mem_w16((self + (uint32_t)64), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)c->mem_r8((self + (uint32_t)5));
  c->r[3] = (uint32_t)0 + (uint32_t)12;
  c->mem_w16((self + (uint32_t)66), (uint16_t)c->r[3]);
  c->r[2] = c->r[2] + (uint32_t)1;
  c->mem_w8((self + (uint32_t)5), (uint8_t)c->r[2]);
  c->r[2] = (uint32_t)0 + (uint32_t)400;
  c->mem_w16((c->r[21] + (uint32_t)192), (uint16_t)c->r[2]);
  c->mem_w16((c->r[20] + (uint32_t)2), (uint16_t)(uint32_t)0);
  c->mem_w16((c->r[20] + (uint32_t)4), (uint16_t)(uint32_t)0); rec_dispatch(c, 0x80084470u);
  c->r[17] = c->r[16] + (uint32_t)-72;
  c->r[18] = c->r[19] + (uint32_t)-20;
  c->r[2] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)2));
  c->r[3] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)20));
  c->r[4] = c->r[16] + (uint32_t)0;
  c->r[2] = c->r[2] + c->r[3];
  c->mem_w16((self + (uint32_t)100), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)6));
  c->r[3] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)24));
  c->r[5] = c->r[20] + (uint32_t)0;
  c->r[2] = c->r[2] + c->r[3];
  c->mem_w16((self + (uint32_t)102), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)10));
  c->r[3] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)28));
  c->r[6] = c->r[19] + (uint32_t)0;
  c->r[2] = c->r[2] + c->r[3];
  c->mem_w16((self + (uint32_t)104), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)0 + (uint32_t)-1000;
  c->mem_w16((c->r[21] + (uint32_t)192), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)0 + (uint32_t)600;
  c->mem_w16((c->r[5] + (uint32_t)2), (uint16_t)(uint32_t)0);
  c->mem_w16((c->r[5] + (uint32_t)4), (uint16_t)c->r[2]); rec_dispatch(c, 0x80084470u);
  c->r[2] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)2));
  c->r[3] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)20));
  c->r[2] = c->r[2] + c->r[3];
  c->mem_w16((self + (uint32_t)96), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)10));
  c->r[3] = (uint32_t)c->mem_r16((c->r[18] + (uint32_t)28));
  c->r[2] = c->r[2] + c->r[3];
  c->mem_w16((self + (uint32_t)98), (uint16_t)c->r[2]); goto L_80052D1C;
L_80052B10:;
  c->mem_w16((self + (uint32_t)64), (uint16_t)c->r[2]); goto L_80052D1C;
L_80052B18:;
  c->r[2] = (uint32_t)c->mem_r16((self + (uint32_t)64));
  c->r[2] = c->r[2] + (uint32_t)-1;
  c->mem_w16((self + (uint32_t)64), (uint16_t)c->r[2]);
  c->r[2] = c->r[2] << 16;
  { int _t = (c->r[2] != (uint32_t)0); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_80052D08; }
  c->r[2] = c->r[2] + (uint32_t)-2040;
  c->r[3] = (uint32_t)0 + (uint32_t)4;
  c->mem_w8((c->r[2] + (uint32_t)7), (uint8_t)c->r[3]);
  c->r[3] = (uint32_t)0 + (uint32_t)1;
  c->mem_w8((c->r[2] + (uint32_t)49), (uint8_t)c->r[3]);
  c->r[2] = (uint32_t)0 + (uint32_t)20;
  c->mem_w16((self + (uint32_t)64), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)c->mem_r8((self + (uint32_t)5));
  c->r[3] = (uint32_t)c->mem_r16((self + (uint32_t)96));
  c->r[4] = (uint32_t)c->mem_r16((self + (uint32_t)98));
  c->r[2] = c->r[2] + (uint32_t)1;
  c->mem_w8((self + (uint32_t)5), (uint8_t)c->r[2]);
  c->mem_w16((self + (uint32_t)100), (uint16_t)c->r[3]);
  c->mem_w16((self + (uint32_t)104), (uint16_t)c->r[4]); goto L_80052D08;
L_80052B70:;
  c->r[2] = (uint32_t)8064u << 16;
  c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)380));
  c->r[2] = c->r[2] & 7u;
  { int _t = (c->r[2] != (uint32_t)0); c->r[4] = (uint32_t)0 + (uint32_t)56; if (_t) goto L_80052BBC; }
  c->r[5] = (uint32_t)(int8_t)c->mem_r8((self + (uint32_t)66));
  c->r[6] = (uint32_t)0 + (uint32_t)0; rec_dispatch(c, 0x80074590u);
  c->r[2] = (uint32_t)c->mem_r16((self + (uint32_t)66));
  c->r[2] = c->r[2] + (uint32_t)2;
  c->mem_w16((self + (uint32_t)66), (uint16_t)c->r[2]);
  c->r[2] = c->r[2] << 16;
  c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
  c->r[2] = (uint32_t)((int32_t)c->r[2] < 27);
  { int _t = (c->r[2] != (uint32_t)0); c->r[2] = (uint32_t)0 + (uint32_t)26; if (_t) goto L_80052BBC; }
  c->mem_w16((self + (uint32_t)66), (uint16_t)c->r[2]);
L_80052BBC:;
  c->r[2] = (uint32_t)c->mem_r16((self + (uint32_t)64));
  c->r[2] = c->r[2] + (uint32_t)-1;
  c->mem_w16((self + (uint32_t)64), (uint16_t)c->r[2]);
  c->r[2] = c->r[2] << 16;
  { int _t = (c->r[2] != (uint32_t)0); c->r[3] = (uint32_t)32780u << 16; if (_t) goto L_80052D08; }
  c->r[3] = c->r[3] + (uint32_t)-2040;
  c->r[2] = (uint32_t)c->mem_r8((c->r[3] + (uint32_t)7));
  c->r[2] = c->r[2] | 128u;
  c->mem_w8((c->r[3] + (uint32_t)7), (uint8_t)c->r[2]);
  c->r[2] = (uint32_t)c->mem_r8((self + (uint32_t)5));
  c->r[3] = (uint32_t)0 + (uint32_t)20;
  c->mem_w16((self + (uint32_t)64), (uint16_t)c->r[3]);
  c->r[3] = (uint32_t)32783u << 16;
  c->r[2] = c->r[2] + (uint32_t)1;
  c->mem_w8((self + (uint32_t)5), (uint8_t)c->r[2]);
  c->r[2] = (uint32_t)0 + (uint32_t)15;
  c->mem_w8((c->r[3] + (uint32_t)-32660), (uint8_t)c->r[2]); goto L_80052D08;
L_80052C10:;
  c->r[2] = (uint32_t)8064u << 16;
  c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)380));
  c->r[2] = c->r[2] & 7u;
  { int _t = (c->r[2] != (uint32_t)0); c->r[4] = (uint32_t)0 + (uint32_t)56; if (_t) goto L_80052C5C; }
  c->r[5] = (uint32_t)(int8_t)c->mem_r8((self + (uint32_t)66));
  c->r[6] = (uint32_t)0 + (uint32_t)0; rec_dispatch(c, 0x80074590u);
  c->r[2] = (uint32_t)c->mem_r16((self + (uint32_t)66));
  c->r[2] = c->r[2] + (uint32_t)2;
  c->mem_w16((self + (uint32_t)66), (uint16_t)c->r[2]);
  c->r[2] = c->r[2] << 16;
  c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
  c->r[2] = (uint32_t)((int32_t)c->r[2] < 27);
  { int _t = (c->r[2] != (uint32_t)0); c->r[2] = (uint32_t)0 + (uint32_t)26; if (_t) goto L_80052C5C; }
  c->mem_w16((self + (uint32_t)66), (uint16_t)c->r[2]);
L_80052C5C:;
  c->r[2] = (uint32_t)(int16_t)c->mem_r16((self + (uint32_t)64));
  c->r[3] = (uint32_t)c->mem_r16((self + (uint32_t)64));
  { int _t = (c->r[2] != (uint32_t)0); c->r[2] = c->r[3] + (uint32_t)-1; if (_t) goto L_80052CB0; }
  c->r[2] = (uint32_t)8064u << 16;
  c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)634));
  c->r[2] = (uint32_t)0 + (uint32_t)2;
  { int _t = (c->r[3] == c->r[2]); c->r[4] = (uint32_t)32780u << 16; if (_t) goto L_80052D1C; }
  c->r[4] = c->r[4] + (uint32_t)-2040;
  c->r[5] = (uint32_t)8064u << 16;
  c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)7));
  c->r[3] = (uint32_t)0 + (uint32_t)6;
  c->mem_w8((c->r[5] + (uint32_t)566), (uint8_t)c->r[3]);
  c->r[2] = c->r[2] & 127u;
  c->mem_w8((c->r[4] + (uint32_t)7), (uint8_t)c->r[2]);
  c->r[2] = (uint32_t)c->mem_r8((self + (uint32_t)5));
  c->r[4] = self + (uint32_t)0;
  c->r[2] = c->r[2] + (uint32_t)1;
  c->mem_w8((self + (uint32_t)5), (uint8_t)c->r[2]); goto L_80052D0C;
L_80052CB0:;
  c->mem_w16((self + (uint32_t)64), (uint16_t)c->r[2]); goto L_80052D08;
L_80052CB8:;
  c->r[2] = (uint32_t)8064u << 16;
  c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)380));
  c->r[2] = c->r[2] & 7u;
  { int _t = (c->r[2] != (uint32_t)0); c->r[4] = self + (uint32_t)0; if (_t) goto L_80052D0C; }
  c->r[4] = (uint32_t)0 + (uint32_t)56;
  c->r[5] = (uint32_t)(int8_t)c->mem_r8((self + (uint32_t)66));
  c->r[6] = (uint32_t)0 + (uint32_t)0; rec_dispatch(c, 0x80074590u);
  c->r[2] = (uint32_t)c->mem_r16((self + (uint32_t)66));
  c->r[2] = c->r[2] + (uint32_t)2;
  c->mem_w16((self + (uint32_t)66), (uint16_t)c->r[2]);
  c->r[2] = c->r[2] << 16;
  c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
  c->r[2] = (uint32_t)((int32_t)c->r[2] < 27);
  { int _t = (c->r[2] != (uint32_t)0); c->r[2] = (uint32_t)0 + (uint32_t)26; if (_t) goto L_80052D08; }
  c->mem_w16((self + (uint32_t)66), (uint16_t)c->r[2]);
L_80052D08:;
  c->r[4] = self + (uint32_t)0;
L_80052D0C:;
   rec_dispatch(c, 0x80052720u);
  c->r[4] = self + (uint32_t)0; rec_dispatch(c, 0x8005262Cu);
L_80052D1C:;
  c->r[3] = (uint32_t)0 + (uint32_t)1;
  c->mem_w8((G + (uint32_t)378), (uint8_t)c->r[3]);
  c->r[2] = (uint32_t)c->mem_r16((self + (uint32_t)46));
  c->mem_w16((G + (uint32_t)46), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)c->mem_r8((self + (uint32_t)5));
  { int _t = (c->r[2] == (uint32_t)0);  if (_t) goto L_80052D50; }
  c->r[2] = (uint32_t)c->mem_r16((self + (uint32_t)50));
  c->r[2] = c->r[2] + (uint32_t)-100;
  c->mem_w16((G + (uint32_t)50), (uint16_t)c->r[2]);
L_80052D50:;
  c->r[2] = (uint32_t)c->mem_r16((self + (uint32_t)54));
  c->mem_w16((G + (uint32_t)54), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)c->mem_r16((self + (uint32_t)86));
  c->mem_w16((G + (uint32_t)86), (uint16_t)c->r[2]);
  c->mem_w8((self + (uint32_t)1), (uint8_t)c->r[3]); goto L_80053040;
L_80052D70:;
  c->r[3] = (uint32_t)c->mem_r8((self + (uint32_t)5));
  c->r[2] = (uint32_t)(c->r[3] < (uint32_t)5);
  { int _t = (c->r[2] == (uint32_t)0); c->r[2] = (uint32_t)32769u << 16; if (_t) goto L_80052FE8; }
  c->r[2] = c->r[2] + (uint32_t)23304;
  c->r[3] = c->r[3] << 2;
  c->r[3] = c->r[3] + c->r[2];
  c->r[2] = c->mem_r32((c->r[3] + (uint32_t)0));
  {  switch (c->r[2]) { case 0x80052DA0u: goto L_80052DA0; case 0x80052E68u: goto L_80052E68; case 0x80052EB0u: goto L_80052EB0; case 0x80052F00u: goto L_80052F00; case 0x80052F50u: goto L_80052F50; default: rec_dispatch(c, c->r[2]); return; } }
L_80052DA0:;
  c->r[2] = (uint32_t)c->mem_r16((self + (uint32_t)64));
  c->r[2] = c->r[2] + (uint32_t)-1;
  c->mem_w16((self + (uint32_t)64), (uint16_t)c->r[2]);
  c->r[2] = c->r[2] << 16;
  { int _t = (c->r[2] != (uint32_t)0); c->r[17] = (uint32_t)8064u << 16; if (_t) goto L_80052FE8; }
  c->r[17] = c->r[17] + (uint32_t)280;
  c->r[4] = c->r[17] + (uint32_t)0;
  c->r[3] = (uint32_t)8064u << 16;
  c->r[5] = c->r[3] + (uint32_t)192;
  c->r[16] = (uint32_t)8064u << 16;
  c->r[16] = c->r[16] + (uint32_t)20;
  c->r[6] = c->r[16] + (uint32_t)0;
  c->r[2] = (uint32_t)0 + (uint32_t)-1000;
  c->mem_w16((c->r[3] + (uint32_t)192), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)0 + (uint32_t)600;
  c->mem_w16((c->r[5] + (uint32_t)2), (uint16_t)(uint32_t)0);
  c->mem_w16((c->r[5] + (uint32_t)4), (uint16_t)c->r[2]); rec_dispatch(c, 0x80084470u);
  c->r[17] = c->r[17] + (uint32_t)-72;
  c->r[16] = c->r[16] + (uint32_t)-20;
  c->r[2] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)2));
  c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)20));
  c->r[2] = c->r[2] + c->r[3];
  c->mem_w16((self + (uint32_t)46), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)10));
  c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)28));
  c->r[4] = self + (uint32_t)44;
  c->r[2] = c->r[2] + c->r[3];
  c->mem_w16((self + (uint32_t)54), (uint16_t)c->r[2]);
  c->r[3] = (uint32_t)c->mem_r16((G + (uint32_t)50));
  c->r[2] = (uint32_t)0 + (uint32_t)1;
  c->mem_w8((self + (uint32_t)1), (uint8_t)c->r[2]);
  c->mem_w16((self + (uint32_t)50), (uint16_t)c->r[3]);
  c->mem_w8((G + (uint32_t)1), (uint8_t)c->r[2]);
  c->r[5] = (uint32_t)(int16_t)c->mem_r16((self + (uint32_t)100));
  c->r[6] = (uint32_t)(int16_t)c->mem_r16((self + (uint32_t)104));
  c->r[2] = (uint32_t)0 + (uint32_t)6144;
  c->mem_w16((self + (uint32_t)74), (uint16_t)(uint32_t)0);
  c->mem_w16((self + (uint32_t)68), (uint16_t)c->r[2]); rec_dispatch(c, 0x800782B0u);
  c->r[2] = c->r[2] + (uint32_t)-512;
  c->r[3] = (uint32_t)c->mem_r8((self + (uint32_t)5));
  c->r[2] = c->r[2] & 4095u;
  c->mem_w16((self + (uint32_t)86), (uint16_t)c->r[2]);
  c->r[3] = c->r[3] + (uint32_t)1;
  c->mem_w8((self + (uint32_t)5), (uint8_t)c->r[3]); goto L_80052FE8;
L_80052E68:;
  c->r[4] = self + (uint32_t)0; rec_dispatch(c, 0x80052720u);
  c->r[4] = self + (uint32_t)0; rec_dispatch(c, 0x80052694u);
  c->r[4] = (uint32_t)(int16_t)c->mem_r16((self + (uint32_t)50));
  c->r[3] = (uint32_t)(int16_t)c->mem_r16((self + (uint32_t)102));
  c->r[2] = (uint32_t)0 + (uint32_t)1;
  { int _t = (c->r[4] != c->r[3]); c->mem_w8((self + (uint32_t)1), (uint8_t)c->r[2]); if (_t) goto L_80052FE8; }
  c->r[4] = self + (uint32_t)0;
  c->r[5] = (uint32_t)0 + (uint32_t)1;
  c->r[2] = (uint32_t)c->mem_r8((self + (uint32_t)5));
  c->r[6] = (uint32_t)0 + (uint32_t)4;
  c->r[2] = c->r[2] + c->r[5];
  c->mem_w8((self + (uint32_t)5), (uint8_t)c->r[2]); rec_dispatch(c, 0x80041768u);
   goto L_80052FE8;
L_80052EB0:;
  // Guest frame used sp+16..+30 (its OWN 72-byte descent) as a scratch 3-halfword staging buffer
  // for FUN_8006CEC4's a1 pointer. We don't own a real guest frame here (native call convention,
  // not guest-ABI-framed — see actor_melee_engage.cpp precedent), so carve a scoped 32-byte
  // descent instead of touching whatever the CALLER's sp currently points at.
  {
    const uint32_t savedSp = c->r[29];
    c->r[29] -= 32;
    c->r[2] = (uint32_t)c->mem_r16((self + (uint32_t)100));
    c->r[4] = self + (uint32_t)44;
    c->mem_w16((c->r[29] + (uint32_t)18), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((self + (uint32_t)102));
    c->r[5] = c->r[29] + (uint32_t)16;
    c->mem_w16((c->r[29] + (uint32_t)22), (uint16_t)c->r[2]);
    c->r[2] = (uint32_t)c->mem_r16((self + (uint32_t)104));
    c->r[6] = (uint32_t)0 + (uint32_t)224;
    c->mem_w16((c->r[29] + (uint32_t)26), (uint16_t)c->r[2]); rec_dispatch(c, 0x8006CEC4u);
    c->r[29] = savedSp;
  }
  { int _t = (c->r[2] == (uint32_t)0); c->r[4] = self + (uint32_t)0; if (_t) goto L_80052F44; }
  c->r[5] = (uint32_t)0 + (uint32_t)0;
  c->r[2] = (uint32_t)c->mem_r8((self + (uint32_t)5));
  c->r[6] = (uint32_t)0 + (uint32_t)4;
  c->r[2] = c->r[2] + (uint32_t)1;
  c->mem_w8((self + (uint32_t)5), (uint8_t)c->r[2]); rec_dispatch(c, 0x80041768u);
  c->r[2] = (uint32_t)0 + (uint32_t)1; goto L_80052F48;
L_80052F00:;
  c->r[4] = (uint32_t)(int16_t)c->mem_r16((self + (uint32_t)96));
  c->r[5] = (uint32_t)(int16_t)c->mem_r16((self + (uint32_t)86));
  c->r[6] = (uint32_t)0 + (uint32_t)128; rec_dispatch(c, 0x800776F8u);
  c->mem_w16((self + (uint32_t)86), (uint16_t)c->r[2]);
  c->r[2] = c->r[2] << 16;
  c->r[3] = (uint32_t)(int16_t)c->mem_r16((self + (uint32_t)96));
  c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
  { int _t = (c->r[2] != c->r[3]); c->r[2] = (uint32_t)0 + (uint32_t)1; if (_t) goto L_80052F48; }
  c->r[4] = G + (uint32_t)0;
  c->r[5] = (uint32_t)0 + (uint32_t)2;
  c->r[2] = (uint32_t)c->mem_r8((self + (uint32_t)5));
  c->r[6] = (uint32_t)0 + (uint32_t)16;
  c->r[2] = c->r[2] + (uint32_t)1;
  c->mem_w8((self + (uint32_t)5), (uint8_t)c->r[2]); rec_dispatch(c, 0x80054D14u);
L_80052F44:;
  c->r[2] = (uint32_t)0 + (uint32_t)1;
L_80052F48:;
  c->mem_w8((self + (uint32_t)1), (uint8_t)c->r[2]); goto L_80052FE8;
L_80052F50:;
  c->r[2] = (uint32_t)c->mem_r16((self + (uint32_t)184));
  c->r[2] = c->r[2] + (uint32_t)-256;
  c->mem_w16((self + (uint32_t)184), (uint16_t)c->r[2]);
  c->r[2] = c->r[2] & 65535u;
  c->r[2] = (uint32_t)(c->r[2] < (uint32_t)257);
  { int _t = (c->r[2] == (uint32_t)0); c->r[2] = (uint32_t)0 + (uint32_t)256; if (_t) goto L_80052F94; }
  c->mem_w16((self + (uint32_t)184), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)8064u << 16;
  c->mem_w8((c->r[2] + (uint32_t)311), (uint8_t)(uint32_t)0);
  c->r[2] = (uint32_t)8064u << 16;
  c->mem_w8((c->r[2] + (uint32_t)566), (uint8_t)(uint32_t)0);
  c->r[2] = (uint32_t)32783u << 16;
  c->mem_w8((c->r[2] + (uint32_t)-32660), (uint8_t)(uint32_t)0);
  c->r[2] = (uint32_t)0 + (uint32_t)3;
  c->mem_w8((self + (uint32_t)4), (uint8_t)c->r[2]);
L_80052F94:;
  c->r[2] = (uint32_t)c->mem_r16((G + (uint32_t)50));
  c->r[2] = c->r[2] + (uint32_t)10;
  c->mem_w16((G + (uint32_t)50), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)c->mem_r16((self + (uint32_t)50));
  c->r[2] = c->r[2] + (uint32_t)10;
  c->mem_w16((self + (uint32_t)50), (uint16_t)c->r[2]);
  c->r[3] = (uint32_t)(int16_t)c->mem_r16((G + (uint32_t)50));
  c->r[2] = (uint32_t)(int16_t)c->mem_r16((self + (uint32_t)98));
  c->r[2] = (uint32_t)((int32_t)c->r[2] < (int32_t)c->r[3]);
  c->r[3] = (uint32_t)c->mem_r16((self + (uint32_t)98));
  { int _t = (c->r[2] == (uint32_t)0);  if (_t) goto L_80052FD4; }
  c->mem_w16((G + (uint32_t)50), (uint16_t)c->r[3]);
L_80052FD4:;
  c->r[3] = (uint32_t)c->mem_r16((self + (uint32_t)184));
  c->r[2] = (uint32_t)0 + (uint32_t)1;
  c->mem_w8((self + (uint32_t)1), (uint8_t)c->r[2]);
  c->mem_w16((self + (uint32_t)188), (uint16_t)c->r[3]);
  c->mem_w16((self + (uint32_t)186), (uint16_t)c->r[3]);
L_80052FE8:;
  c->r[2] = (uint32_t)c->mem_r8((self + (uint32_t)1));
  { int _t = (c->r[2] == (uint32_t)0);  if (_t) goto L_80053040; }
  c->r[2] = (uint32_t)c->mem_r16((self + (uint32_t)46));
  c->mem_w16((G + (uint32_t)46), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)c->mem_r8((self + (uint32_t)5));
  c->r[2] = (uint32_t)(c->r[2] < (uint32_t)4);
  { int _t = (c->r[2] == (uint32_t)0);  if (_t) goto L_80053028; }
  c->r[2] = (uint32_t)c->mem_r16((self + (uint32_t)50));
  c->r[2] = c->r[2] + (uint32_t)-100;
  c->mem_w16((G + (uint32_t)50), (uint16_t)c->r[2]);
L_80053028:;
  c->r[2] = (uint32_t)c->mem_r16((self + (uint32_t)54));
  c->mem_w16((G + (uint32_t)54), (uint16_t)c->r[2]);
  c->r[2] = (uint32_t)c->mem_r16((self + (uint32_t)86));
  c->mem_w16((G + (uint32_t)86), (uint16_t)c->r[2]);
L_80053040:;
  c->r[4] = self + (uint32_t)0; rec_dispatch(c, 0x8004190Cu);
  c->r[4] = self + (uint32_t)0; rec_dispatch(c, 0x800518FCu);
   goto L_80053060;
L_80053058:;
  c->r[4] = self + (uint32_t)0; rec_dispatch(c, 0x8007A624u);
L_80053060:;
  // Epilogue: restore r16-r23+ra from kSpills' offsets and ascend sp — the mirror of the manual
  // prologue above. NOTE: the two jump-table `default: rec_dispatch(...); return;` cases earlier in
  // this function do NOT reach here — that matches ground truth exactly (see the file banner's
  // explanation of why this is manual spill/restore rather than GuestFrame RAII).
  for (int i = 0; i < 9; i++) c->r[kSpills[i].reg] = c->mem_r32(c->r[29] + (uint32_t)kSpills[i].offset);
  c->r[29] += 72;
  return;
}

// ---------------------------------------------------------------------------------------------
// Wiring: no static `func_800527C8(c)` call site found anywhere in generated/ — only the generic
// rec_dispatch switch-case (shard_disp.c), which every recompiled address gets and which the
// registry's dispatch always checks before ever reaching gen_func_800527C8. No setter needed:
// unlike the ov_a00 toy-spawn cluster, there is no direct intra-shard `jal`/call site bypassing
// rec_dispatch for this address. Same shape as game/player/actor_tomba.cpp's registerOverrides()
// comment: "no substrate shard calls this address directly."
// ---------------------------------------------------------------------------------------------
extern void gen_func_800527C8(Core*);  // substrate body — the oracle/substrate leg

void RegisterBehActorTombaProximityCombatOverride(Game* /*game*/) {
  overrides::install(0x800527C8u, "beh_actor_tomba_proximity_combat",
                     beh_actor_tomba_proximity_combat, gen_func_800527C8);
}
