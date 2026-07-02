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

// ov_xform51128 lives in engine_submit.cpp (its verify wrapper and body would need moving with it);
// forward-declare it here so we can call it directly without a header.
void ov_xform51128(Core* c);

void NodeXform::build(uint32_t node) {
  Core* c = core;
  const uint32_t SCR_M = 0x1F800000u;   // 8-word source matrix
  const uint32_t SCR_R = 0x1F800020u;   // rot output
  c->mem_w32(SCR_M +  4, 0);
  c->mem_w32(SCR_M + 12, 0);
  c->mem_w32(SCR_M + 20, 0);
  c->mem_w32(SCR_M + 24, 0);
  c->mem_w32(SCR_M + 28, 0);
  c->mem_w32(SCR_M +  0, (uint32_t)(int32_t)(int16_t)c->mem_r16(node + 184));
  c->mem_w32(SCR_M +  8, (uint32_t)(int32_t)(int16_t)c->mem_r16(node + 186));
  c->mem_w32(SCR_M + 16, (uint32_t)(int32_t)(int16_t)c->mem_r16(node + 188));
  Math::rotmat(c,node + 84, SCR_R);                            // libgte RotMatrix at 0x80085480
  Math::matMul(c,SCR_R, SCR_M, node + 152);                    // node.mat152 = rot × mat 0x80084110
  c->mem_w32(node + 172, (uint32_t)(int32_t)(int16_t)c->mem_r16(node + 46));
  c->mem_w32(node + 176, (uint32_t)(int32_t)(int16_t)c->mem_r16(node + 50));
  c->mem_w32(node + 180, (uint32_t)(int32_t)(int16_t)c->mem_r16(node + 54));
  c->r[4] = node; ov_xform51128(c);
}
