// demo_leaf_b.cpp — PORT_GEN draft, byte-faithful transcription of gen_func_8002311C.
// ORACLE: gen_func_8002311C (generated/shard_5.c:2021-2029)
// PORT_GEN: 0x8002311C generated/shard_5.c:2021-2029
//
// This body is the gen function's guest-visible operations VERBATIM — every c->r[] op,
// mem_r/mem_w call, func_X/rec_dispatch call with its r31 constant, and label/goto is
// preserved unchanged. Faithful by construction; the only allowed next step is RENAMING
// (locals/labels -> named fields/control-flow), verified equivalent by tools/port_check.py.
// UNWIRED — dead code. Do not wire into any dispatch table before running port_check.py
// and the mandatory line-by-line verify pass (docs/fleet-workflow.md §9).
#include "core/demo_leaf_b.h"
#include "core.h"

void rec_dispatch(Core*, uint32_t);  // overlay_router.cpp — shared dispatch choke point
void func_8001EC3C(Core*);  // generated/shard_disp.c

void DemoLeafB::run() {
  Core* c = mCore;
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[31] = 0x8002312Cu;
     func_8001EC3C(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

