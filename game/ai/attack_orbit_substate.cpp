// game/ai/attack_orbit_substate.cpp — see header for the RE writeup + field layout.
#include "attack_orbit_substate.h"
#include "core.h"

extern "C" void rec_dispatch(Core*, uint32_t);

namespace {

// Object field offsets (see attack_orbit_substate.h for the full writeup).
constexpr uint32_t O_KIND      = 0x00;
constexpr uint32_t O_STATE4    = 0x04;
constexpr uint32_t O_PHASE     = 0x07;
constexpr uint32_t O_TARGET    = 0x10;
constexpr uint32_t O_POSSRC    = 0x14;
constexpr uint32_t O_HITGUARD  = 0x1B;
constexpr uint32_t O_SUBFLAG   = 0x2B;
constexpr uint32_t O_POS_X     = 0x2C;
constexpr uint32_t O_AIM_X     = 0x2E;
constexpr uint32_t O_POS_Y     = 0x30;
constexpr uint32_t O_AIM_Y     = 0x32;
constexpr uint32_t O_POS_Z     = 0x34;
constexpr uint32_t O_AIM_Z     = 0x36;
constexpr uint32_t O_AIM_UNUSED= 0x58;
constexpr uint32_t O_TIMER     = 0x40;
constexpr uint32_t O_SPD_X     = 0x48;
constexpr uint32_t O_SPD_Z     = 0x4C;
constexpr uint32_t O_RATE      = 0x4E;
constexpr uint32_t O_ANGLE     = 0x56;
constexpr uint32_t O_FLAGS     = 0x62;
constexpr uint32_t O_ATKID     = 0xC4;
constexpr uint32_t O_TSTATE    = 0x7A;   // target's own state (read via O_TARGET ptr)
constexpr uint32_t O_TSTATE2   = 0x6C;   // target's secondary state
constexpr uint32_t O_TSUB      = 0x5E;   // target's sub-state byte

constexpr uint32_t FN_ACQUIRE_TARGET = 0x8011740Cu;  // still-substrate leaf (out of band)
constexpr uint32_t FN_INIT_A         = 0x801402B8u;  // still-substrate leaf (out of band)
constexpr uint32_t FN_REARM          = 0x801406E4u;  // still-substrate leaf (out of band)
constexpr uint32_t FN_ATTACK_EFFECT  = 0x800331D8u;  // still-substrate leaf (out of band)

}  // namespace

// FUN_801458E0 — node[3]==0x81 sub-behavior: 6-phase acquire/orbit machine, see header for the
// full phase writeup.
void AttackOrbitSubstate::orbitTargetMotion() {
  Core* c = core;
  const uint32_t obj = c->r[4];
  const uint8_t phase = c->mem_r8(obj + O_PHASE);

  if (phase < 6) {
    switch (phase) {
      case 0: {
        c->r[4] = obj; c->r[5] = 0;
        rec_dispatch(c, FN_ACQUIRE_TARGET);            // FUN_8011740C(obj, 0) -> target ptr in r2
        c->mem_w32(obj + O_TARGET, c->r[2]);
        c->mem_w32(obj + O_POSSRC, 0);
        c->mem_w16(obj + O_FLAGS, (uint16_t)(c->mem_r16(obj + O_FLAGS) | 1));
        c->mem_w16(obj + O_ANGLE, 0);
        c->mem_w8(obj + O_PHASE, (uint8_t)(phase + 1));
        [[fallthrough]];
      }
      case 1: {
        c->r[4] = obj; c->r[5] = 0x23; c->r[6] = 8;
        rec_dispatch(c, FN_INIT_A);                     // FUN_801402B8(obj, 0x23, 8)
        c->mem_w16(obj + O_RATE, 0x800);
        c->mem_w16(obj + O_TIMER, 0x14);
        c->mem_w8(obj + O_PHASE, (uint8_t)(c->mem_r8(obj + O_PHASE) + 1));
        [[fallthrough]];
      }
      case 2: {
        int32_t dx = (int16_t)c->mem_r16(obj + O_SPD_X) * (int16_t)c->mem_r16(obj + O_RATE);
        int32_t dz = (int16_t)c->mem_r16(obj + O_SPD_Z) * (int16_t)c->mem_r16(obj + O_RATE);
        int32_t px = (int32_t)c->mem_r32(obj + O_POS_X);
        int32_t pz = (int32_t)c->mem_r32(obj + O_POS_Z);
        if ((c->mem_r16(obj + O_FLAGS) & 1) == 0) { px += dx; pz += dz; }
        else                                       { px -= dx; pz -= dz; }
        c->mem_w32(obj + O_POS_X, (uint32_t)px);
        c->mem_w32(obj + O_POS_Z, (uint32_t)pz);

        uint16_t t = (uint16_t)(c->mem_r16(obj + O_TIMER) - 1);
        c->mem_w16(obj + O_TIMER, t);
        if ((int16_t)t <= 0) {
          c->mem_w8(obj + O_PHASE, (uint8_t)(c->mem_r8(obj + O_PHASE) + 1));
        }
        break;   // case 2 always ends the switch here (matches recomp: no cascade)
      }
      case 3: {
        c->r[4] = obj; c->r[5] = 0x24; c->r[6] = 8;
        rec_dispatch(c, FN_INIT_A);                     // FUN_801402B8(obj, 0x24, 8)
        c->mem_w16(obj + O_TIMER, 0x10);
        c->mem_w8(obj + O_PHASE, (uint8_t)(c->mem_r8(obj + O_PHASE) + 1));
        [[fallthrough]];
      }
      case 4: {
        uint16_t t = (uint16_t)(c->mem_r16(obj + O_TIMER) - 1);
        c->mem_w16(obj + O_TIMER, t);
        if ((int16_t)t > 0) break;
        c->mem_w16(obj + O_TIMER, 0x10);
        c->mem_w8(obj + O_PHASE, (uint8_t)(c->mem_r8(obj + O_PHASE) + 1));
        c->mem_w16(obj + O_FLAGS, (uint16_t)(c->mem_r16(obj + O_FLAGS) | 4));
        break;
      }
      case 5: {
        const uint16_t told = c->mem_r16(obj + O_TIMER);     // pre-decrement value used for the test
        c->mem_w16(obj + O_ANGLE, (uint16_t)((int16_t)c->mem_r16(obj + O_ANGLE) + 0x80));
        c->mem_w16(obj + O_TIMER, (uint16_t)(told - 1));
        if ((int16_t)told > 0) break;
        c->mem_w8(obj + O_PHASE, 1);
        c->mem_w16(obj + O_FLAGS, (uint16_t)(c->mem_r16(obj + O_FLAGS) ^ 1));
        c->mem_w16(obj + O_ANGLE, 0);
        c->mem_w16(obj + O_FLAGS, (uint16_t)(c->mem_r16(obj + O_FLAGS) & 0xfffb));
        break;
      }
      default:
        break;
    }
  }

  // Common tail — every path (including the phase>=6 no-op) reaches this.
  c->r[4] = obj;
  rec_dispatch(c, FN_REARM);                              // FUN_801406E4(obj)
  c->mem_w16(obj + O_AIM_Y, (uint16_t)((int16_t)c->mem_r16(obj + O_AIM_Y) + 0x14));
}

