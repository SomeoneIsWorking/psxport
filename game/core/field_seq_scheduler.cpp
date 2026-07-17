// fseq.cpp — PORT_GEN draft, byte-faithful transcription of gen_func_80075A80.
// ORACLE: gen_func_80075A80 (generated/shard_7.c:10956-11098)
// PORT_GEN: 0x80075A80 generated/shard_7.c:10956-11098
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
void func_800998E4(Core*);  // generated/shard_disp.c
void func_80092660(Core*);  // generated/shard_disp.c
void func_80098F90(Core*);  // generated/shard_disp.c
void func_80075824(Core*);  // generated/shard_disp.c
void func_80099490(Core*);  // generated/shard_disp.c
void func_8008E0C0(Core*);  // generated/shard_disp.c
void func_80074BF8(Core*);  // generated/shard_disp.c
void func_80074E48(Core*);  // generated/shard_disp.c

void Engine::fieldSeqSchedulerTick() {
  Core* c = core;
    c->r[29] = c->r[29] + (uint32_t)-88;
    c->r[4] = c->r[29] + (uint32_t)32;
    c->r[2] = (uint32_t)32780u << 16;
    c->mem_w32((c->r[29] + (uint32_t)76), c->r[21]);
    c->r[21] = c->r[2] + (uint32_t)-7688;
    c->mem_w32((c->r[29] + (uint32_t)80), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)72), c->r[20]);
    c->mem_w32((c->r[29] + (uint32_t)68), c->r[19]);
    c->mem_w32((c->r[29] + (uint32_t)64), c->r[18]);
    c->mem_w32((c->r[29] + (uint32_t)60), c->r[17]);
    c->r[31] = 0x80075AB0u;
    c->mem_w32((c->r[29] + (uint32_t)56), c->r[16]); func_800998E4(c);
    c->r[2] = (uint32_t)32780u << 16;
    c->r[18] = c->mem_r32((c->r[2] + (uint32_t)-4744));
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = c->r[2] + (uint32_t)-7624;
    c->r[3] = c->r[18] << 1;
    c->r[3] = c->r[3] + c->r[18];
    c->r[3] = c->r[3] << 2;
    c->r[17] = c->r[3] + c->r[2];
    c->r[2] = (uint32_t)((int32_t)c->r[18] < 24);
    { int _t = (c->r[2] == c->r[0]); c->r[16] = c->r[17] + (uint32_t)1; if (_t) goto L_80075C30; }
    c->r[20] = (uint32_t)32780u << 16;
    c->r[19] = c->r[18] << 16;
  L_80075AE4:;
    c->r[4] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)0));
    { int _t = (c->r[4] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)255; if (_t) goto L_80075BF4; }
    { int _t = (c->r[4] != c->r[2]); c->r[2] = c->r[0] + (uint32_t)4; if (_t) goto L_80075BB0; }
    c->r[7] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)2));
    c->r[2] = c->r[7] & 128u;
    { int _t = (c->r[2] == c->r[0]); c->r[4] = (uint32_t)((int32_t)c->r[19] >> 16); if (_t) goto L_80075B48; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)3));
    c->r[6] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)1));
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)4));
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[2]);
    c->r[2] = (uint32_t)32778u << 16;
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)20350));
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)5));
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)6));
    c->r[7] = c->r[7] & 15u; goto L_80075B7C;
  L_80075B48:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)3));
    c->r[6] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)1));
    c->r[7] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)2));
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[2]);
    c->r[2] = (uint32_t)32780u << 16;
    c->r[5] = (uint32_t)(int16_t)c->mem_r16((c->r[2] + (uint32_t)-4732));
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)4));
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)5));
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[2]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)6));
  L_80075B7C:;
    c->r[31] = 0x80075B84u;
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[2]); func_80092660(c);
    c->r[2] = c->r[0] + (uint32_t)1;
    c->r[2] = c->r[2] << (c->r[18] & 31);
    c->r[3] = c->mem_r32((c->r[20] + (uint32_t)-7336));
    c->r[2] = ~(c->r[0] | c->r[2]);
    c->r[3] = c->r[3] & c->r[2];
    c->mem_w32((c->r[20] + (uint32_t)-7336), c->r[3]);
    c->r[2] = (uint32_t)c->mem_r8((c->r[17] + (uint32_t)0));
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w8((c->r[17] + (uint32_t)0), (uint8_t)c->r[2]); goto L_80075C14;
  L_80075BB0:;
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)7));
    { int _t = (c->r[3] != c->r[2]); c->r[2] = c->r[4] + (uint32_t)-1; if (_t) goto L_80075BF0; }
    c->mem_w8((c->r[17] + (uint32_t)0), (uint8_t)c->r[2]);
    c->r[2] = c->r[2] & 255u;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[29] + c->r[18]; if (_t) goto L_80075BF8; }
    c->r[2] = c->r[0] + (uint32_t)1;
    c->r[3] = c->mem_r32((c->r[20] + (uint32_t)-7336));
    c->r[2] = c->r[2] << (c->r[18] & 31);
    c->r[3] = c->r[3] | c->r[2];
    c->mem_w32((c->r[20] + (uint32_t)-7336), c->r[3]);
    c->mem_w8((c->r[16] + (uint32_t)2), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)1), (uint8_t)c->r[0]); goto L_80075BF4;
  L_80075BF0:;
    c->mem_w8((c->r[17] + (uint32_t)0), (uint8_t)c->r[2]);
  L_80075BF4:;
    c->r[2] = c->r[29] + c->r[18];
  L_80075BF8:;
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)32));
    { int _t = (c->r[3] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)3; if (_t) goto L_80075C10; }
    { int _t = (c->r[3] != c->r[2]); c->r[2] = (uint32_t)1u << 16; if (_t) goto L_80075C18; }
  L_80075C10:;
    c->mem_w8((c->r[16] + (uint32_t)0), (uint8_t)c->r[0]);
  L_80075C14:;
    c->r[2] = (uint32_t)1u << 16;
  L_80075C18:;
    c->r[19] = c->r[19] + c->r[2];
    c->r[18] = c->r[18] + (uint32_t)1;
    c->r[16] = c->r[16] + (uint32_t)12;
    c->r[2] = (uint32_t)((int32_t)c->r[18] < 24);
    { int _t = (c->r[2] != c->r[0]); c->r[17] = c->r[17] + (uint32_t)12; if (_t) goto L_80075AE4; }
  L_80075C30:;
    c->r[16] = (uint32_t)32780u << 16;
    c->r[5] = c->mem_r32((c->r[16] + (uint32_t)-7336));
    { int _t = (c->r[5] == c->r[0]);  if (_t) goto L_80075C50; }
    c->r[31] = 0x80075C4Cu;
    c->r[4] = c->r[0] + c->r[0]; func_80098F90(c);
    c->mem_w32((c->r[16] + (uint32_t)-7336), c->r[0]);
  L_80075C50:;
    c->r[31] = 0x80075C58u;
    c->r[4] = c->r[21] + c->r[0]; func_80075824(c);
    c->r[31] = 0x80075C60u;
    c->r[4] = c->r[21] + c->r[0]; func_80099490(c);
    c->r[17] = (uint32_t)32780u << 16;
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[17] + (uint32_t)-4736));
    c->r[2] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[3] == c->r[2]); c->mem_w32((c->r[21] + (uint32_t)0), c->r[0]); if (_t) goto L_80075CC8; }
    c->r[2] = (uint32_t)32780u << 16;
    c->r[2] = c->r[2] + (uint32_t)-7320;
    c->r[3] = c->r[3] << 3;
    c->r[3] = c->r[3] + c->r[2];
    c->r[4] = (uint32_t)(int16_t)c->mem_r16((c->r[3] + (uint32_t)0));
    c->r[31] = 0x80075C90u;
    c->r[5] = c->r[0] + c->r[0]; func_8008E0C0(c);
    c->r[2] = c->r[2] << 16;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)32780u << 16; if (_t) goto L_80075CC8; }
    c->r[16] = c->r[2] + (uint32_t)-7688;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)50));
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[2] + c->r[0]; if (_t) goto L_80075CC0; }
    c->r[31] = 0x80075CB8u;
    c->mem_w16((c->r[17] + (uint32_t)-4736), (uint16_t)c->r[0]); func_80074BF8(c);
    c->mem_w8((c->r[16] + (uint32_t)50), (uint8_t)c->r[0]); goto L_80075CC8;
  L_80075CC0:;
    c->r[31] = 0x80075CC8u;
     func_80074E48(c);
  L_80075CC8:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)80));
    c->r[21] = c->mem_r32((c->r[29] + (uint32_t)76));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)72));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)68));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)64));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)60));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)56));
    c->r[29] = c->r[29] + (uint32_t)88; return;
    return;
}

// ---- ownership wiring: FUN_80075A80 — the field per-frame SEQUENCE-SCHEDULER tick (walks the 0x18-slot
// timer table 0x800BE238, counts each slot down, starts/stops libsnd sequences via the audio leaves,
// maintains the active-mask 0x800BE358). Called unconditionally every field frame from the owned
// Engine::fieldFrame. Byte-faithful (gen_func_80075A80 = oracle leg); audio leaves stay substrate. ----
extern void shard_set_override(uint32_t, void (*)(Core*));
extern void gen_func_80075A80(Core*);
namespace { void ov_fieldSeqSchedulerTick(Core* c) { eng(c).fieldSeqSchedulerTick(); } }
void Engine::registerFieldSeqSchedulerTick() {
  overrides::install(0x80075A80u, "Engine::fieldSeqSchedulerTick", ov_fieldSeqSchedulerTick,
                     gen_func_80075A80, shard_set_override);
}
