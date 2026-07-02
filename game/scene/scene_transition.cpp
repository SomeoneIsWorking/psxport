// class SceneTransition — see scene_transition.h. Implementations only.
//
// `areaMaskTrigger` is RE'd 1:1 from disas 0x800782F0 (docs L55158-L55186 in
// scratch/decomp/ram_f1000_all.c). Guest-memory-direct: the two resident tables
// (PTR_DAT_800A54A8 and DAT_800A55B0) live in MAIN.EXE .rodata and are read via mem_r32/mem_r8.
//
// The sub-scene swap methods (resetSwap / beginSwap / completeSwap / stepSwapWaiter) are RE'd from
// disas 0x80073260 / 0x800732C0 / 0x80073300 / 0x80073328 (decomp L52186-L52316). resetSwap and
// stepSwapWaiter still dispatch a handful of substrate leaves:
//   FUN_80074590  (sound/rumble trigger)  — inside resetSwap
//   FUN_80054198  (scene-block reset)     — inside resetSwap
//   FUN_80072E60  (case-1 gate helper)    — stepSwapWaiter case 1
//   FUN_80072EFC  (case-1 advance action) — stepSwapWaiter case 1
//   FUN_80072F14  (case-5 consume gate)   — stepSwapWaiter case 5
// All five are pure resident leaves and stay substrate until their own top-down ownership pass.
//
// Verify gates:
//   * `debug scene_transitionverify` — A/Bs areaMaskTrigger vs rec_super_call(0x800782F0).
//   * `debug subswapverify`          — A/Bs stepSwapWaiter vs rec_super_call(0x80073328) — the
//                                      state-machine gate (covers resetSwap/beginSwap/completeSwap
//                                      transitively, since case 0/1/3 call them).
#include "scene_transition.h"
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

namespace {

inline void areaMaskTrigger_impl(Core* c, uint8_t area, uint8_t sub) {
  if (area < 9) {
    uint32_t tbl_entry = c->mem_r32(0x800A54A8u + (uint32_t)area * 4u);
    uint16_t h         = c->mem_r16(tbl_entry + (uint32_t)sub * 8u + 6u);
    uint8_t  base      = c->mem_r8 (0x800A55B0u + area);
    uint32_t shift     = ((uint32_t)base + (((uint32_t)h & 0x0600u) >> 9)) & 0x1Fu;
    uint32_t reg       = c->mem_r32(0x800BFE50u);
    c->mem_w32(0x800BFE50u, reg | (1u << shift));
  }
  uint8_t cur = c->mem_r8(0x800BF870u);
  if (cur == 5) { c->mem_w8(0x800BF9DBu, (uint8_t)(c->mem_r8(0x800BF9DBu) | 0x02)); return; }
  if (cur == 6) { c->mem_w8(0x800BF9DBu, (uint8_t)(c->mem_r8(0x800BF9DBu) | 0x04)); return; }
  if (cur == 7) { c->mem_w8(0x800BF9DBu, (uint8_t)(c->mem_r8(0x800BF9DBu) | 0x08)); return; }
  if (cur == 8) { c->mem_w8(0x800BF9DBu, (uint8_t)(c->mem_r8(0x800BF9DBu) | 0x10));         }
}

}  // namespace

void SceneTransition::areaMaskTrigger(Core* c, uint8_t area, uint8_t sub) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("scene_transitionverify") ? 1 : 0;
  if (!s_v) { areaMaskTrigger_impl(c, area, sub); return; }

  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);

  areaMaskTrigger_impl(c, area, sub);

  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  c->r[4] = area; c->r[5] = sub;
  rec_super_call(c, 0x800782F0u);

  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1;
  for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1;
  for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[scene_transitionverify] MISMATCH area=%u sub=%u ram@%x spad@%x\n",
                           area, sub, ro, so);
  } else if (++ng % 50 == 0) fprintf(stderr, "[scene_transitionverify] %ld matches\n", ng);
}

