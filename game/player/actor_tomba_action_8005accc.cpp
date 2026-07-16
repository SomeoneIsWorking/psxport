// actor_tomba.cpp — PORT_GEN draft, byte-faithful transcription of gen_func_8005ACC8.
// ORACLE: gen_func_8005ACC8 (generated/shard_5.c:8935-9054)
// PORT_GEN: 0x8005ACC8 generated/shard_5.c:8935-9054
//
// This body is the gen function's guest-visible operations VERBATIM — every c->r[] op,
// mem_r/mem_w call, func_X/rec_dispatch call with its r31 constant, and label/goto is
// preserved unchanged. Faithful by construction; the only allowed next step is RENAMING
// (locals/labels -> named fields/control-flow), verified equivalent by tools/port_check.py.
// WIRED 2026-07-16 via ActorTomba::registerOverrides (engine_set_override_main 0x8005ACC8); port_check
// PASS + MIRROR_VERIFY-gated. Verbatim PORT_GEN body — RENAMING into named fields is a future step.
#include "actor_tomba.h"
#include "core.h"

void rec_dispatch(Core*, uint32_t);  // overlay_router.cpp — shared dispatch choke point
void func_80055E28(Core*);  // generated/shard_disp.c
void func_80055FBC(Core*);  // generated/shard_disp.c
void func_80056B48(Core*);  // generated/shard_disp.c
void func_80054E80(Core*);  // generated/shard_disp.c
void func_80076D68(Core*);  // generated/shard_disp.c
void func_80055D5C(Core*);  // generated/shard_disp.c
void func_8005444C(Core*);  // generated/shard_disp.c
void func_800532A0(Core*);  // generated/shard_disp.c
void func_80055390(Core*);  // generated/shard_disp.c
void func_800558B4(Core*);  // generated/shard_disp.c
void func_80054D14(Core*);  // generated/shard_disp.c
void func_80055C30(Core*);  // generated/shard_disp.c
void func_800559F4(Core*);  // generated/shard_disp.c
void func_800551C4(Core*);  // generated/shard_disp.c
void func_800538E0(Core*);  // generated/shard_disp.c
void func_8005A714(Core*);  // generated/shard_disp.c
void func_80056C00(Core*);  // generated/shard_disp.c

