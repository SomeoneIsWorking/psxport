// actor_tomba.cpp — PORT_GEN draft, byte-faithful transcription of gen_func_8005EF48.
// ORACLE: gen_func_8005EF48 (generated/shard_6.c:9432-9570)
// PORT_GEN: 0x8005EF48 generated/shard_6.c:9432-9570
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
void func_80053D90(Core*);  // generated/shard_disp.c
void func_80054E24(Core*);  // generated/shard_disp.c
void func_80056B48(Core*);  // generated/shard_disp.c
void func_80055D5C(Core*);  // generated/shard_disp.c
void func_80076D68(Core*);  // generated/shard_disp.c
void func_80055824(Core*);  // generated/shard_disp.c
void func_8005444C(Core*);  // generated/shard_disp.c
void func_80056D44(Core*);  // generated/shard_disp.c
void func_80055E28(Core*);  // generated/shard_disp.c
void func_800574E0(Core*);  // generated/shard_disp.c
void func_80057C08(Core*);  // generated/shard_disp.c
void func_80054D14(Core*);  // generated/shard_disp.c

void ActorTomba::actionHandler8005EF48() {
  Core* c = core;
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[31]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)6));
    c->r[2] = c->r[0] + (uint32_t)1;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)((int32_t)c->r[3] < 2); if (_t) goto L_8005EFE8; }
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8005EF80; }
    { int _t = (c->r[3] == c->r[0]);  if (_t) goto L_8005EF94; }
    c->mem_w8((c->r[16] + (uint32_t)362), (uint8_t)c->r[0]); goto L_8005F1A0;
  L_8005EF80:;
    c->r[2] = c->r[0] + (uint32_t)2;
    { int _t = (c->r[3] == c->r[2]);  if (_t) goto L_8005F0F0; }
    c->mem_w8((c->r[16] + (uint32_t)362), (uint8_t)c->r[0]); goto L_8005F1A0;
  L_8005EF94:;
    c->r[31] = 0x8005EF9Cu;
    c->r[4] = c->r[16] + c->r[0]; func_80053D90(c);
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] + c->r[0];
    c->r[6] = c->r[5] + c->r[0];
    c->mem_w8((c->r[16] + (uint32_t)325), (uint8_t)c->r[0]);
    c->r[31] = 0x8005EFB4u;
    c->mem_w8((c->r[16] + (uint32_t)326), (uint8_t)c->r[0]); func_80054E24(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)6));
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)362));
    c->mem_w16((c->r[16] + (uint32_t)74), (uint16_t)c->r[0]);
    c->r[2] = c->r[2] + (uint32_t)1;
    c->r[3] = c->r[3] & 1u;
    { int _t = (c->r[3] == c->r[0]); c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[2]); if (_t) goto L_8005EFDC; }
    c->r[2] = c->r[0] + (uint32_t)-2048;
    c->mem_w16((c->r[16] + (uint32_t)68), (uint16_t)c->r[2]); goto L_8005F19C;
  L_8005EFDC:;
    c->r[2] = c->r[0] + (uint32_t)2048;
    c->mem_w16((c->r[16] + (uint32_t)68), (uint16_t)c->r[2]); goto L_8005F19C;
  L_8005EFE8:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[16] + (uint32_t)68));
    { int _t = ((int32_t)c->r[2] >= 0);  if (_t) goto L_8005EFFC; }
    c->r[2] = c->r[0] - c->r[2];
  L_8005EFFC:;
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 2048);
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_8005F028; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)362));
    c->r[2] = c->r[2] & 1u;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)-2048; if (_t) goto L_8005F020; }
    c->r[2] = c->r[0] + (uint32_t)2048;
  L_8005F020:;
    c->mem_w16((c->r[16] + (uint32_t)68), (uint16_t)c->r[2]);
    c->r[4] = c->r[16] + c->r[0];
  L_8005F028:;
    c->r[31] = 0x8005F030u;
    c->r[5] = c->r[0] + (uint32_t)1; func_80056B48(c);
    c->r[31] = 0x8005F038u;
    c->r[4] = c->r[16] + c->r[0]; func_80055D5C(c);
    c->r[31] = 0x8005F040u;
    c->r[4] = c->r[16] + c->r[0]; func_80076D68(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)41));
    c->r[2] = c->r[2] & 128u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8005F06C; }
    c->r[31] = 0x8005F05Cu;
     func_80055824(c);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)2; if (_t) goto L_8005F06C; }
    c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[2]); goto L_8005F0A8;
  L_8005F06C:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)50));
    c->r[4] = c->r[16] + c->r[0];
    c->r[2] = c->r[2] + (uint32_t)8;
    c->r[31] = 0x8005F080u;
    c->mem_w16((c->r[16] + (uint32_t)50), (uint16_t)c->r[2]); func_8005444C(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)41));
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_8005F0B4; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)362));
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_8005F19C; }
    c->mem_w16((c->r[16] + (uint32_t)68), (uint16_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[0]);
  L_8005F0A8:;
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)7), (uint8_t)c->r[0]); goto L_8005F19C;
  L_8005F0B4:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)50));
    c->r[5] = c->r[0] + c->r[0];
    c->mem_w16((c->r[16] + (uint32_t)68), (uint16_t)c->r[0]);
    c->r[2] = c->r[2] + (uint32_t)64;
    c->r[31] = 0x8005F0CCu;
    c->mem_w16((c->r[16] + (uint32_t)50), (uint16_t)c->r[2]); func_80056D44(c);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)5));
    c->r[2] = c->r[0] + (uint32_t)2;
    { int _t = (c->r[3] != c->r[2]); c->r[2] = c->r[0] + (uint32_t)10; if (_t) goto L_8005F19C; }
    c->mem_w16((c->r[16] + (uint32_t)64), (uint16_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)15;
    c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[2]);
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[3]); goto L_8005F19C;
  L_8005F0F0:;
    c->r[31] = 0x8005F0F8u;
    c->r[4] = c->r[16] + c->r[0]; func_80076D68(c);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)362));
    c->r[2] = c->r[3] & 2u;
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[3] & 1u; if (_t) goto L_8005F11C; }
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[0] + (uint32_t)-2048; if (_t) goto L_8005F118; }
    c->r[2] = c->r[0] + (uint32_t)2048;
  L_8005F118:;
    c->mem_w16((c->r[16] + (uint32_t)68), (uint16_t)c->r[2]);
  L_8005F11C:;
    c->r[4] = c->r[16] + c->r[0];
    c->r[31] = 0x8005F128u;
    c->r[5] = c->r[0] + (uint32_t)1; func_80055E28(c);
    c->r[4] = c->r[16] + c->r[0];
    c->r[31] = 0x8005F134u;
    c->r[5] = c->r[0] + (uint32_t)1; func_80056B48(c);
    c->r[4] = c->r[16] + c->r[0];
    c->r[31] = 0x8005F140u;
    c->r[5] = c->r[0] + c->r[0]; func_800574E0(c);
    c->r[31] = 0x8005F148u;
    c->r[4] = c->r[16] + c->r[0]; func_80055D5C(c);
    c->r[4] = c->r[16] + c->r[0];
    c->r[31] = 0x8005F154u;
    c->r[5] = c->r[0] + (uint32_t)2; func_80057C08(c);
    c->r[2] = c->mem_r32((c->r[16] + (uint32_t)344));
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_8005F19C; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)64));
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w16((c->r[16] + (uint32_t)64), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_8005F19C; }
    c->r[5] = c->r[0] + (uint32_t)20;
    c->r[31] = 0x8005F18Cu;
    c->r[6] = c->r[0] + c->r[0]; func_80054D14(c);
    c->r[2] = c->r[0] + (uint32_t)2;
    c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[2]);
  L_8005F19C:;
    c->mem_w8((c->r[16] + (uint32_t)362), (uint8_t)c->r[0]);
  L_8005F1A0:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

