// game/ai/beh_a06_scripted_actor.cpp — PC-native port of A06's cutscene SCRIPTED-ACTOR behavior.
//
// Guest handler at 0x8013AA14 (A06 overlay). Registered in BehaviorDispatch::kTable so the object-
// list walker's native path routes here. This behavior is the DIRECT ancestor of the resident
// cutscene-script interpreter (0x80041098) — three A06 fns chain into it:
//
//   0x8013AA14 (entry, this fn — beh_a06_scripted_actor)
//     └─ jal 0x80139c84 (case-4 sub-machine)
//         └─ jal 0x80139a28 (case-3 sub-sub-machine)
//             └─ jal 0x80041098 (ScriptInterp::step)
//
// The two lower fns are inlined here as static helpers (variant4Sm / variant4Phase3) so the WHOLE
// chain runs native — every `jal 0x80041098` / `jal 0x80040cdc` in the chain becomes a direct
// `eng(c).script.step / init` call. This is the caller-chain hookup that finally makes
// ScriptInterp::step actually FIRE at runtime under BehaviorDispatch (previously it was landed
// but dark). It also enables op-0x03E native fnptr routing for every script-driven fade fn
// registered as its own beh_* (script tables at 0x80149A20 / F18 / F38 / F68 / F98 / 958 / 6E8 /
// 788 / 7B8 / 838, all reached from here).
//
// RE'd from Ghidra decomp of the three chain fns (scratch/decomp/a06_scripted_actor.c) with hand-
// disas spot-checks of the raw MIPS at 0x8013AA14 / 0x80139C84 / 0x80139A28. Substrate leaves the
// port keeps reachable via rec_dispatch (each one a small standalone leaf, safe to promote one at
// a time):
//
//   0x800519E0 — asset-load / init leaf (returns 0 on OK)
//   0x80041718 — obj init2 (called with 0,0)
//   0x8007778C — per-frame prelude (position/anim tick)
//   0x8004190C — per-frame leaf 2
//   0x80042354 — arm-with (kind, subkind) helper — takes (1,4), (1,2), (1,1) from here
//   0x80042310 — arm-clear helper (no args)
//   0x80051B04 — music/SFX cue (parent, 0x13, 0xf) — variant-4 init
//   0x80077C40 — variant-4/case-0 registrar (obj, tableA, id)
//   0x800518FC — per-frame cleanup gated on obj[+0x01]
//   0x8007A624 — despawn (outer state 3)
//   0x801381A8 — variant-0 per-frame handler
//   0x80139088 — variant-1 sub-state 1 handler
//   0x801392D8 — variant-1 sub-state 2 handler
//   0x8013A81C — variant-5 per-frame handler
//   0x801398E4 — sub-check inside variant4Sm case 2 (returns 2/3 to drive state advance)

#include "core.h"
#include "game_ctx.h"
#include "core/engine.h"
#include "object/behavior_dispatch.h"
#include "scene/script_interp.h"
#include <cstdint>

extern "C" void rec_dispatch(Core* c, uint32_t addr);