// ─────────────────────────────────────────────────────────────────────────────
// Sub-scene swap: FUN_80073260/2C0/300/328
// ─────────────────────────────────────────────────────────────────────────────

void SceneTransition::resetSwap(Core* c, uint32_t node) {
  // node[0] = 2
  c->mem_w8(node + 0, 2);
  // if (node[0xBF] != 0) FUN_80074590(0x17, 0, 0xf)   — sound/rumble kick
  if (c->mem_r8(node + 0xBF) != 0) {
    c->r[4] = 0x17; c->r[5] = 0; c->r[6] = 0x0F;
    rec_dispatch(c, 0x80074590u);
  }
  // Scene-block reset: clear +325 flag, dispatch FUN_80054198(0x800E7E80), clear +5/+6/+7
  c->mem_w8(E7FC5_FLAG, 0);
  c->r[4] = SCENE_BLOCK; rec_dispatch(c, 0x80054198u);
  c->mem_w8(SCENE_BLOCK + 5, 0);
  c->mem_w8(SCENE_BLOCK + 6, 0);
  c->mem_w8(SCENE_BLOCK + 7, 0);
}

void SceneTransition::beginSwap(Core* c, uint32_t node) {
  resetSwap(c, node);
  c->mem_w8(BF818_SWAP_PHASE, 1);
  c->mem_w8(BF817_SWAP_KEY,   c->mem_r8(node + 3));
}

void SceneTransition::completeSwap(Core* c, uint32_t node) {
  // >>> THE 2→3 HANDSHAKE. This is the ONLY writer of bf818=3 in the whole game.
  //     If bf818 stays at 2 forever, the door freezes — see docs/findings/scene.md.
  resetSwap(c, node);
  c->mem_w8(BF818_SWAP_PHASE, 3);
}

