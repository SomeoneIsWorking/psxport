// Array8Dispatch::tick — see array8_dispatch.h. Faithful port of guest FUN_80026368.
#include "array8_dispatch.h"
#include "game_ctx.h"
#include "core.h"
#include "game.h"   // c->game->pc_skip fork

void Array8Dispatch::tick() {
  Core* c = core;
  if (c->game && !c->game->pc_skip) { tickFaithful(); return; }
  for (int i = 0; i < 8; i++) {
    uint32_t slot = ARRAY_BASE + (uint32_t)i * SLOT_STRIDE;
    if (c->mem_r8(slot) == 0) continue;
    uint32_t type = c->mem_r8(slot + 2);
    uint32_t h    = c->mem_r32(METHOD_TABLE + type * 4u);
    eng(c).behaviors.dispatchObj(slot, h);
  }
}

// tickFaithful(): line-for-line mirror of gen_func_80026368 (generated/shard_5.c). The gen spills
// s0/s1/s2/ra to its guest-stack frame before the loop and restores them after, and sets the
// jal-site link register (r31 = 0x800263C0, the PC right after the loop's single `jal`) immediately
// before every dispatch -- both are guest-visible bytes that the plain host loop in tick() does not
// reproduce.
void Array8Dispatch::tickFaithful() {
  Core* c = core;
  c->r[29] -= 32;                              // gen 80026368 prologue: s0/s1/s2 + ra
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 16, c->r[16]);
  c->r[16] = ARRAY_BASE;                       // 0x80100400
  c->mem_w32(sp + 20, c->r[17]);
  c->r[17] = 0;
  c->mem_w32(sp + 24, c->r[18]);
  c->r[18] = METHOD_TABLE;                     // 0x8009D314
  c->mem_w32(sp + 28, c->r[31]);
  while ((int32_t)c->r[17] < 8) {
    uint32_t slot = c->r[16];
    if (c->mem_r8(slot) != 0) {
      uint32_t type = c->mem_r8(slot + 2);
      uint32_t h    = c->mem_r32(c->r[18] + type * 4u);
      c->r[31] = 0x800263C0u;                  // gen's jal-site ra for this call, every iteration
      c->r[4]  = slot;
      eng(c).behaviors.dispatchObj(slot, h); // r31 already set for the callee's own prologue spill
    }
    c->r[17] += 1;
    c->r[16] += SLOT_STRIDE;
  }
  c->r[31] = c->mem_r32(sp + 28);              // gen 80026368 epilogue
  c->r[18] = c->mem_r32(sp + 24);
  c->r[17] = c->mem_r32(sp + 20);
  c->r[16] = c->mem_r32(sp + 16);
  c->r[29] += 32;
}
