// actor_tomba.cpp — PORT_GEN draft, byte-faithful transcription of gen_func_8005AEE4.
// ORACLE: gen_func_8005AEE4 (generated/shard_6.c:8984-9121)
// PORT_GEN: 0x8005AEE4 generated/shard_6.c:8984-9121
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
void func_80055E28(Core*);  // generated/shard_disp.c
void func_80055FBC(Core*);  // generated/shard_disp.c
void func_80056B48(Core*);  // generated/shard_disp.c
void func_80055D5C(Core*);  // generated/shard_disp.c
void func_80074590(Core*);  // generated/shard_disp.c
void func_80054D14(Core*);  // generated/shard_disp.c
void func_800538E0(Core*);  // generated/shard_disp.c
void func_80076D68(Core*);  // generated/shard_disp.c
void func_800574E0(Core*);  // generated/shard_disp.c
void func_80057C08(Core*);  // generated/shard_disp.c
void func_8005444C(Core*);  // generated/shard_disp.c
void func_800532A0(Core*);  // generated/shard_disp.c
void func_80055390(Core*);  // generated/shard_disp.c
void func_800558B4(Core*);  // generated/shard_disp.c
void func_80054E80(Core*);  // generated/shard_disp.c
void func_80055C30(Core*);  // generated/shard_disp.c
void func_800559F4(Core*);  // generated/shard_disp.c
void func_80056C00(Core*);  // generated/shard_disp.c
void func_800551C4(Core*);  // generated/shard_disp.c

