// TransitionState3::walkOnce — see transition_state3.h.
//
// Faithful port of guest FUN_8007B04C (decomp scratch/decomp/ram_f1000_all.c L56987-L57017).
// Uses the shared `dispatch_obj_method` helper (game/object/engine_tomba2.cpp) so behavior
// dispatch routes through the same native-or-substrate table used by the other native walkers.
#include "transition_state3.h"
#include "core.h"
#include "tomba2_types.h"        // T2_OBJLIST_HEAD_1/2, T2OBJ_HANDLER/NEXT/RENDER_FLAG

// Shared behavior-dispatch helper — defined in game/object/engine_tomba2.cpp.
void dispatch_obj_method(Core* c, uint32_t obj, uint32_t h);

void TransitionState3::walkOnce() {
  Core* c = core;
  uint32_t l2 = c->mem_r32(T2_OBJLIST_HEAD_2);           // captured up-front (decomp: iVar3 = DAT_800f2624)
  uint32_t n  = c->mem_r32(T2_OBJLIST_HEAD_1);
  while (n) {
    uint32_t next = c->mem_r32(n + T2OBJ_NEXT);
    c->mem_w8 (n + T2OBJ_RENDER_FLAG, 0);
    if (c->mem_r8(n + 0x28) & 0x80) {
      uint32_t h = c->mem_r32(n + T2OBJ_HANDLER);
      dispatch_obj_method(c, n, h);
    }
    l2 = c->mem_r32(T2_OBJLIST_HEAD_2);                  // re-read each iteration (handler may mutate)
    n  = next;
  }
  n = l2;
  while (n) {
    uint32_t next = c->mem_r32(n + T2OBJ_NEXT);
    c->mem_w8 (n + T2OBJ_RENDER_FLAG, 0);
    if (c->mem_r8(n + 0x28) & 0x80) {
      uint32_t h = c->mem_r32(n + T2OBJ_HANDLER);
      dispatch_obj_method(c, n, h);
    }
    n = next;
  }
}
