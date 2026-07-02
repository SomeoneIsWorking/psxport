// PC-native PLAYER velocity-integrate handler. Per the CLAUDE.md boundary update (2026-06-21) game
// CONTENT — including how Tomba moves — is now a valid native-ownership target. FUN_80056B48 integrates a
// per-frame velocity (speed × dir) into the player/scene MASTER position (the 16.16-fixed X/Y/Z at
// 0x800E7EAC/B0/B4, struct base G = 0x800E7E80). This is a CONTENT-INTERFACE fn: the camera follow + the
// int16 position mirrors + render all read these words, so the native body must write RAM-IDENTICAL bytes
// vs the recomp reference. Verified 0-diff vs rec_super_call via the `playerverify` gate (full RAM+
// scratchpad A/B), driven with movement (press right/left). RE below; disasm tools/disas.py 0x80056B48.
//
// FUN_80056B48(a0 = G = 0x800E7E80, a1 = suppress-Y flag) — disasm 0x80056B48..0x80056BFC:
//   READS (all relative to a0):
//     a0+0x44 (lh, s16)  speed      [0x800E7EC4]
//     a0+0x48 (lh, s16)  dirX       [0x800E7EC8]
//     a0+0x4A (lh, s16)  dirY       [0x800E7ECA]
//     a0+0x4C (lh, s16)  dirZ       [0x800E7ECC]
//     a0+0x2C (lw, s32)  posX 16.16 [0x800E7EAC]
//     a0+0x34 (lw, s32)  posZ 16.16 [0x800E7EB4]
//     a0+0x30 (lw, s32)  posY 16.16 [0x800E7EB0]  (only when a1==0)
//     a0+0x16B (lbu)     flag363    [0x800E7FEB]
//     a0+0x61  (lbu)     flag97     [0x800E7EE1]
//     a0+0x5F  (lbu)     flag95     [0x800E7EDF]
//   WRITES:
//     a0+0x2C  posX += dirX*speed   (signed 32-bit add, low 32 of the s16×s16 product = exact)
//     a0+0x34  posZ += dirZ*speed
//     a0+0x30  posY += dirY*speed   (only when a1==0)
//     a0+0x5F  flag95 &= ~0x04      (only on the else branch below)
//   CONTROL FLOW (tail):
//     if (flag363 == 0 && flag97 == 0)  jal 0x80054650(a0, a1=0)   // a stop/settle helper (kept dispatched)
//     else                              flag95 &= ~0x04
//   v0 (return) is whatever falls out: the jal-branch returns 0x80054650's v0; the else-branch's last value
//   is the masked flag95 byte. The recomp epilogue does no explicit v0 set, so we mirror by leaving r[2] as
//   the dispatched call's result (jal path) or the masked byte (else path) — and the A/B gate covers v0.
#include "core.h"
#include "cfg.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rec_super_call(Core*, uint32_t);   // interpret the original PSX body (A/B oracle / super-call)
void rec_dispatch(Core*, uint32_t);     // run a guest fn in-context (honors its own override)

// The native body. a0 = c->r[4] (G), a1 = c->r[5] (suppress-Y).
static void player_move_56b48(Core* c) {
  uint32_t a0 = c->r[4];
  uint32_t a1 = c->r[5];

  int32_t speed = c->mem_r16s(a0 + 0x44);   // lh a0+72
  int32_t dirX  = c->mem_r16s(a0 + 0x48);   // lh a0+72(+0x48)
  // posX += dirX*speed   (mult dirX,speed -> mflo a2; addu posX,a2)
  c->mem_w32(a0 + 0x2C, c->mem_r32(a0 + 0x2C) + (uint32_t)(dirX * speed));
  // posZ += dirZ*speed   (dirZ @ +0x4C)
  int32_t dirZ = c->mem_r16s(a0 + 0x4C);    // lh a0+76 (+0x4C)
  c->mem_w32(a0 + 0x34, c->mem_r32(a0 + 0x34) + (uint32_t)(dirZ * speed));

  if (a1 == 0) {                                     // bne a1,zero skips the Y integrate
    int32_t dirY = c->mem_r16s(a0 + 0x4A);   // lh a0+74 (+0x4A)
    c->mem_w32(a0 + 0x30, c->mem_r32(a0 + 0x30) + (uint32_t)(dirY * speed));
  }

  // tail: if (flag363==0 && flag97==0) jal 0x80054650(a0,0); else flag95 &= ~0x04
  if (c->mem_r8(a0 + 0x16B) == 0 && c->mem_r8(a0 + 0x61) == 0) {
    c->r[4] = a0; c->r[5] = 0;
    rec_dispatch(c, 0x80054650u);                    // settle/stop helper — kept as content (dispatched)
  } else {
    uint8_t f = (uint8_t)(c->mem_r8(a0 + 0x5F) & 0xFB);
    c->mem_w8(a0 + 0x5F, f);
    c->r[2] = f;                                      // else-branch falls through with v0 = masked byte
  }
}
