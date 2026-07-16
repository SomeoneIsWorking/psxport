// actor_tomba.cpp — PORT_GEN draft, byte-faithful transcription of gen_func_800531DC.
// ORACLE: gen_func_800531DC (generated/shard_6.c:8186-8224)
// PORT_GEN: 0x800531DC generated/shard_6.c:8186-8224
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

void ActorTomba::actionHandler800531DC() {
  Core* c = core;
    c->r[2] = (uint32_t)32780u << 16;
    c->r[5] = c->r[2] + (uint32_t)-1936;
    c->r[3] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)-1936));
    c->r[2] = c->r[0] + (uint32_t)5;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = (uint32_t)((int32_t)c->r[3] < 6); if (_t) goto L_8005323C; }
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)1; if (_t) goto L_8005320C; }
    { int _t = (c->r[3] == c->r[2]);  if (_t) goto L_80053220; }
  L_80053204:;
     return;
  L_8005320C:;
    c->r[2] = c->r[0] + (uint32_t)6;
    { int _t = (c->r[3] == c->r[2]); c->r[2] = c->r[0] + (uint32_t)14; if (_t) goto L_80053270; }
     return;
  L_80053220:;
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)50));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < -10586);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)-10586; if (_t) goto L_80053204; }
    c->mem_w16((c->r[4] + (uint32_t)50), (uint16_t)c->r[2]); return;
  L_8005323C:;
    c->r[2] = (uint32_t)c->mem_r8((c->r[5] + (uint32_t)1));
    c->r[2] = c->r[2] + (uint32_t)-1;
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)3);
    { int _t = (c->r[2] == c->r[0]);  if (_t) goto L_80053298; }
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)50));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < -15130);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)-15130; if (_t) goto L_80053204; }
    c->mem_w16((c->r[4] + (uint32_t)50), (uint16_t)c->r[2]); return;
  L_80053270:;
    c->r[3] = (uint32_t)c->mem_r8((c->r[4] + (uint32_t)42));
    { int _t = (c->r[3] != c->r[2]);  if (_t) goto L_80053298; }
    c->r[2] = (uint32_t)(int16_t)c->mem_r16((c->r[4] + (uint32_t)50));
    c->r[2] = (uint32_t)((int32_t)c->r[2] < -7637);
    { int _t = (c->r[2] == c->r[0]); c->r[2] = c->r[0] + (uint32_t)-7637; if (_t) goto L_80053204; }
    c->mem_w16((c->r[4] + (uint32_t)50), (uint16_t)c->r[2]);
  L_80053298:;
     return;
    return;
}

