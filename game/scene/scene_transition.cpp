// class SceneTransition — see scene_transition.h. Implementations only.
//
// Instance methods on the per-Core SceneTransition subsystem (embedded via Engine).
// `core` is the wired back-pointer to the owning Core; all guest-memory access goes through it.
//
// `areaMaskTrigger` is RE'd 1:1 from disas 0x800782F0. The sub-scene swap methods
// (resetSwap / beginSwap / completeSwap / stepSwapWaiter) are RE'd from 0x80073260 / 2C0 / 300 / 328.
// A handful of substrate leaves stay rec_dispatched (FUN_80074590 SFX, FUN_80054198 scene-block
// reset, FUN_80072E60/EFC/F14 case-1/5 helpers) — top-down ownership will absorb them next.
//
// Verify gates:
//   * `debug scene_transitionverify` — A/Bs areaMaskTrigger vs rec_super_call(0x800782F0).
//   * `debug subswapverify`          — A/Bs stepSwapWaiter vs rec_super_call(0x80073328).
#include "scene_transition.h"
#include "game_ctx.h"
#include "game.h"      // c->game->verify — the shared A/B verify scaffold
#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

// ─────────────────────────────────────────────────────────────────────────────
// FUN_800782F0 — area-mask "seen" register + area-mode bit
// ─────────────────────────────────────────────────────────────────────────────

void SceneTransition::areaMaskTrigger(uint8_t area, uint8_t sub) {
  Core* c = core;

  auto impl = [&](uint8_t a, uint8_t s) {
    if (a < 9) {
      uint32_t tbl_entry = c->mem_r32(0x800A54A8u + (uint32_t)a * 4u);
      uint16_t h         = c->mem_r16(tbl_entry + (uint32_t)s * 8u + 6u);
      uint8_t  base      = c->mem_r8 (0x800A55B0u + a);
      uint32_t shift     = ((uint32_t)base + (((uint32_t)h & 0x0600u) >> 9)) & 0x1Fu;
      uint32_t reg       = c->mem_r32(0x800BFE50u);
      c->mem_w32(0x800BFE50u, reg | (1u << shift));
    }
    uint8_t cur = c->mem_r8(0x800BF870u);
    if (cur == 5) { c->mem_w8(0x800BF9DBu, (uint8_t)(c->mem_r8(0x800BF9DBu) | 0x02)); return; }
    if (cur == 6) { c->mem_w8(0x800BF9DBu, (uint8_t)(c->mem_r8(0x800BF9DBu) | 0x04)); return; }
    if (cur == 7) { c->mem_w8(0x800BF9DBu, (uint8_t)(c->mem_r8(0x800BF9DBu) | 0x08)); return; }
    if (cur == 8) { c->mem_w8(0x800BF9DBu, (uint8_t)(c->mem_r8(0x800BF9DBu) | 0x10));         }
  };

  int s_v = c->game->verify.on("scene_transitionverify");
  if (!s_v) { impl(area, sub); return; }

  uint8_t* ram0 = c->game->verify.ram0();
  uint8_t* ramN = c->game->verify.ramN();
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);

  impl(area, sub);

  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  c->r[4] = area; c->r[5] = sub;
  rec_super_call(c, 0x800782F0u);

  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1;
  for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1;
  for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  VerifyHarness::Check& chk = c->game->verify.check("scene_transitionverify");
  long &ng = chk.nMatch, &nb = chk.nMismatch;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[scene_transitionverify] MISMATCH area=%u sub=%u ram@%x spad@%x\n",
                           area, sub, ro, so);
  } else if (++ng % 50 == 0) fprintf(stderr, "[scene_transitionverify] %ld matches\n", ng);
}

// ─────────────────────────────────────────────────────────────────────────────
// Sub-scene swap: FUN_80073260/2C0/300/328
// ─────────────────────────────────────────────────────────────────────────────

void SceneTransition::resetSwap(uint32_t node) {
  Core* c = core;
  c->mem_w8(node + 0, 2);
  if (c->mem_r8(node + 0xBF) != 0) {
    eng(c).sfx.trigger(0x17, 0, 0x0F);           // FUN_80074590 (native)
  }
  c->mem_w8(E7FC5_FLAG, 0);
  clearSwapBlock(SCENE_BLOCK);          // FUN_80054198 (native)
  c->mem_w8(SCENE_BLOCK + 5, 0);
  c->mem_w8(SCENE_BLOCK + 6, 0);
  c->mem_w8(SCENE_BLOCK + 7, 0);
}

