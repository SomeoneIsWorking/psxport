// actor_tomba.cpp — PORT_GEN draft, byte-faithful transcription of gen_func_800588BC.
// ORACLE: gen_func_800588BC (generated/shard_0.c:7918-7939)
// PORT_GEN: 0x800588BC generated/shard_0.c:7918-7939
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

void ActorTomba::actionHandler800588BC() {
  Core* c = core;
    c->r[2] = (uint32_t)8064u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)311));
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80058910; }
    c->r[2] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)356));
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)3; if (_t) goto L_80058910; }
    c->r[3] = (uint32_t)32780u << 16;
    c->mem_w8((c->r[4] + (uint32_t)0), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)1;
    c->mem_w8((c->r[3] + (uint32_t)-2034), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)4;
    c->mem_w8((c->r[4] + (uint32_t)4), (uint8_t)c->r[2]);
    c->r[2] = c->r[0] + (uint32_t)8;
    c->mem_w8((c->r[4] + (uint32_t)361), (uint8_t)c->r[2]);
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)358), (uint16_t)c->r[0]);
    c->r[2] = (uint32_t)8064u << 16;
    c->mem_w16((c->r[2] + (uint32_t)400), (uint16_t)c->r[0]);
  L_80058910:;
     return;
    return;
}

