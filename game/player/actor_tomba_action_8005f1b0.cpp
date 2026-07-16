// actor_tomba.cpp — PORT_GEN draft, byte-faithful transcription of gen_func_8005F1B0.
// ORACLE: gen_func_8005F1B0 (generated/shard_7.c:8248-8322)
// PORT_GEN: 0x8005F1B0 generated/shard_7.c:8248-8322
//
// This body is the gen function's guest-visible operations VERBATIM — every c->r[] op,
// mem_r/mem_w call, func_X/rec_dispatch call with its r31 constant, and label/goto is
// preserved unchanged. Faithful by construction; the only allowed next step is RENAMING
// (locals/labels -> named fields/control-flow), verified equivalent by tools/port_check.py.
// WIRED 2026-07-16 via ActorTomba::registerOverrides; port_check PASS + MIRROR_VERIFY-gated.
// Verbatim PORT_GEN body — RENAMING into named fields is a future step.
#include "actor_tomba.h"
#include "core.h"

void rec_dispatch(Core*, uint32_t);  // overlay_router.cpp — shared dispatch choke point
void func_80074590(Core*);  // generated/shard_disp.c
void func_80053D90(Core*);  // generated/shard_disp.c
void func_800663A8(Core*);  // generated/shard_disp.c
void func_80066478(Core*);  // generated/shard_disp.c
void func_80066538(Core*);  // generated/shard_disp.c
void func_8005314C(Core*);  // generated/shard_disp.c
void func_8005344C(Core*);  // generated/shard_disp.c
void func_80054D14(Core*);  // generated/shard_disp.c
void func_80056D44(Core*);  // generated/shard_disp.c
void func_800551C4(Core*);  // generated/shard_disp.c

void ActorTomba::actionHandler8005F1B0() {
  Core* c = core;
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)6));
    c->r[17] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[3] == c->r[17]); c->r[2] = (uint32_t)((int32_t)c->r[3] < 2); if (_t) goto L_8005F22C; }
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8005F1EC; }
    { int _t = (c->r[3] == c->r[0]); c->r[4] = c->r[0] + (uint32_t)35; if (_t) goto L_8005F200; }
     goto L_8005F2DC;
  L_8005F1EC:;
    c->r[2] = c->r[0] + (uint32_t)2;
    { int _t = (c->r[3] == c->r[2]);  if (_t) goto L_8005F23C; }
     goto L_8005F2DC;
  L_8005F200:;
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8005F20Cu;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
    c->r[31] = 0x8005F214u;
    c->r[4] = c->r[16] + c->r[0]; func_80053D90(c);
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)1;
    c->mem_w16((c->r[16] + (uint32_t)88), (uint16_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)97), (uint8_t)c->r[0]);
    c->r[31] = 0x8005F22Cu;
    c->mem_w8((c->r[16] + (uint32_t)326), (uint8_t)c->r[0]); func_800663A8(c);
  L_8005F22C:;
    c->r[31] = 0x8005F234u;
    c->r[4] = c->r[16] + c->r[0]; func_80066478(c);
     goto L_8005F2DC;
  L_8005F23C:;
    c->r[31] = 0x8005F244u;
    c->r[4] = c->r[16] + c->r[0]; func_80066538(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)41));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8005F2A0; }
    c->r[31] = 0x8005F25Cu;
    c->r[4] = c->r[16] + c->r[0]; func_8005314C(c);
    c->r[4] = c->r[16] + c->r[0];
    c->mem_w8((c->r[16] + (uint32_t)325), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)324), (uint8_t)c->r[0]);
    c->mem_w16((c->r[16] + (uint32_t)80), (uint16_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)328), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)4), (uint8_t)c->r[17]);
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[0]);
    c->r[31] = 0x8005F280u;
    c->mem_w8((c->r[16] + (uint32_t)7), (uint8_t)c->r[0]); func_8005344C(c);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_8005F2D4; }
    c->r[5] = c->r[0] + (uint32_t)2;
    c->r[6] = c->r[0] + (uint32_t)4;
    c->r[31] = 0x8005F298u;
    c->mem_w16((c->r[16] + (uint32_t)68), (uint16_t)c->r[0]); func_80054D14(c);
    c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[0]); goto L_8005F2D4;
  L_8005F2A0:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)74));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 9985);
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_8005F2D4; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)74));
    c->r[5] = c->r[0] + c->r[0];
    c->mem_w8((c->r[16] + (uint32_t)4), (uint8_t)c->r[17]);
    c->r[31] = 0x8005F2C8u;
    c->mem_w16((c->r[16] + (uint32_t)80), (uint16_t)c->r[2]); func_80056D44(c);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)80));
    c->mem_w16((c->r[16] + (uint32_t)74), (uint16_t)c->r[2]);
  L_8005F2D4:;
    c->r[31] = 0x8005F2DCu;
    c->r[4] = c->r[16] + c->r[0]; func_800551C4(c);
  L_8005F2DC:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

