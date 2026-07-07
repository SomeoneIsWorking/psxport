// game/scene/mode_state_arm.cpp — ModeStateArm method bodies. See mode_state_arm.h for the
// per-method RE contracts. Pure guest-RAM writes, no substrate callees.

#include "scene/mode_state_arm.h"
#include "core.h"

// ModeStateArm::arm — native ownership of FUN_8005082C (Ghidra decomp scratch/decomp/
// bf816_leaves.c). The engine's mode-state arm primitive; see the class doc-comment
// (mode_state_arm.h) for the field layout + semantics.
void ModeStateArm::arm(uint8_t a, uint8_t b, uint8_t c) { Core* c_ = core;
  // Back up the previous payload triple before overwriting.
  c_->mem_w8(0x800BF8A6u, c_->mem_r8(0x800EA0D7u));
  c_->mem_w8(0x800BF8A5u, c_->mem_r8(0x800EA0D6u));
  c_->mem_w8(0x800BF8A4u, c_->mem_r8(0x800EA0D5u));
  const uint8_t prev_arm = c_->mem_r8(0x800EA0D4u);

  c_->mem_w8(0x800EC144u, 1);
  c_->mem_w8(0x800EA0D4u, 1);
  c_->mem_w8(0x800EA0D5u, a);
  c_->mem_w8(0x800EA0D6u, b);
  c_->mem_w8(0x800EA0D7u, c);
  c_->mem_w8(0x800EC145u, a);
  c_->mem_w8(0x800EC146u, b);
  c_->mem_w8(0x800EC147u, c);
  c_->mem_w8(0x800BF8A7u, (uint8_t)((prev_arm << 7) | 1));
}

// ModeStateArm::armFromAreaTable — native ownership of FUN_800508A8 (Ghidra decomp same file).
void ModeStateArm::armFromAreaTable() { Core* c_ = core;
  const uint8_t  area          = c_->mem_r8 (0x800BF870u);
  const uint16_t collected_bmp = c_->mem_r16(0x800BFE56u);
  const uint32_t collected_bit = ((uint32_t)collected_bmp >> (area & 0x1F)) & 1u;
  const uint32_t row           = 0x800A5500u + (uint32_t)area * 8u + collected_bit * 4u;

  const uint8_t cls = c_->mem_r8(row + 0);
  const uint8_t a   = c_->mem_r8(row + 1);
  const uint8_t b   = c_->mem_r8(row + 2);
  const uint8_t d   = c_->mem_r8(row + 3);

  c_->mem_w8(0x800EC144u, cls);
  c_->mem_w8(0x800EA0D5u, a);
  c_->mem_w8(0x800EA0D6u, b);
  c_->mem_w8(0x800EA0D7u, d);
  c_->mem_w8(0x800EA0D4u, cls);
  c_->mem_w8(0x800EC145u, a);
  c_->mem_w8(0x800EC146u, b);
  c_->mem_w8(0x800EC147u, d);
  c_->mem_w8(0x800BF8A7u, cls == 1 ? 0x81u : 0x02u);
  c_->mem_w8(0x800BF8A4u, a);
  c_->mem_w8(0x800BF8A5u, b);
  c_->mem_w8(0x800BF8A6u, d);
}