void ActorTomba::actionHandler8005ACC8() {
  Core* c = core;
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[16] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)6));
    { int _t = (c->r[3] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_8005ACFC; }
    { int _t = (c->r[3] == c->r[2]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_8005AD1C; }
     goto L_8005AED0;
  L_8005ACFC:;
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)6));
    c->r[2] = c->r[0] + (uint32_t)7;
    c->mem_w16((c->r[16] + (uint32_t)64), (uint16_t)c->r[2]);
    c->mem_w16((c->r[16] + (uint32_t)66), (uint16_t)c->r[0]);
    c->mem_w8((c->r[16] + (uint32_t)7), (uint8_t)c->r[0]);
    c->r[3] = c->r[3] + (uint32_t)1;
    c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[3]);
    c->r[4] = c->r[16] + c->r[0];
  L_8005AD1C:;
    c->r[31] = 0x8005AD24u;
    c->r[5] = c->r[0] + c->r[0]; func_80055E28(c);
    c->r[3] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)330));
    c->r[2] = c->r[3] & 2u;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = c->r[3] & 1u; if (_t) goto L_8005AD3C; }
    c->mem_w8((c->r[16] + (uint32_t)327), (uint8_t)c->r[2]);
  L_8005AD3C:;
    c->r[5] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)330));
    c->r[31] = 0x8005AD48u;
    c->r[4] = c->r[16] + c->r[0]; func_80055FBC(c);
    c->r[4] = c->r[16] + c->r[0];
    c->r[31] = 0x8005AD54u;
    c->r[5] = c->r[0] + c->r[0]; func_80056B48(c);
    c->r[4] = c->r[16] + c->r[0];
    c->r[31] = 0x8005AD60u;
    c->r[5] = c->r[0] + c->r[0]; func_80054E80(c);
    c->r[31] = 0x8005AD68u;
    c->r[4] = c->r[16] + c->r[0]; func_80076D68(c);
    c->r[31] = 0x8005AD70u;
    c->r[4] = c->r[16] + c->r[0]; func_80055D5C(c);
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)50));
    c->r[4] = c->r[16] + c->r[0];
    c->r[2] = c->r[2] + (uint32_t)8;
    c->r[31] = 0x8005AD84u;
    c->mem_w16((c->r[16] + (uint32_t)50), (uint16_t)c->r[2]); func_8005444C(c);
    c->r[31] = 0x8005AD8Cu;
    c->r[4] = c->r[16] + c->r[0]; func_800532A0(c);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_8005AE20; }
    c->r[31] = 0x8005AD9Cu;
    c->r[4] = c->r[16] + c->r[0]; func_80055390(c);
    c->r[17] = c->r[2] + c->r[0];
    { int _t = (c->r[17] != c->r[0]);  if (_t) goto L_8005AE00; }
    c->r[31] = 0x8005ADB0u;
    c->r[4] = c->r[16] + c->r[0]; func_800558B4(c);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_8005AE00; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)329));
    c->r[2] = c->r[2] & 2u;
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8005AE00; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)357));
    c->mem_w8((c->r[16] + (uint32_t)5), (uint8_t)c->r[0]);
    { int _t = (c->r[2] == c->r[0]); c->mem_w8((c->r[16] + (uint32_t)6), (uint8_t)c->r[0]); if (_t) goto L_8005ADEC; }
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)17;
    c->r[6] = c->r[0] + (uint32_t)4; goto L_8005ADF8;
  L_8005ADEC:;
    c->r[4] = c->r[16] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)2;
    c->r[6] = c->r[0] + (uint32_t)8;
  L_8005ADF8:;
    c->r[31] = 0x8005AE00u;
     func_80054D14(c);
  L_8005AE00:;
    c->r[31] = 0x8005AE08u;
    c->r[4] = c->r[16] + c->r[0]; func_80055C30(c);
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_8005AE20; }
    { int _t = (c->r[17] != c->r[0]);  if (_t) goto L_8005AE20; }
    c->r[31] = 0x8005AE20u;
    c->r[4] = c->r[16] + c->r[0]; func_800559F4(c);
  L_8005AE20:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)329));
    c->r[2] = c->r[2] & 4u;
    { int _t = (c->r[2] != c->r[0]); c->r[2] = (uint32_t)8064u << 16; if (_t) goto L_8005AE44; }
    c->r[31] = 0x8005AE3Cu;
    c->r[4] = c->r[16] + c->r[0]; func_800551C4(c);
     goto L_8005AE64;
  L_8005AE44:;
    c->r[2] = (uint32_t)c->mem_r16((c->r[2] + (uint32_t)380));
    c->r[2] = c->r[2] & 3u;
    { int _t = (c->r[2] != c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_8005AE64; }
    c->r[5] = c->r[16] + (uint32_t)44;
    c->r[31] = 0x8005AE64u;
    c->r[6] = c->r[0] + c->r[0]; func_800538E0(c);
  L_8005AE64:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)41));
    { int _t = (c->r[2] == c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_8005AE9C; }
    c->r[2] = (uint32_t)c->mem_r16((c->r[16] + (uint32_t)64));
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->mem_w16((c->r[16] + (uint32_t)64), (uint16_t)c->r[2]);
    c->r[2] = c->r[2] << 16;
    { int _t = (c->r[2] != c->r[0]);  if (_t) goto L_8005AE9C; }
    c->r[31] = 0x8005AE98u;
    c->r[4] = c->r[16] + c->r[0]; func_8005A714(c);
    c->r[4] = c->r[16] + c->r[0];
  L_8005AE9C:;
    c->r[31] = 0x8005AEA4u;
    c->r[5] = c->r[0] + c->r[0]; func_80056C00(c);
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)5));
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)2);
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_8005AED0; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[16] + (uint32_t)41));
    { int _t = (c->r[2] == c->r[0]); c->r[3] = (uint32_t)32780u << 16; if (_t) goto L_8005AED0; }
    c->r[2] = c->r[0] + (uint32_t)1;
    c->mem_w8((c->r[3] + (uint32_t)-2018), (uint8_t)c->r[2]);
  L_8005AED0:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

