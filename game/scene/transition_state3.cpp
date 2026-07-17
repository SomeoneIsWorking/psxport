// TransitionState3::walkOnce — see transition_state3.h.
//
// Faithful port of guest FUN_8007B04C (decomp scratch/decomp/ram_f1000_all.c L56987-L57017).
// Behavior dispatch routes through `eng(c).behaviors.dispatchObj` — the same native-or-
// substrate table used by the other native walkers (ObjectList / Array8Dispatch).
#include "transition_state3.h"
#include "game_ctx.h"
#include "core.h"
#include "tomba2_types.h"        // T2_OBJLIST_HEAD_1/2, T2OBJ_HANDLER/NEXT/RENDER_FLAG

void TransitionState3::walkOnce() {
  Core* c = core;
  uint32_t l2 = c->mem_r32(T2_OBJLIST_HEAD_2);           // captured up-front (decomp: iVar3 = DAT_800f2624)
  uint32_t n  = c->mem_r32(T2_OBJLIST_HEAD_1);
  while (n) {
    uint32_t next = c->mem_r32(n + T2OBJ_NEXT);
    c->mem_w8 (n + T2OBJ_RENDER_FLAG, 0);
    if (c->mem_r8(n + 0x28) & 0x80) {
      uint32_t h = c->mem_r32(n + T2OBJ_HANDLER);
      eng(c).behaviors.dispatchObj(n, h);
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
      eng(c).behaviors.dispatchObj(n, h);
    }
    n = next;
  }
}
