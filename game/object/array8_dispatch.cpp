// Array8Dispatch::tick — see array8_dispatch.h. Faithful port of guest FUN_80026368.
#include "array8_dispatch.h"
#include "core.h"

void dispatch_obj_method(Core* c, uint32_t obj, uint32_t h);   // shared helper (engine_tomba2.cpp)

void Array8Dispatch::tick() {
  Core* c = core;
  for (int i = 0; i < 8; i++) {
    uint32_t slot = ARRAY_BASE + (uint32_t)i * SLOT_STRIDE;
    if (c->mem_r8(slot) == 0) continue;
    uint32_t type = c->mem_r8(slot + 2);
    uint32_t h    = c->mem_r32(METHOD_TABLE + type * 4u);
    dispatch_obj_method(c, slot, h);
  }
}