void ActorTomba::actionHandler8005AEE4() {
  Core* c = core;
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[0] + (uint32_t)1;
    c->r[5] = c->r[17] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]);
    c->r[31] = 0x8005AF0Cu;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]); func_80055E28(c);
    c->r[5] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)330));
    c->r[31] = 0x8005AF18u;
    c->r[4] = c->r[16] + c->r[0]; func_80055FBC(c);
    c->r[4] = c->r[16] + c->r[0];
    c->r[31] = 0x8005AF24u;
    c->r[5] = c->r[17] + c->r[0]; func_80056B48(c);
    c->r[31] = 0x8005AF2Cu;
    c->r[4] = c->r[16] + c->r[0]; func_80055D5C(c);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)6));
    { int _t = (c->r[3] == c->r[17]); c->r[19] = c->r[17] + c->r[0]; if (_t) goto L_8005AFC0; }
    c->r[2] = (uint32_t)((int32_t)c->r[3] < 2);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_8005AF58; }
    { int _t = (c->r[3] == c->r[0]); c->r[4] = c->r[0] + (uint32_t)29; if (_t) goto L_8005AF68; }
     goto L_8005B110;
  L_8005AF58:;
    { int _t = (c->r[3] == c->r[2]);  if (_t) goto L_8005AFE8; }
     goto L_8005B110;
  L_8005AF68:;
    c->r[5] = c->r[0] + c->r[0];
    c->r[31] = 0x8005AF74u;
    c->r[6] = c->r[5] + c->r[0]; func_80074590(c);
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)20;
    c->r[6] = c->r[0] + (uint32_t)2;
    c->r[31] = 0x8005AF88u;
    c->mem_w16((c->r[16] + (uint32_t)88), (uint16_t)c->r[0]); func_80054D14(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)385));
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_8005AFAC; }
    c->r[5] = c->r[16] + (uint32_t)44;
    c->r[31] = 0x8005AFA4u;
    c->r[6] = c->r[0] + c->r[0]; func_800538E0(c);
     goto L_8005AFB0;
  L_8005AFAC:;
    c->mem_w8((c->r[16] + (uint32_t)385), (uint8_t)c->r[0]);
  L_8005AFB0:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)6));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[2]);
  L_8005AFC0:;
    c->r[31] = 0x8005AFC8u;
    c->r[4] = c->r[16] + c->r[0]; func_80076D68(c);
    c->r[4] = c->r[16] + c->r[0];
    c->r[31] = 0x8005AFD4u;
    c->r[5] = c->r[0] + c->r[0]; func_800574E0(c);
    c->r[4] = c->r[16] + c->r[0];
    c->r[31] = 0x8005AFE0u;
    c->r[5] = c->r[0] + c->r[0]; func_80057C08(c);
     goto L_8005B110;
  L_8005AFE8:;
    c->r[31] = 0x8005AFF0u;
    c->r[4] = c->r[16] + c->r[0]; func_80076D68(c);
    c->r[4] = c->r[16] + c->r[0];
    c->r[3] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)50));
    c->r[18] = c->r[2] + c->r[0];
    c->r[3] = c->r[3] + (uint32_t)8;
    c->r[31] = 0x8005B008u;
    c->mem_w16((c->r[16] + (uint32_t)50), (uint16_t)c->r[3]); func_8005444C(c);
    c->r[31] = 0x8005B010u;
    c->r[4] = c->r[16] + c->r[0]; func_800532A0(c);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_8005B0D0; }
    c->r[31] = 0x8005B020u;
    c->r[4] = c->r[16] + c->r[0]; func_80055390(c);
    c->r[17] = c->r[2] + c->r[0];
    { int _t = (c->r[17] != c->r[0]);  if (_t) goto L_8005B0D0; }
    c->r[31] = 0x8005B034u;
    c->r[4] = c->r[16] + c->r[0]; func_800558B4(c);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_8005B0D0; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)330));
    c->r[4] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)327));
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)329));
    c->r[2] = c->r[2] & 14u;
    c->mem_w8((c->r[16] + (uint32_t)330), (uint8_t)c->r[2]);
    c->r[2] = c->r[2] | c->r[4];
    c->r[3] = c->r[3] & 2u;
    { int _t = (c->r[3] == c->r[0]); c->mem_w8((c->r[16] + (uint32_t)330), (uint8_t)c->r[2]); if (_t) goto L_8005B0AC; }
    { int _t = (c->r[18] != c->r[19]);  if (_t) goto L_8005B0D0; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)357));
    c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[0]);
    { int _t = (c->r[2] == c->r[0]); c->mem_w8((c->r[16] + (uint32_t)7), (uint8_t)c->r[0]); if (_t) goto L_8005B094; }
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)17;
    c->r[31] = 0x8005B08Cu;
    c->r[6] = c->r[0] + (uint32_t)2; func_80054D14(c);
     goto L_8005B0D0;
  L_8005B094:;
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)2;
    c->r[31] = 0x8005B0A4u;
    c->r[6] = c->r[0] + (uint32_t)6; func_80054D14(c);
     goto L_8005B0D0;
  L_8005B0AC:;
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[19]);
    c->r[31] = 0x8005B0C0u;
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[0]); func_80054E80(c);
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[16] + (uint32_t)44;
    c->r[31] = 0x8005B0D0u;
    c->r[6] = c->r[0] + c->r[0]; func_800538E0(c);
  L_8005B0D0:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)41));
    c->r[2] = c->r[2] & 128u;
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_8005B108; }
    c->r[31] = 0x8005B0ECu;
    c->r[4] = c->r[16] + c->r[0]; func_80055C30(c);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_8005B104; }
    { int _t = (c->r[17] != c->r[0]);  if (_t) goto L_8005B104; }
    c->r[31] = 0x8005B104u;
    c->r[4] = c->r[16] + c->r[0]; func_800559F4(c);
  L_8005B104:;
    c->r[4] = c->r[16] + c->r[0];
  L_8005B108:;
    c->r[31] = 0x8005B110u;
    c->r[5] = c->r[0] + c->r[0]; func_80056C00(c);
  L_8005B110:;
    c->r[31] = 0x8005B118u;
    c->r[4] = c->r[16] + c->r[0]; func_800551C4(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

