// actor_tomba.cpp — PORT_GEN draft, byte-faithful transcription of gen_func_800660AC.
// ORACLE: gen_func_800660AC (generated/shard_5.c:10052-10124)
// PORT_GEN: 0x800660AC generated/shard_5.c:10052-10124
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
void func_80076D68(Core*);  // generated/shard_disp.c
void func_80054D14(Core*);  // generated/shard_disp.c
void func_80074590(Core*);  // generated/shard_disp.c
void func_80065478(Core*);  // generated/shard_disp.c

void ActorTomba::actionHandler800660AC() {
  Core* c = core;
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[31] = 0x800660C8u;
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]); func_80076D68(c);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)6));
    c->r[17] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[3] == c->r[17]); c->r[18] = c->r[2] + c->r[0]; if (_t) goto L_80066124; }
    c->r[2] = (uint32_t)((int32_t)c->r[3] < 2);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_800660F4; }
    { int _t = (c->r[3] == c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_80066104; }
     goto L_800661C0;
  L_800660F4:;
    { int _t = (c->r[3] == c->r[2]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_8006617C; }
     goto L_800661C0;
  L_80066104:;
    c->r[5] = c->r[0] + (uint32_t)199;
    c->r[6] = c->r[0] + (uint32_t)3;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[31] = 0x80066118u;
    c->mem_w8((c->r[2] + (uint32_t)-2034), (uint8_t)c->r[0]); func_80054D14(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)6));
    c->mem_w16((c->r[16] + (uint32_t)64), (uint16_t)c->r[0]); goto L_80066170;
  L_80066124:;
    c->r[3] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)64));
    c->r[2] = c->r[0] + (uint32_t)12;
    { int _t = (c->r[3] != c->r[2]); c->r[4] = c->r[0] + (uint32_t)38; if (_t) goto L_80066140; }
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x80066140u;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
  L_80066140:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)64));
    c->r[2] = c->r[2] + (uint32_t)1;
    { int _t = (c->r[18] != c->r[17]); c->mem_w16((c->r[16] + (uint32_t)64), (uint16_t)c->r[2]); if (_t) goto L_800661BC; }
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)2;
    c->r[31] = 0x80066164u;
    c->r[6] = c->r[0] + (uint32_t)6; func_80054D14(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)6));
    c->r[3] = c->r[0] + (uint32_t)7;
    c->mem_w16((c->r[16] + (uint32_t)64), (uint16_t)c->r[3]);
  L_80066170:;
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[2]); goto L_800661BC;
  L_8006617C:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)64));
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w16((c->r[16] + (uint32_t)64), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_800661C0; }
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)311));
    { int _t = (c->r[3] != c->r[17]); c->r[2] = c->r[0] + (uint32_t)34; if (_t) goto L_800661B4; }
    c->r[2] = (uint32_t)32780u << 16;
    c->mem_w8((c->r[2] + (uint32_t)-2034), (uint8_t)c->r[3]);
    c->r[2] = c->r[0] + (uint32_t)34;
  L_800661B4:;
    c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[2]);
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[0]);
  L_800661BC:;
    c->r[4] = c->r[16] + c->r[0];
  L_800661C0:;
    c->r[31] = 0x800661C8u;
    c->r[5] = c->r[0] + (uint32_t)1; func_80065478(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