namespace {

// Data pointers this behavior hands the script interpreter. All live in A06 overlay data.
// tableA is the "secondary" table the interpreter records into obj[+0x7C]; each script pointer
// is a distinct cutscene bytecode (see docs/findings/scene.md "Cutscene SCRIPT INTERPRETER").
constexpr uint32_t TABLE_A          = 0x8014D014u;
constexpr uint32_t SCRIPT_80149A20  = 0x80149A20u;
constexpr uint32_t SCRIPT_80149F18  = 0x80149F18u;
constexpr uint32_t SCRIPT_80149F38  = 0x80149F38u;
constexpr uint32_t SCRIPT_80149F68  = 0x80149F68u;
constexpr uint32_t SCRIPT_80149F98  = 0x80149F98u;
constexpr uint32_t SCRIPT_80149958  = 0x80149958u;
constexpr uint32_t SCRIPT_801496E8  = 0x801496E8u;
constexpr uint32_t SCRIPT_80149788  = 0x80149788u;
constexpr uint32_t SCRIPT_801497B8  = 0x801497B8u;
constexpr uint32_t SCRIPT_80149838  = 0x80149838u;
constexpr uint32_t DATA_80141E90    = 0x80141E90u;   // asset-list ptr for FUN_800519E0

// External guest globals this behavior consults. Kept named for readability.
constexpr uint32_t G_ECFA4 = 0x800ECFA4u;   // read as arg to FUN_800519E0 (asset id)
constexpr uint32_t G_ECF90 = 0x800ECF90u;   // read into obj[+0x3C] at init
constexpr uint32_t G_BF80F = 0x800BF80Fu;   // gate byte, variant4Sm case 1
constexpr uint32_t G_BF868 = 0x800BF868u;   // pointer slot set to obj in variant-4 init
constexpr uint32_t G_BF870 = 0x800BF870u;   // area mode / shift amount for BFE56 test
constexpr uint32_t G_BF8D2 = 0x800BF8D2u;   // gate byte, variant 1 sub 0
constexpr uint32_t G_BF8D5 = 0x800BF8D5u;   // gate byte, variant4Sm case 0
constexpr uint32_t G_BF90C = 0x800BF90Cu;   // variant-2 script select
constexpr uint32_t G_BF90D = 0x800BF90Du;   // variant-3 script select
constexpr uint32_t G_BFA1D = 0x800BFA1Du;   // bitmask, variant 1 sub 0
constexpr uint32_t G_BFA22 = 0x800BFA22u;   // counter, variant4Sm case 0
constexpr uint32_t G_BFAE5 = 0x800BFAE5u;   // gate byte, variant4Phase3 state 0
constexpr uint32_t G_BFE56 = 0x800BFE56u;   // 32-bit bitmask, variant4Sm case 4

// Object field offsets — same layout the script interpreter uses (see script_interp.h).
constexpr uint32_t O_KIND     = 0x00u;   // state-0 sets to 9
constexpr uint32_t O_CLEANUP  = 0x01u;   // gate: if nonzero, call FUN_800518FC each frame
constexpr uint32_t O_VARIANT  = 0x03u;   // 0..5 selects the case-1 arm
constexpr uint32_t O_STATE_04 = 0x04u;   // outer state (0..3)
constexpr uint32_t O_SUB_05   = 0x05u;   // case-4 / variant-1 sub-state
constexpr uint32_t O_SUB_06   = 0x06u;   // inner state
constexpr uint32_t O_SUB_07   = 0x07u;   // inner-inner state
constexpr uint32_t O_READY_2B = 0x2Bu;   // must == 3 for scripts to arm
constexpr uint32_t O_D_3C     = 0x3Cu;   // init value from G_ECF90
constexpr uint32_t O_H_54     = 0x54u;   // zeroed at init
constexpr uint32_t O_H_56     = 0x56u;   // variant-specific value
constexpr uint32_t O_H_58     = 0x58u;   // zeroed at init
constexpr uint32_t O_B_5F     = 0x5Fu;   // zeroed at init
constexpr uint32_t O_PROG_70  = 0x70u;   // ScriptInterp progress byte (-1 == 0xFF = done)
constexpr uint32_t O_TABLE_A  = 0x7Cu;   // ScriptInterp secondary table
constexpr uint32_t O_H_80     = 0x80u;
constexpr uint32_t O_H_82     = 0x82u;
constexpr uint32_t O_H_84     = 0x84u;
constexpr uint32_t O_H_86     = 0x86u;
constexpr uint32_t O_B_BF     = 0xBFu;   // zeroed at init
constexpr uint32_t O_D_C4     = 0xC4u;   // parent-obj ptr (used for variant-4 SFX cue)

// Small helper for "call substrate leaf with (a0=obj)" pattern that recurs throughout.
inline void callObj1(Core* c, uint32_t obj, uint32_t addr) {
  c->r[4] = obj;
  rec_dispatch(c, addr);
}
// "(a0, a1)" leaf call.
inline void call2(Core* c, uint32_t a0, uint32_t a1, uint32_t addr) {
  c->r[4] = a0; c->r[5] = a1;
  rec_dispatch(c, addr);
}
// "(a0, a1, a2)" leaf call.
inline void call3(Core* c, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t addr) {
  c->r[4] = a0; c->r[5] = a1; c->r[6] = a2;
  rec_dispatch(c, addr);
}

// FUN_800519E0(obj, 0xf, *(u32)0x800ECFA4, &DAT_80141E90) — asset-load init. Returns 0 on OK.
uint32_t assetInit(Core* c, uint32_t obj) {
  c->r[4] = obj;
  c->r[5] = 0xFu;
  c->r[6] = c->mem_r32(G_ECFA4);
  c->r[7] = DATA_80141E90;
  rec_dispatch(c, 0x800519E0u);
  return c->r[2];
}

// ── FUN_80139A28 — variant-4 case-3 sub-machine (inner script cycle) ─────────────────────────────
// Outer state on obj[+0x06]; each state runs its own 3-substate inner cycle:
//   substate 0: init script; substate 1: step it; on progress==0xFF wrap up
// When any state's cycle completes it calls FUN_80042310 + sets obj[+0x04]=3 (advance outer to
// despawn). Direct arg-2 to init is a distinct script pointer per state (F18 / F38 / A20 / A20).
void variant4Phase3(Core* c, uint32_t obj) {
  const uint8_t s6 = c->mem_r8(obj + O_SUB_06);
  bool wrapUp = false;

  auto initAndArm = [&](uint32_t scriptPtr) {
    c->mem_w8(obj + O_SUB_07, 1u);
    eng(c).script.init(obj, TABLE_A, scriptPtr);
    c->mem_w8(obj + O_PROG_70, 1u);
  };

  auto stepAndCheckDone = [&](bool advanceOuterToDespawn) {
    eng(c).script.step(obj);
    if ((int8_t)c->mem_r8(obj + O_PROG_70) == -1) {
      c->mem_w8(obj + O_SUB_07, 0u);
      wrapUp = true;
      if (advanceOuterToDespawn) {
        // Case-3-of-outer path: after wrap, set obj[+0x04]=3 (this is the joined tail below).
      }
    }
  };

  if (s6 == 1) {
    // Original state-1 sub-machine — its wrap path resets obj[+0x06]=0 (loop back), NOT +0x04.
    const uint8_t s7 = c->mem_r8(obj + O_SUB_07);
    if (s7 == 0) {
      initAndArm(SCRIPT_80149F18);
    } else if (s7 == 1) {
      stepAndCheckDone(/*advanceOuterToDespawn=*/false);
      if (wrapUp) {
        callObj1(c, obj, 0x80042310u);
        c->mem_w8(obj + O_SUB_06, 0u);
      }
    }
    return;
  }

  if (s6 == 0) {
    // Gate: only arm when obj[+0x2B]==3 (ready) AND the external DAT_800BFAE5 gate says which
    // outer state to advance to (1 = first phase, 2 = skip to phase 2).
    if (c->mem_r8(obj + O_READY_2B) != 3u) return;
    call2(c, 1u, 4u, 0x80042354u);
    const uint8_t gate = c->mem_r8(G_BFAE5);
    c->mem_w8(obj + O_SUB_06, (uint8_t)(gate == 0 ? 1 : 2));
    return;
  }

  if (s6 == 2) {
    const uint8_t s7 = c->mem_r8(obj + O_SUB_07);
    if (s7 == 0) {
      // Phase-2 init: uses script F38.
      c->mem_w8(obj + O_SUB_07, 1u);
      eng(c).script.init(obj, TABLE_A, SCRIPT_80149F38);
      c->mem_w8(obj + O_PROG_70, 1u);
    } else if (s7 == 1) {
      stepAndCheckDone(/*advanceOuterToDespawn=*/true);
      if (wrapUp) {
        // Phase-2 done → advance to phase 3 (using script A20).
        c->mem_w8(obj + O_SUB_07, (uint8_t)(c->mem_r8(obj + O_SUB_07) + 1u));  // = 1
        // (Re-init immediately in the same tick — decomp's LAB_80139c1c fallthrough.)
        eng(c).script.init(obj, TABLE_A, SCRIPT_80149A20);
        c->mem_w8(obj + O_PROG_70, 1u);
      }
    } else if (s7 == 2) {
      eng(c).script.step(obj);
      if ((int8_t)c->mem_r8(obj + O_PROG_70) == -1) {
        c->mem_w8(obj + O_SUB_07, 0u);
        callObj1(c, obj, 0x80042310u);
        c->mem_w8(obj + O_STATE_04, 3u);
      }
    }
    return;
  }

  if (s6 == 3) {
    const uint8_t s7 = c->mem_r8(obj + O_SUB_07);
    if (s7 == 0) {
      c->mem_w8(obj + O_SUB_07, 1u);
      eng(c).script.init(obj, TABLE_A, SCRIPT_80149A20);
      c->mem_w8(obj + O_PROG_70, 1u);
    } else if (s7 == 1) {
      eng(c).script.step(obj);
      if ((int8_t)c->mem_r8(obj + O_PROG_70) == -1) {
        c->mem_w8(obj + O_SUB_07, 0u);
        callObj1(c, obj, 0x80042310u);
        c->mem_w8(obj + O_STATE_04, 3u);
      }
    }
  }
}

// ── FUN_80139C84 — variant-4 outer sub-machine (5 states) ───────────────────────────────────────
// Reached from FUN_8013AA14 case-4. Own outer state on obj[+0x05]. Cases 0/1/2/3/4 each drive a
// script-init / step cycle; case 3 recursively invokes variant4Phase3. FUN_801398E4 is a substrate
// sub-check leaf whose return value (2 or 3) drives case-2's post-check.
uint32_t sub801398E4(Core* c, uint32_t obj) {
  callObj1(c, obj, 0x801398E4u);
  return c->r[2];
}
void variant4Sm(Core* c, uint32_t obj) {
  const uint8_t s5 = c->mem_r8(obj + O_SUB_05);

  auto initScript = [&](uint32_t scriptPtr) {
    eng(c).script.init(obj, TABLE_A, scriptPtr);
    c->mem_w8(obj + O_PROG_70, 1u);
  };

  switch (s5) {
    case 0:
      if ((int8_t)c->mem_r8(G_BF8D5) == -1) {
        c->mem_w8(obj + O_SUB_05, 4u);
        call3(c, obj, TABLE_A, 0u, 0x80077C40u);
      } else if (c->mem_r8(G_BFA22) < 0x14u) {
        c->mem_w8(obj + O_SUB_05, (uint8_t)(c->mem_r8(G_BFA22) < 10u ? 1 : 2));
      } else {
        c->mem_w8(obj + O_SUB_05, 3u);
      }
      return;

    case 1: {
      const uint8_t s6 = c->mem_r8(obj + O_SUB_06);
      if (s6 == 0) {
        if (c->mem_r8(G_BF80F) != 0) return;
        c->mem_w8(obj + O_SUB_06, 1u);
        call2(c, 1u, 4u, 0x80042354u);
        initScript(SCRIPT_80149958);
        return;
      }
      if (s6 != 1) return;
      eng(c).script.step(obj);
      if ((int8_t)c->mem_r8(obj + O_PROG_70) != -1) return;
      callObj1(c, obj, 0x80042310u);
      const uint8_t s5cur = c->mem_r8(obj + O_SUB_05);
      c->mem_w8(obj + O_SUB_06, 0u);
      c->mem_w8(obj + O_SUB_05, (uint8_t)(s5cur + 1u));
      return;
    }

    case 2: {
      const uint8_t s6 = c->mem_r8(obj + O_SUB_06);
      if (s6 == 0) {
        if (c->mem_r8(obj + O_READY_2B) != 3u) return;
        c->mem_w8(obj + O_SUB_06, 2u);
        call2(c, 1u, 4u, 0x80042354u);
        return;
      }
      if (s6 != 2) return;
      const uint32_t r = sub801398E4(c, obj);
      const uint8_t s5cur = c->mem_r8(obj + O_SUB_05);
      if (r == 2) {
        callObj1(c, obj, 0x80042310u);
        c->mem_w8(obj + O_SUB_06, 0u);
        c->mem_w8(obj + O_SUB_05, (uint8_t)(s5cur + 1u));
      } else if (r == 3) {
        c->mem_w8(obj + O_SUB_06, 3u);
        c->mem_w8(obj + O_SUB_05, (uint8_t)(s5cur + 1u));
      }
      return;
    }

    case 3:
      variant4Phase3(c, obj);
      return;

    case 4: {
      const uint8_t s6 = c->mem_r8(obj + O_SUB_06);
      if (s6 == 1) {
        c->mem_w8(obj + O_SUB_06, 2u);
        const uint32_t mask = c->mem_r32(G_BFE56);
        const uint32_t shift = (uint32_t)c->mem_r8(G_BF870) & 0x1Fu;
        const uint32_t bit = (mask >> shift) & 1u;
        initScript(bit == 0 ? SCRIPT_80149F68 : SCRIPT_80149F98);
        return;
      }
      if (s6 > 1 && s6 == 2) {
        eng(c).script.step(obj);
        if ((int8_t)c->mem_r8(obj + O_PROG_70) != -1) return;
        c->mem_w8(obj + O_SUB_06, 0u);
        callObj1(c, obj, 0x80042310u);
        return;
      }
      if (s6 != 0) return;
      if (c->mem_r8(obj + O_READY_2B) != 3u) return;
      c->mem_w8(obj + O_SUB_06, 1u);
      call2(c, 1u, 1u, 0x80042354u);
      return;
    }
    default:
      return;
  }
}

// ── state 0 (INIT) — asset load + zero fields + variant-specific setup ──────────────────────────
void state0Init(Core* c, uint32_t obj) {
  if (assetInit(c, obj) != 0) return;   // asset not ready yet — retry next frame
  const uint32_t initVal = c->mem_r32(G_ECF90);
  c->mem_w32(obj + O_TABLE_A, TABLE_A);
  c->mem_w32(obj + O_D_3C,    initVal);
  call3(c, obj, 0u, 0u, 0x80041718u);   // obj init2 leaf
  c->mem_w8 (obj + O_KIND,    9u);
  c->mem_w16(obj + O_H_54,    0u);
  c->mem_w16(obj + O_H_58,    0u);
  c->mem_w8 (obj + O_B_BF,    0u);
  c->mem_w8 (obj + O_B_5F,    0u);
  c->mem_w8 (obj + O_READY_2B,0u);
  c->mem_w8 (obj + O_STATE_04, (uint8_t)(c->mem_r8(obj + O_STATE_04) + 1u));

  // Variant-specific field seed (obj[+0x03] selects). Verbatim from disas 0x8013AB00..0x8013ABBC.
  const uint8_t variant = c->mem_r8(obj + O_VARIANT);
  switch (variant) {
    case 0:
      c->mem_w16(obj + O_H_56, 0x0080);
      c->mem_w16(obj + O_H_80, 0x0050);
      c->mem_w16(obj + O_H_82, 0x00A0);
      break;
    case 1:
      c->mem_w16(obj + O_H_56, 0x0C00);
      c->mem_w16(obj + O_H_80, 0x008C);
      c->mem_w16(obj + O_H_82, 0x0118);
      break;
    case 2:
      c->mem_w16(obj + O_H_56, 0x0800);
      c->mem_w16(obj + O_H_80, 0x0050);
      c->mem_w16(obj + O_H_82, 0x00A0);
      break;
    case 3:
      c->mem_w16(obj + O_H_56, 0x0200);
      c->mem_w16(obj + O_H_80, 0x0050);
      c->mem_w16(obj + O_H_82, 0x00A0);
      break;
    case 4: {
      c->mem_w16(obj + O_H_80, 0x0050);
      c->mem_w16(obj + O_H_56, 0x0000);
      c->mem_w16(obj + O_H_82, 0x00A0);
      const uint32_t parent = c->mem_r32(obj + O_D_C4);
      call3(c, parent, 0x13u, 0xFu, 0x80051B04u);        // music/SFX cue
      call3(c, obj, TABLE_A, 0x15u, 0x80077C40u);        // variant-4 registrar
      c->mem_w32(G_BF868, obj);                          // publish self as the "active" v4 actor
      break;
    }
    case 5:
      c->mem_w16(obj + O_H_56, 0x0100);
      c->mem_w16(obj + O_H_80, 0x0050);
      c->mem_w16(obj + O_H_82, 0x00A0);
      break;
    default:
      break;
  }
  c->mem_w16(obj + O_H_84, 0x0088);
  c->mem_w16(obj + O_H_86, 0x00A6);
}

// ── state 1 (RUN) — per-frame body — the variant switch ──────────────────────────────────────────
void state1Run(Core* c, uint32_t obj) {
  callObj1(c, obj, 0x8007778Cu);
  callObj1(c, obj, 0x8004190Cu);

  const uint8_t variant = c->mem_r8(obj + O_VARIANT);
  switch (variant) {
    case 0:
      callObj1(c, obj, 0x801381A8u);
      break;
    case 1: {
      const uint8_t s5 = c->mem_r8(obj + O_SUB_05);
      if (s5 == 1) {
        callObj1(c, obj, 0x80139088u);
      } else if (s5 == 0) {
        if ((c->mem_r8(G_BFA1D) & 2u) == 0 && (int8_t)c->mem_r8(G_BF8D2) != -1)
          c->mem_w8(obj + O_SUB_05, 1u);
        else
          c->mem_w8(obj + O_SUB_05, 2u);
      } else if (s5 == 2) {
        callObj1(c, obj, 0x801392D8u);
      }
      break;
    }
    case 2: {
      // Variant-2: script-driven fade with two script variants (obj[+0x06] gates).
      const uint8_t s6 = c->mem_r8(obj + O_SUB_06);
      if (s6 == 1) {
        c->mem_w8(obj + O_SUB_06, 2u);
        const uint32_t scriptPtr = (c->mem_r8(G_BF90C) == 0) ? SCRIPT_801496E8 : SCRIPT_80149788;
        eng(c).script.init(obj, TABLE_A, scriptPtr);
        c->mem_w8(obj + O_PROG_70, 1u);
      } else if (s6 == 0) {
        if (c->mem_r8(obj + O_READY_2B) == 3u) {
          c->mem_w8(obj + O_SUB_06, 1u);
          call2(c, 1u, 2u, 0x80042354u);
        }
      } else if (s6 == 2) {
        eng(c).script.step(obj);
        if ((int8_t)c->mem_r8(obj + O_PROG_70) == -1) {
          c->mem_w8(obj + O_SUB_06, 0u);
          callObj1(c, obj, 0x80042310u);
        }
      }
      break;
    }
    case 3: {
      // Variant-3: same shape as variant-2 with different script pair + gate byte.
      const uint8_t s6 = c->mem_r8(obj + O_SUB_06);
      if (s6 == 1) {
        c->mem_w8(obj + O_SUB_06, 2u);
        const uint32_t scriptPtr = (c->mem_r8(G_BF90D) == 0) ? SCRIPT_801497B8 : SCRIPT_80149838;
        eng(c).script.init(obj, TABLE_A, scriptPtr);
        c->mem_w8(obj + O_PROG_70, 1u);
      } else if (s6 == 0) {
        if (c->mem_r8(obj + O_READY_2B) == 3u) {
          c->mem_w8(obj + O_SUB_06, 1u);
          call2(c, 1u, 2u, 0x80042354u);
        }
      } else if (s6 == 2) {
        eng(c).script.step(obj);
        if ((int8_t)c->mem_r8(obj + O_PROG_70) == -1) {
          c->mem_w8(obj + O_SUB_06, 0u);
          callObj1(c, obj, 0x80042310u);
        }
      }
      break;
    }
    case 4:
      variant4Sm(c, obj);
      break;
    case 5:
      callObj1(c, obj, 0x8013A81Cu);
      break;
    default:
      break;
  }

  if (c->mem_r8(obj + O_CLEANUP) != 0) callObj1(c, obj, 0x800518FCu);
  c->mem_w8(obj + O_READY_2B, 0u);
}

}  // namespace

// Entry — the per-object handler the object walker's native path invokes via BehaviorDispatch.
// Outer state on obj[+0x04]: 0 = INIT, 1 = RUN (per-frame), 2 = no-op hold, 3 = DESPAWN via
// substrate FUN_8007A624. a0 (c->r[4]) already holds the obj address; BehaviorDispatch set it.
void beh_a06_scripted_actor(Core* c) {
  const uint32_t obj = c->r[4];
  const uint8_t state = c->mem_r8(obj + O_STATE_04);
  switch (state) {
    case 0: state0Init(c, obj); return;
    case 1: state1Run (c, obj); return;
    case 2: /* no-op hold */    return;
    case 3: callObj1(c, obj, 0x8007A624u); return;
    default: return;
  }
}
