// acue.cpp — PORT_GEN draft, byte-faithful transcription of gen_func_8004FA38.
// ORACLE: gen_func_8004FA38 (generated/shard_5.c:7466-7520)
// PORT_GEN: 0x8004FA38 generated/shard_5.c:7466-7520
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
void func_8004F8DC(Core*);  // generated/shard_disp.c
void func_8004EE2C(Core*);  // generated/shard_disp.c

void Engine::announcerCuePush() {
  Core* c = core;
    c->r[29] = c->r[29] + (uint32_t)-40;
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[19]);
    c->r[19] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[18] = c->r[5] + c->r[0];
    c->r[2] = (uint32_t)32780u << 16;
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[2] + (uint32_t)-2744;
    c->mem_w32((c->r[29] + (uint32_t)36), c->r[31]);
    c->mem_w32((c->r[29] + (uint32_t)32), c->r[20]);
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[17] + (uint32_t)10));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < 8);
    { int _t = (c->r[2] != c->r[0]); c->r[20] = c->r[6] + c->r[0]; if (_t) goto L_8004FA80; }
    c->r[31] = 0x8004FA80u;
    c->r[4] = c->r[17] + c->r[0]; func_8004F8DC(c);
  L_8004FA80:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[17] + (uint32_t)10));
    c->r[2] = c->r[2] << 5;
    c->r[2] = c->r[2] + (uint32_t)432;
    c->r[16] = c->r[2] + c->r[17];
    c->r[31] = 0x8004FA9Cu;
    c->r[4] = c->r[16] + c->r[0]; func_8004EE2C(c);
    c->r[2] = c->r[18] << 16;
    c->r[2] = (uint32_t)((int32_t)c->r[2] >> 16);
    c->r[3] = c->r[0] + (uint32_t)-1;
    { int _t = (c->r[2] != c->r[3]); c->r[2] = c->r[0] | 65534u; if (_t) goto L_8004FAB8; }
    c->mem_w32((c->r[16] + (uint32_t)0), c->r[19]); goto L_8004FAEC;
  L_8004FAB8:;
    c->r[4] = c->r[19] + c->r[0];
    c->r[3] = (uint32_t)c->mem_r16((c->r[19] + (uint32_t)0));
    { int _t = (c->r[3] == c->r[2]); c->r[5] = c->r[16] + (uint32_t)4; if (_t) goto L_8004FAE4; }
  L_8004FACC:;
    c->r[4] = c->r[4] + (uint32_t)2;
    c->mem_w16((c->r[5] + (uint32_t)0), (uint16_t)c->r[3]);
    c->r[3] = (uint32_t)c->mem_r16((c->r[4] + (uint32_t)0));
    { int _t = (c->r[3] != c->r[2]); c->r[5] = c->r[5] + (uint32_t)2; if (_t) goto L_8004FACC; }
  L_8004FAE4:;
    c->r[2] = c->r[0] | 65534u;
    c->mem_w16((c->r[5] + (uint32_t)0), (uint16_t)c->r[2]);
  L_8004FAEC:;
    c->mem_w8((c->r[16] + (uint32_t)28), (uint8_t)c->r[20]);
    c->r[2] = (uint32_t)c->mem_r16((c->r[17] + (uint32_t)10));
    c->r[2] = c->r[2] + (uint32_t)1;
    c->mem_w16((c->r[17] + (uint32_t)10), (uint16_t)c->r[2]);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)36));
    c->r[20] = c->mem_r32((c->r[29] + (uint32_t)32));
    c->r[19] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)40; return;
    return;
}

// ---- ownership wiring: FUN_8004FA38 — the announcer/message-CUE QUEUE PUSH (8-slot ring at 0x800BF6F8,
// stride 0x20; flushes via FUN_8004F8DC at overflow; param_2==-1 stores the message pointer, else copies
// the inline -2-terminated short string; param_3 = a per-entry flag). Called every field frame from the
// owned Engine::fieldFrame. Byte-faithful (gen_func_8004FA38 = oracle leg); the flush/clear leaves stay
// substrate. ----
extern void shard_set_override(uint32_t, void (*)(Core*));
extern void gen_func_8004FA38(Core*);
namespace { void ov_announcerCuePush(Core* c) { eng(c).announcerCuePush(); } }
void Engine::registerAnnouncerCuePush() {
  overrides::install(0x8004FA38u, "Engine::announcerCuePush", ov_announcerCuePush,
                     gen_func_8004FA38, shard_set_override);
}
