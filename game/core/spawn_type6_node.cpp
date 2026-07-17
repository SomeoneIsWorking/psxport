// st6.cpp — PORT_GEN draft, byte-faithful transcription of gen_func_800310F4.
// ORACLE: gen_func_800310F4 (generated/shard_4.c:3123-3153)
// PORT_GEN: 0x800310F4 generated/shard_4.c:3123-3153
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
void func_8007A980(Core*);  // generated/shard_disp.c
void func_80028E10(Core*);  // generated/shard_disp.c

void Engine::spawnType6Node() {
  Core* c = core;
    c->r[2] = (uint32_t)32782u << 16;
    c->r[2] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)32380));
    c->r[29] = c->r[29] + (uint32_t)-32;
    c->mem_w32((c->r[29] + (uint32_t)24), c->r[18]);
    c->r[18] = c->r[4] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);
    c->r[17] = c->r[5] + c->r[0];
    c->mem_w32((c->r[29] + (uint32_t)28), c->r[31]);
    c->r[2] = (uint32_t)(c->r[2] < (uint32_t)7);
    { int _t = (c->r[2] != c->r[0]); c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]); if (_t) goto L_80031150; }
    c->r[4] = c->r[0] + c->r[0];
    c->r[5] = c->r[0] + (uint32_t)6;
    c->r[31] = 0x80031130u;
    c->r[6] = c->r[0] + (uint32_t)1; func_8007A980(c);
    c->r[16] = c->r[2] + c->r[0];
    { int _t = (c->r[16] == c->r[0]); c->r[4] = c->r[16] + c->r[0]; if (_t) goto L_80031150; }
    c->mem_w16((c->r[16] + (uint32_t)50), (uint16_t)c->r[17]);
    c->r[31] = 0x80031148u;
    c->r[5] = c->r[18] + c->r[0]; func_80028E10(c);
    c->r[2] = c->r[16] + c->r[0]; goto L_80031154;
  L_80031150:;
    c->r[2] = c->r[0] + c->r[0];
  L_80031154:;
    c->r[31] = c->mem_r32((c->r[29] + (uint32_t)28));
    c->r[18] = c->mem_r32((c->r[29] + (uint32_t)24));
    c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));
    c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));
    c->r[29] = c->r[29] + (uint32_t)32; return;
    return;
}

// ---- ownership wiring: FUN_800310F4 — spawn a TYPE-6 pool node with a param. If the active-object
// free-count (0x800E7E7C) >= 7 and Spawn (FUN_8007A980(0,6,1)) succeeds, store param_2 at node+0x32 and
// run its init (FUN_80028E10, node, param_1); return the node (r2) or 0. Called from the owned
// Engine::fieldFrame (engine.cpp:1063) + ActorTomba. Byte-faithful; spawn/init leaves stay substrate. ----
extern void shard_set_override(uint32_t, void (*)(Core*));
extern void gen_func_800310F4(Core*);
namespace { void ov_spawnType6Node(Core* c) { eng(c).spawnType6Node(); } }
void Engine::registerSpawnType6Node() {
  overrides::install(0x800310F4u, "Engine::spawnType6Node", ov_spawnType6Node,
                     gen_func_800310F4, shard_set_override);
}