// FUN_80145AF0 — node[3]==0x80 sub-behavior: aim-point recompute + one-shot attack-window trigger,
// see header for the full writeup.
void AttackOrbitSubstate::aimAtTargetAnchor() {
  Core* c = core;
  const uint32_t obj = c->r[4];
  const uint32_t target = c->mem_r32(obj + O_TARGET);
  const uint32_t posSrc = c->mem_r32(obj + O_POSSRC);
  const uint8_t phase = c->mem_r8(obj + O_PHASE);

  if (phase > 2) return;                                  // phase 3+: no-op, no common tail (recomp)
  if (phase != 2) {
    if (phase == 0) {
      c->r[4] = obj; rec_dispatch(c, FN_REARM);            // FUN_801406E4(obj) — phase-0-only re-arm
    } else if (phase != 1) {
      return;                                              // unreachable for a u8, kept for parity
    }
    c->r[4] = obj; c->r[5] = 0x1C; c->r[6] = 0;
    rec_dispatch(c, FN_INIT_A);                             // FUN_801402B8(obj, 0x1c, 0)
    c->mem_w8(obj + O_PHASE, 2);
    c->mem_w16(obj + O_FLAGS, (uint16_t)(c->mem_r16(obj + O_FLAGS) | 1));
    c->mem_w16(obj + O_ANGLE, 0x800);
  }

  // ---- aim-point recompute: self aim-point := posSrc's position + fixed anchor delta ----
  const int16_t sx = (int16_t)c->mem_r16(posSrc + O_POS_X);
  const int16_t sy = (int16_t)c->mem_r16(posSrc + O_POS_Y);
  const int16_t sz = (int16_t)c->mem_r16(posSrc + O_POS_Z);
  c->mem_w16(obj + O_AIM_X, (uint16_t)(sx + 0x28));
  c->mem_w16(obj + O_AIM_Y, (uint16_t)(sy - 0xCD));
  c->mem_w16(obj + O_AIM_UNUSED, 0);
  c->mem_w16(obj + O_AIM_Z, (uint16_t)sz);

  // ---- attack-window check against the captured target ----
  const int16_t tState = (int16_t)c->mem_r16(target + O_TSTATE);
  if (tState == 2 || tState == 6) {
    c->mem_w8(obj + O_SUBFLAG, 0);
  } else {
    if ((int16_t)c->mem_r16(target + O_TSTATE2) != 2) return;
    const uint8_t sub = c->mem_r8(target + O_TSUB);
    if ((uint8_t)(sub - 2) > 1) return;                     // sub must be 2 or 3
    c->mem_w8(obj + O_SUBFLAG, sub == 2 ? (uint8_t)0x80 : (uint8_t)0);
  }

  const uint8_t hitGuard = c->mem_r8(obj + O_HITGUARD);
  c->mem_w32(obj + O_STATE4, 2u);   // packed 4-byte write: outer state=2, ALSO resets node[0x07]

  if ((hitGuard & 0x40) == 0) {
    c->r[4] = obj;
    c->r[5] = c->mem_r32(obj + O_ATKID);
    c->r[6] = (uint32_t)-100;
    c->r[7] = 0;
    rec_dispatch(c, FN_ATTACK_EFFECT);                      // func_0x800331D8(obj, obj[0xC4], -100, 0)
    c->mem_w8(obj + O_HITGUARD, (uint8_t)(hitGuard | 0x40));
    c->mem_w8(obj + O_KIND, 7);
  }
}
