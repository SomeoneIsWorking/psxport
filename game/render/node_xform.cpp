// NodeXform::build — PC-native reimpl of guest FUN_80051844.
//
// RE'd 1:1 from gen_func_80051844 (generated/shard_7.c) / disas 0x80051844:
//   scratchpad @0x1F800000 = { sx16(node+184), 0, sx16(node+186), 0, sx16(node+188), 0, 0, 0 }
//     (8 int32 words — 3 trans lanes at +0/+8/+16, zeros elsewhere)
//   ov_rotmat(a0=node+84 euler, a1=0x1F800020)              // libgte RotMatrix at 0x80085480
//   ov_mat_mul(a0=0x1F800020 rot, a1=0x1F800000 mat, a2=node+152)
//                                                           // node.mat152 = rot × mat   (0x80084110)
//   node+172 = sx16(node+46)                                // world-space position copy
//   node+176 = sx16(node+50)
//   node+180 = sx16(node+54)
//   ov_xform51128(node)                                     // propagate to children     (0x80051128)
//
// All 3 callees are already native (ov_rotmat, ov_mat_mul, ov_xform51128), so this is pure
// scratchpad seeding + native-call orchestration. No rec_dispatch needed.
#include "node_xform.h"
#include "core.h"
#include "engine_math.h"     // Math::rotmat, Math::matMul (static)

void NodeXform::build(uint32_t node) {
  Core* c = core;
  const uint32_t SCR_M = 0x1F800000u;   // 8-word source matrix
  const uint32_t SCR_R = 0x1F800020u;   // rot output
  c->mem_w32(SCR_M +  4, 0);
  c->mem_w32(SCR_M + 12, 0);
  c->mem_w32(SCR_M + 20, 0);
  c->mem_w32(SCR_M + 24, 0);
  c->mem_w32(SCR_M + 28, 0);
  c->mem_w32(SCR_M +  0, (uint32_t)c->mem_r16s(node + 184));
  c->mem_w32(SCR_M +  8, (uint32_t)c->mem_r16s(node + 186));
  c->mem_w32(SCR_M + 16, (uint32_t)c->mem_r16s(node + 188));
  c->math.rotmat(node + 84, SCR_R);                            // libgte RotMatrix at 0x80085480
  c->math.matMul(SCR_R, SCR_M, node + 152);                    // node.mat152 = rot × mat 0x80084110
  c->mem_w32(node + 172, (uint32_t)c->mem_r16s(node + 46));
  c->mem_w32(node + 176, (uint32_t)c->mem_r16s(node + 50));
  c->mem_w32(node + 180, (uint32_t)c->mem_r16s(node + 54));
  propagate(node);
}

// FUN_80051128 — per-object CHILD-NODE TRANSFORM loop. RE'd from disas:
//   guard: if node[9]==0 -> return
//   loop s2 in [0, node[8]) with continue-bound node[9] (dual-bound idiom):
//     child = node[0xC0 + 4*s2]
//     seed scratchpad @0x1F800000: diagonal { child[56], 0, child[58], 0, child[60], 0, 0, 0 }
//     rotmat(child+8 euler → 0x1F800020) ; matMul(rot, work → 0x1F800040)
//     sentinel = child+6 (s16)
//     ROOT (sentinel==-1): matMul(node+152, 0x1F800040 → child+24); applyMatlv(child → child+44);
//       child[0x2C/0x30/0x34] += node[0xAC/0xB0/0xB4]
//     SIBLING: p = node[0xC0 + 4*sentinel]; matMul(p+24, 0x1F800040 → child+24); applyMatlv(...)
//       child[0x2C/0x30/0x34] += p[0x2C/0x30/0x34]
// (GOTCHAS: +56/58/60 are sign-extended lhu+sll16+sra16; sentinel is sll'd by 2 before the branch
// so in the sibling path it's already a byte offset. NO render packets, NO GTE ops.)
void NodeXform::propagate(uint32_t node) {
  Core* c = core;
  if (c->mem_r8(node + 9) == 0) return;
  int i = 0;
  while (i < (int)(uint8_t)c->mem_r8(node + 8)) {
    uint32_t child = c->mem_r32(node + 0xC0 + 4u * (uint32_t)i);
    c->mem_w32(0x1F800004u, 0); c->mem_w32(0x1F80000Cu, 0); c->mem_w32(0x1F800014u, 0);
    c->mem_w32(0x1F800018u, 0); c->mem_w32(0x1F80001Cu, 0);
    c->mem_w32(0x1F800000u, (uint32_t)c->mem_r16s(child + 56));
    c->mem_w32(0x1F800008u, (uint32_t)c->mem_r16s(child + 58));
    c->mem_w32(0x1F800010u, (uint32_t)c->mem_r16s(child + 60));
    int16_t sentinel = c->mem_r16s(child + 6);
    c->math.rotmat(child + 8, 0x1F800020u);
    c->math.matMul(0x1F800020u, 0x1F800000u, 0x1F800040u);
    if (sentinel == -1) {
      c->math.matMul(node + 152, 0x1F800040u, child + 24);
      c->math.applyMatlv(child, child + 44);
      c->mem_w32(child + 0x2C, c->mem_r32(child + 0x2C) + c->mem_r32(node + 0xAC));
      c->mem_w32(child + 0x30, c->mem_r32(child + 0x30) + c->mem_r32(node + 0xB0));
      c->mem_w32(child + 0x34, c->mem_r32(child + 0x34) + c->mem_r32(node + 0xB4));
    } else {
      uint32_t p = c->mem_r32(node + 0xC0 + 4u * (uint32_t)(int)sentinel);
      c->math.matMul(p + 24, 0x1F800040u, child + 24);
      c->math.applyMatlv(child, child + 44);
      c->mem_w32(child + 0x2C, c->mem_r32(child + 0x2C) + c->mem_r32(p + 0x2C));
      c->mem_w32(child + 0x30, c->mem_r32(child + 0x30) + c->mem_r32(p + 0x30));
      c->mem_w32(child + 0x34, c->mem_r32(child + 0x34) + c->mem_r32(p + 0x34));
    }
    i++;
    if (!(i < (int)(uint8_t)c->mem_r8(node + 9))) break;
  }
}