namespace {

// The FUN_80073328 body — walked from stepSwapWaiter. Every code path except case 5 (which sets
// v0=1 on consume) leaves v0=0. Guest a0 = node = param_1.
inline int stepSwapWaiter_impl(Core* c, uint32_t node) {
  const uint8_t case_id = c->mem_r8(node + 6);
  if (case_id >= 6) return 0;   // sltiu v0, v1, 6 → tail (v0=0)

  // Case 0 and case 3 share the same "SECOND-branch gate":
  //   node[0x29]!=0 && DAT_800e7ea9!=0 && DAT_800e7ffb==0
  auto second_branch_gate = [&]() -> bool {
    if (c->mem_r8(node + 0x29) == 0) return false;
    if (c->mem_r8(SceneTransition::E7EA9_ARMED) == 0) return false;
    if (c->mem_r8(SceneTransition::E7FFB_INHIBIT) != 0) return false;
    return true;
  };
  auto pause_latch_tail = [&]() {
    // LAB_800734fc: if (*0x1F800137 == 0) *0x1F800137 = 2
    if (c->mem_r8(SceneTransition::IF800137_PAUSE) == 0)
      c->mem_w8(SceneTransition::IF800137_PAUSE, 2);
  };

  switch (case_id) {
    case 0: {
      if (second_branch_gate()) {
        c->mem_w8(node + 6, (uint8_t)(case_id + 1));   // advance
        SceneTransition::beginSwap(c, node);
        pause_latch_tail();
        return 0;
      }
      // FIRST branch: wait for bf818==5 && bf817==node[3]
      if (c->mem_r8(SceneTransition::BF818_SWAP_PHASE) != 5) return 0;
      if (c->mem_r8(SceneTransition::BF817_SWAP_KEY)  != c->mem_r8(node + 3)) return 0;
      c->mem_w8(node + 6, (uint8_t)(case_id + 1));   // advance
      SceneTransition::beginSwap(c, node);
      return 0;
    }
    case 1: {
      c->r[4] = node; rec_dispatch(c, 0x80072E60u);    // gate helper (still substrate)
      if (c->r[2] != 0 && c->mem_r8(0x800BF816u) != 0) {
        c->mem_w8(node + 6, (uint8_t)(case_id + 1));   // advance
        rec_dispatch(c, 0x80072EFCu);                  // action leaf (still substrate)
      }
      return 0;
    }
    case 2: {
      // bf818 == 2 && bf80f (suspend) == 0 → do the swap-body midpoint
      if (c->mem_r8(SceneTransition::BF818_SWAP_PHASE) != 2) return 0;
      if (c->mem_r8(SceneTransition::BF80F_SUSPEND)    != 0) return 0;
      c->mem_w8 (node + 0, 1);                          // node[0] = 1
      c->mem_w8 (node + 6, (uint8_t)(case_id + 1));     // advance
      // node[0x5F] = 1 - node[0x5F]
      c->mem_w8 (node + 0x5F, (uint8_t)(1u - (uint32_t)c->mem_r8(node + 0x5F)));
      // node+0x84/+0x86 += 800  (s16)
      c->mem_w16(node + 0x84, (uint16_t)(c->mem_r16(node + 0x84) + 800));
      c->mem_w16(node + 0x86, (uint16_t)(c->mem_r16(node + 0x86) + 800));
      return 0;
    }
    case 3: {
      // >>> THE 2→3 HANDSHAKE point. Second-branch gate must be met AND swap must not be
      //     suspended. Under the current door_freeze.pad repro this never fires (node[0x29]==0
      //     on every candidate entity) — that is the OPEN bug tracked in docs/findings/scene.md.
      if (second_branch_gate()) {
        if (c->mem_r8(SceneTransition::BF80F_SUSPEND) != 0) return 0;
        c->mem_w8(node + 6, (uint8_t)(case_id + 1));   // advance
        SceneTransition::completeSwap(c, node);        // bf818 := 3   >>> release the waiter
        pause_latch_tail();
        return 0;
      }
      // FIRST branch: wait for bf818==6 (permanent dead-end — no code writes bf818==6)
      if (c->mem_r8(SceneTransition::BF818_SWAP_PHASE) != 6) return 0;
      if (c->mem_r8(SceneTransition::BF80F_SUSPEND) == 0) {
        c->mem_w8(node + 6, (uint8_t)(case_id + 1));
        SceneTransition::completeSwap(c, node);
      }
      return 0;
    }
    case 4: {
      if (c->mem_r8(SceneTransition::BF818_SWAP_PHASE) != 4) return 0;
      if (c->mem_r8(SceneTransition::BF80F_SUSPEND)    != 0) return 0;
      c->mem_w8(node + 6, (uint8_t)(case_id + 1));
      c->mem_w8(SceneTransition::BF818_SWAP_PHASE, 0);   // clear phase — transition complete
      return 0;
    }
    case 5: {
      c->r[4] = node; rec_dispatch(c, 0x80072F14u);
      if (c->r[2] != 0) {
        c->mem_w8 (node + 0, 1);
        c->mem_w8 (node + 6, 0);
        c->mem_w8 (node + 0x5F, (uint8_t)(1u - (uint32_t)c->mem_r8(node + 0x5F)));
        c->mem_w16(node + 0x84, (uint16_t)(c->mem_r16(node + 0x84) - 800));
        c->mem_w16(node + 0x86, (uint16_t)(c->mem_r16(node + 0x86) - 800));
        return 1;
      }
      return 0;
    }
  }
  return 0;
}

}  // namespace

int SceneTransition::stepSwapWaiter(Core* c, uint32_t node) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("subswapverify") ? 1 : 0;
  if (!s_v) { return stepSwapWaiter_impl(c, node); }

  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);

  int rv_n = stepSwapWaiter_impl(c, node);

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
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0 || rv_n != rv_s) {
    if (nb++ < 40) fprintf(stderr, "[subswapverify] MISMATCH node=%08x case=%u ram@%x spad@%x rv_n=%d rv_s=%d\n",
                           node, c->mem_r8(node + 6), ro, so, rv_n, rv_s);
  } else if (++ng % 50 == 0) fprintf(stderr, "[subswapverify] %ld matches\n", ng);
  return rv_n;
}
