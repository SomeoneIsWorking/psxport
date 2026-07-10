// demo_leaf_a.cpp — PORT_GEN draft, byte-faithful transcription of gen_func_8001CE90.
// ORACLE: gen_func_8001CE90 (generated/shard_1.c:1221-1229)
// PORT_GEN: 0x8001CE90 generated/shard_1.c:1221-1229
//
// This body is the gen function's guest-visible operations VERBATIM — every c->r[] op,
// mem_r/mem_w call, func_X/rec_dispatch call with its r31 constant, and label/goto is
// preserved unchanged. Faithful by construction; the only allowed next step is RENAMING
// (locals/labels -> named fields/control-flow), verified equivalent by tools/port_check.py.
// UNWIRED — dead code. Do not wire into any dispatch table before running port_check.py
// and the mandatory line-by-line verify pass (docs/fleet-workflow.md §9).
#include "core/demo_leaf_a.h"
#include "core.h"

void rec_dispatch(Core*, uint32_t);  // overlay_router.cpp — shared dispatch choke point
void func_8001CE04(Core*);  // generated/shard_disp.c

void DemoLeafA::run() {
  Core* c = mCore;
    c->r[29] = c->r[29] + (uint32_t)-24;
    c->mem_w32((c->r[29] + (uint32_t)16), c->r[31]);
    c->r[31] = 0x8001CEA0u;
    c->r[4] = c->r[4] & 255u; func_8001CE04(c);
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)24; return;
    return;
}