// FUN_80054198 — small swap-block ephemeral clear. RE'd from disas 0x80054198..0x800541F0.
// Note the (v0=2 into 329(a0)) short branch vs the (v = node[+0x147]+2, into 329/330(a0)) long
// branch — the DELAY-SLOT constant reload (`addiu v0, zero, 2` after `bne v1, v0, ...`) is what
// lets both paths share the eventual `sb v0, 329(a0)`. Ports verbatim.
void SceneTransition::clearSwapBlock(uint32_t node) {
  Core* c = core;
  if (c->mem_r8(node + 0x146) == 4 && c->mem_r8(node + 0) == 2) return;
  c->mem_w16(node + 0x44, 0);
  c->mem_w16(node + 0x182, 0);
  if (c->mem_r8(node + 2) != 0) {
    c->mem_w8(node + 0x149, 2);
    return;
  }
  c->mem_w16(node + 0x50, 0);
  c->mem_w8(node + 0x148, 0);
  uint8_t v = (uint8_t)(c->mem_r8(node + 0x147) + 2);
  c->mem_w8(node + 0x14A, v);
  c->mem_w8(node + 0x149, v);
}

void SceneTransition::beginSwap(uint32_t node) {
  Core* c = core;
  resetSwap(node);
  c->mem_w8(BF818_SWAP_PHASE, 1);
  c->mem_w8(BF817_SWAP_KEY,   c->mem_r8(node + 3));
}

void SceneTransition::completeSwap(uint32_t node) {
  // >>> THE 2→3 HANDSHAKE. This is the ONLY writer of bf818=3 in the whole game.
  //     If bf818 stays 2 forever, the door freezes — see docs/findings/scene.md.
  Core* c = core;
  resetSwap(node);
  c->mem_w8(BF818_SWAP_PHASE, 3);
}

