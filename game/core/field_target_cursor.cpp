// f251f0_draft.cpp — PORT_GEN draft, byte-faithful transcription of gen_func_800251F0.
// ORACLE: gen_func_800251F0 (generated/shard_1.c:2304-2488)
// PORT_GEN: 0x800251F0 generated/shard_1.c:2304-2488
//
// This body is the gen function's guest-visible operations VERBATIM — every c->r[] op,
// mem_r/mem_w call, func_X/rec_dispatch call with its r31 constant, and label/goto is
// preserved unchanged. Faithful by construction; the only allowed next step is RENAMING
// (locals/labels -> named fields/control-flow), verified equivalent by tools/port_check.py.
// UNWIRED — dead code. Do not wire into any dispatch table before running port_check.py
// and the mandatory line-by-line verify pass (docs/fleet-workflow.md §9).
#include "engine.h"
#include "game_ctx.h"
#include "override_registry.h"
#include "core.h"

void rec_dispatch(Core*, uint32_t);  // overlay_router.cpp — shared dispatch choke point
void func_80074590(Core*);  // generated/shard_disp.c

void Engine::fieldTargetCursor() {
  Core* c = core;
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[5] = c->r[2] + (uint32_t)-1936;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[2] = c->mem_r32((c->r[5] + (uint32_t)16));
    c->r[2] = c->r[2] & 1536u;
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80025578; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)17));
    c->r[2] = c->r[2] & 1u;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_80025578; }
    c->r[6] = c->r[2] + (uint32_t)-2040;
    c->r[2] = (uint32_t)c->mem_r8((c->r[6] + (uint32_t)14));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_80025578; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)5));
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)((int32_t)c->r[3] < 2); if (_t) goto L_8002535C; }
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80025264; }
    { int _t = (c->r[3] == c->r[0]); c->r[3] = c->r[0] + c->r[0]; if (_t) goto L_80025280; }
     goto L_80025578;
  L_80025264:;
    c->r[2] = c->r[0] + (uint32_t)2;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)3; if (_t) goto L_800253C0; }
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)32782u << 16; if (_t) goto L_80025474; }
     goto L_80025578;
  L_80025280:;
    c->r[6] = c->r[5] + c->r[0];
    c->r[2] = (uint32_t)32778u << 16;
    c->r[5] = c->r[2] + (uint32_t)-11644;
    c->mem_w8((c->r[4] + (uint32_t)6), (uint8_t)c->r[0]);
    c->mem_w8((c->r[4] + (uint32_t)7), (uint8_t)c->r[0]);
  L_80025294:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[5] + (uint32_t)0));
    c->r[2] = c->r[2] + c->r[6];
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)580));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_800252D0; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)7));
    c->r[2] = c->r[4] + c->r[2];
    c->mem_w8((c->r[2] + (uint32_t)34), (uint8_t)c->r[3]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)7));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[4] + (uint32_t)7), (uint8_t)c->r[2]);
  L_800252D0:;
    c->r[3] = c->r[3] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[3] < 10);
    { int _t = (c->r[2] != c->r[0]); c->r[5] = c->r[5] + (uint32_t)4; if (_t) goto L_80025294; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)7));
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)2);
    { int _t = (c->r[2] == c->r[0]); c->r[3] = c->r[0] + c->r[0]; if (_t) goto L_80025304; }
    c->mem_w8((c->r[4] + (uint32_t)8), (uint8_t)c->r[0]); goto L_80025578;
  L_800252FC:;
    c->mem_w8((c->r[4] + (uint32_t)8), (uint8_t)c->r[3]); goto L_8002535C;
  L_80025304:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)5));
    c->r[5] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)7));
    c->r[2] = c->r[2] + (uint32_t)1;
    { int _t = (c->r[5] == c->r[0]); c->mem_w8((c->r[4] + (uint32_t)5), (uint8_t)c->r[2]); if (_t) goto L_8002535C; }
    c->r[2] = (uint32_t)32778u << 16;
    c->r[7] = c->r[2] + (uint32_t)-11644;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[6] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1908));
    c->r[2] = c->r[4] + c->r[3];
  L_8002532C:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)34));
    c->r[2] = c->r[2] << 2;
    c->r[2] = c->r[2] + c->r[7];
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)0));
    { int _t = (c->r[2] == c->r[6]);  if (_t) goto L_800252FC; }
    c->r[3] = c->r[3] + (uint32_t)1;
    c->r[2] = (uint32_t)((int32_t)c->r[3] < (int32_t)c->r[5]);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[4] + c->r[3]; if (_t) goto L_8002532C; }
  L_8002535C:;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-2033));
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)32782u << 16; if (_t) goto L_80025578; }
    c->r[3] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)32360));
    c->r[2] = c->r[3] & 2048u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_8002539C; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)562));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_80025578; }
    c->mem_w8((c->r[4] + (uint32_t)5), (uint8_t)c->r[2]); goto L_800254DC;
  L_8002539C:;
    c->r[2] = c->r[3] & 1024u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_800254F4; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)562));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)3; if (_t) goto L_80025578; }
    c->mem_w8((c->r[4] + (uint32_t)5), (uint8_t)c->r[2]); goto L_800254DC;
  L_800253C0:;
    c->r[2] = (uint32_t)32782u << 16;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)32360));
    c->r[2] = c->r[2] & 2048u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80025428; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[6] + (uint32_t)7));
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80025428; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)8));
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)7));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[4] + (uint32_t)8), (uint8_t)c->r[2]);
    c->r[2] = c->r[2] & 255u;
    c->r[2] = (uint32_t)(c->r[2] < c->r[3]);
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_8002540C; }
    c->mem_w8((c->r[4] + (uint32_t)8), (uint8_t)c->r[0]);
  L_8002540C:;
    c->mem_w8((c->r[4] + (uint32_t)6), (uint8_t)c->r[0]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)562));
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_800254DC; }
    c->mem_w8((c->r[4] + (uint32_t)5), (uint8_t)c->r[0]); goto L_80025578;
  L_80025428:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)6));
    c->r[3] = c->r[0] + (uint32_t)-16;
    c->r[2] = c->r[2] + (uint32_t)-4;
    c->mem_w8((c->r[4] + (uint32_t)6), (uint8_t)c->r[2]);
    c->r[2] = c->r[2] << 24;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 24);
    { int _t = (c->r[2] != c->r[3]);  if (_t) goto L_80025578; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)8));
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)7));
    c->mem_w8((c->r[4] + (uint32_t)5), (uint8_t)c->r[0]);
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[4] + (uint32_t)8), (uint8_t)c->r[2]);
    c->r[2] = c->r[2] & 255u;
    c->r[2] = (uint32_t)(c->r[2] < c->r[3]);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_80025544; }
    c->mem_w8((c->r[4] + (uint32_t)8), (uint8_t)c->r[0]); goto L_80025544;
  L_80025474:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)32360));
    c->r[2] = c->r[2] & 1024u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_800254FC; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[6] + (uint32_t)7));
    { int _t = (c->r[2] != c->r[0]); c->r[3] = c->r[0] + (uint32_t)255; if (_t) goto L_800254FC; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)8));
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w8((c->r[4] + (uint32_t)8), (uint8_t)c->r[2]);
    c->r[2] = c->r[2] & 255u;
    { int _t = (c->r[2] != c->r[3]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_800254C8; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)7));
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w8((c->r[4] + (uint32_t)8), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)8064u << 16;
  L_800254C8:;
    c->mem_w8((c->r[4] + (uint32_t)6), (uint8_t)c->r[0]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)562));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_800254F4; }
  L_800254DC:;
    c->r[4] = c->r[0] + (uint32_t)21;
    c->r[5] = c->r[0] + (uint32_t)5;
    c->r[31] = 0x800254ECu;
    c->r[6] = c->r[0] + c->r[0]; func_80074590(c);
     goto L_80025578;
  L_800254F4:;
    c->mem_w8((c->r[4] + (uint32_t)5), (uint8_t)c->r[0]); goto L_80025578;
  L_800254FC:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)6));
    c->r[3] = c->r[0] + (uint32_t)16;
    c->r[2] = c->r[2] + (uint32_t)4;
    c->mem_w8((c->r[4] + (uint32_t)6), (uint8_t)c->r[2]);
    c->r[2] = c->r[2] & 255u;
    { int _t = (c->r[2] != c->r[3]); c->r[3] = c->r[0] + (uint32_t)255; if (_t) goto L_80025578; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)8));
    c->mem_w8((c->r[4] + (uint32_t)5), (uint8_t)c->r[0]);
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w8((c->r[4] + (uint32_t)8), (uint8_t)c->r[2]);
    c->r[2] = c->r[2] & 255u;
    { int _t = (c->r[2] != c->r[3]);  if (_t) goto L_80025544; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)7));
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w8((c->r[4] + (uint32_t)8), (uint8_t)c->r[2]);
  L_80025544:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)8));
    c->r[3] = (uint32_t)32778u << 16;
    c->r[2] = c->r[4] + c->r[2];
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)34));
    c->r[3] = c->r[3] + (uint32_t)-11644;
    c->r[2] = c->r[2] << 2;
    c->r[2] = c->r[2] + c->r[3];
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)0));
    c->r[2] = (uint32_t)32780u << 16;
    c->mem_w8((c->r[2] + (uint32_t)-1908), (uint8_t)c->r[3]);
    c->r[2] = (uint32_t)32782u << 16;
    c->mem_w8((c->r[2] + (uint32_t)32492), (uint8_t)c->r[3]);
    c->mem_w8((c->r[4] + (uint32_t)6), (uint8_t)c->r[0]);
  L_80025578:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

// ---- ownership wiring: FUN_800251F0 is the field TARGET-SELECT cursor, called every field frame from
// Engine::sceneEventFifo (the 0x800251F0 "default" branch, engine.cpp). Own it globally via the registry
// (byte-faithful; gen_func_800251F0 stays the oracle leg). setter = shard_set_override so any direct
// func_800251F0(c) caller reaches the native body too, not only rec_dispatch. ----
extern void shard_set_override(uint32_t, void (*)(Core*));
extern void gen_func_800251F0(Core*);
namespace { void ov_fieldTargetCursor(Core* c) { eng(c).fieldTargetCursor(); } }
void Engine::registerFieldTargetCursor() {
  overrides::install(0x800251F0u, "Engine::fieldTargetCursor", ov_fieldTargetCursor,
                     gen_func_800251F0, shard_set_override);
}