int SceneTransition::stepSwapWaiter(uint32_t node) {
  Core* c = core;

  auto impl = [&](uint32_t n) -> int {
    const uint8_t case_id = c->mem_r8(n + 6);
    if (case_id >= 6) return 0;

    auto second_branch_gate = [&]() -> bool {
      if (c->mem_r8(n + 0x29) == 0) return false;
      if (c->mem_r8(E7EA9_ARMED) == 0) return false;
      if (c->mem_r8(E7FFB_INHIBIT) != 0) return false;
      return true;
    };
    auto pause_latch_tail = [&]() {
      if (c->mem_r8(IF800137_PAUSE) == 0) c->mem_w8(IF800137_PAUSE, 2);
    };

    switch (case_id) {
      case 0: {
        if (second_branch_gate()) {
          c->mem_w8(n + 6, (uint8_t)(case_id + 1));
          beginSwap(n);
          pause_latch_tail();
          return 0;
        }
        if (c->mem_r8(BF818_SWAP_PHASE) != 5) return 0;
        if (c->mem_r8(BF817_SWAP_KEY)  != c->mem_r8(n + 3)) return 0;
        c->mem_w8(n + 6, (uint8_t)(case_id + 1));
        beginSwap(n);
        return 0;
      }
      case 1: {
        // FUN_80072E60 (inlined; disas 0x80072E60..0x80072EF8): ramp obj[+0x50] toward ±1024 by 64
        // per tick, driven by direction byte obj[+0x46] (0 = decrement, 1 = increment, else no-op).
        // Returns 1 if the clamp was reached this tick (a1 = 1 in the recomp), 0 otherwise. Tail
        // always sets obj[+0x56] = obj[+0x5A] + obj[+0x50] (the ADD form; FUN_80072EFC is the SUB
        // form for the completion path below).
        uint32_t clampHit = 0;
        uint8_t dir = c->mem_r8(n + 0x46);
        if (dir == 0) {
          int16_t v = (int16_t)(c->mem_r16(n + 0x50) - 64);
          c->mem_w16(n + 0x50, (uint16_t)v);
          if (v < -1024) { c->mem_w16(n + 0x50, (uint16_t)-1024); clampHit = 1; }
        } else if (dir == 1) {
          int16_t v = (int16_t)(c->mem_r16(n + 0x50) + 64);
          c->mem_w16(n + 0x50, (uint16_t)v);
          if (v >= 1025) { c->mem_w16(n + 0x50, (uint16_t)1024); clampHit = 1; }
        }
        // TAIL (always): obj[+0x56] = obj[+0x5A] + obj[+0x50] (ADD, not the sub of FUN_80072EFC)
        c->mem_w16(n + 0x56, (uint16_t)(c->mem_r16(n + 0x5A) + c->mem_r16(n + 0x50)));

        if (clampHit != 0 && c->mem_r8(0x800BF816u) != 0) {
          c->mem_w8(n + 6, (uint8_t)(case_id + 1));
          // FUN_80072EFC: n[+0x56] = n[+0x5A] - n[+0x50]  (SUB form, disas 0x80072EFC..0x80072F10)
          c->mem_w16(n + 0x56, (uint16_t)(c->mem_r16(n + 0x5A) - c->mem_r16(n + 0x50)));
        }
        return 0;
      }
      case 2: {
        if (c->mem_r8(BF818_SWAP_PHASE) != 2) return 0;
        if (c->mem_r8(BF80F_SUSPEND)    != 0) return 0;
        c->mem_w8 (n + 0, 1);
        c->mem_w8 (n + 6, (uint8_t)(case_id + 1));
        c->mem_w8 (n + 0x5F, (uint8_t)(1u - (uint32_t)c->mem_r8(n + 0x5F)));
        c->mem_w16(n + 0x84, (uint16_t)(c->mem_r16(n + 0x84) + 800));
        c->mem_w16(n + 0x86, (uint16_t)(c->mem_r16(n + 0x86) + 800));
        return 0;
      }
      case 3: {
        // >>> THE 2→3 HANDSHAKE point. Second-branch gate + not-suspended → completeSwap fires.
        //     Under the door_freeze.pad repro this never fires (docs/findings/scene.md).
        if (second_branch_gate()) {
          if (c->mem_r8(BF80F_SUSPEND) != 0) return 0;
          c->mem_w8(n + 6, (uint8_t)(case_id + 1));
          completeSwap(n);        // bf818 := 3   >>> release the waiter
          pause_latch_tail();
          return 0;
        }
        if (c->mem_r8(BF818_SWAP_PHASE) != 6) return 0;
        if (c->mem_r8(BF80F_SUSPEND) == 0) {
          c->mem_w8(n + 6, (uint8_t)(case_id + 1));
          completeSwap(n);
        }
        return 0;
      }
      case 4: {
        if (c->mem_r8(BF818_SWAP_PHASE) != 4) return 0;
        if (c->mem_r8(BF80F_SUSPEND)    != 0) return 0;
        c->mem_w8(n + 6, (uint8_t)(case_id + 1));
        c->mem_w8(BF818_SWAP_PHASE, 0);
        return 0;
      }
      case 5: {
        // FUN_80072F14 (inlined; disas 0x80072F14..0x80072FDC): REVERSE ramp — sibling of
        // FUN_80072E60. Ramps obj[+0x50] TOWARD zero (dir==0 → was neg, add 64; dir==1 → was pos,
        // sub 64) driven by direction byte obj[+0x46]. On zero-cross clamps to 0 and sets
        // clampHit=1; if obj[+0xBF] != 0 also fires SFX 24 (0x18) via FUN_80074590(24,0,15). Tail
        // writes obj[+0x56] = obj[+0x5A] - obj[+0x50] (the SUB form; FUN_80072EFC's semantic).
        uint32_t clampHit = 0;
        uint8_t dir = c->mem_r8(n + 0x46);
        if (dir == 0) {
          int16_t v = (int16_t)(c->mem_r16(n + 0x50) + 64);
          c->mem_w16(n + 0x50, (uint16_t)v);
          if (v > 0) { c->mem_w16(n + 0x50, 0); clampHit = 1; }
        } else if (dir == 1) {
          int16_t v = (int16_t)(c->mem_r16(n + 0x50) - 64);
          c->mem_w16(n + 0x50, (uint16_t)v);
          if (v < 0) { c->mem_w16(n + 0x50, 0); clampHit = 1; }
        }
        // ZERO-CROSS SFX: only when clampHit && obj[+0xBF] != 0
        if (clampHit != 0 && c->mem_r8(n + 0xBF) != 0) {
          eng(c).sfx.trigger(24, 0, 15);                 // FUN_80074590 (native)
        }
        // TAIL: obj[+0x56] = obj[+0x5A] - obj[+0x50]  (SUB form; matches FUN_80072EFC)
        c->mem_w16(n + 0x56, (uint16_t)(c->mem_r16(n + 0x5A) - c->mem_r16(n + 0x50)));

        if (clampHit != 0) {
          c->mem_w8 (n + 0, 1);
          c->mem_w8 (n + 6, 0);
          c->mem_w8 (n + 0x5F, (uint8_t)(1u - (uint32_t)c->mem_r8(n + 0x5F)));
          c->mem_w16(n + 0x84, (uint16_t)(c->mem_r16(n + 0x84) - 800));
          c->mem_w16(n + 0x86, (uint16_t)(c->mem_r16(n + 0x86) - 800));
          return 1;
        }
        return 0;
      }
    }
    return 0;
  };

  int s_v = c->game->verify.on("subswapverify");
  if (!s_v) return impl(node);

  uint8_t* ram0 = c->game->verify.ram0();
  uint8_t* ramN = c->game->verify.ramN();
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);

  int rv_n = impl(node);

  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  c->r[4] = node;
  rec_super_call(c, 0x80073328u);
  int rv_s = (int)c->r[2];

  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1;
  for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1;
  for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  VerifyHarness::Check& chk = c->game->verify.check("subswapverify");
  long &ng = chk.nMatch, &nb = chk.nMismatch;
  if (ro >= 0 || so >= 0 || rv_n != rv_s) {
    if (nb++ < 40) fprintf(stderr, "[subswapverify] MISMATCH node=%08x case=%u ram@%x spad@%x rv_n=%d rv_s=%d\n",
                           node, c->mem_r8(node + 6), ro, so, rv_n, rv_s);
  } else if (++ng % 50 == 0) fprintf(stderr, "[subswapverify] %ld matches\n", ng);
  return rv_n;
}
