// game/ai/beh_typed_table_seed_gate.cpp — PC-native per-object BEHAVIOR handler FUN_80133C14 (OVERLAY).
//
// SEMANTIC MODEL (from RE of disas 0x80133C14..0x80133D64 + sibling handlers FUN_800739AC / FUN_80073CD8):
//   This is a "CULL-RECORD SCENE-PHASE ACTOR": a per-object handler that
//     - at INIT (state 0)  allocates a render/cull record, seeds its trigger-box and per-type oscillation
//                          base, then advances to ACTIVE.
//     - while ACTIVE (state 1) reads the GLOBAL SCENE-PHASE gate byte 0x800E7EAA:
//         - if scenePhase < 0x1C   -> latch stateEcho and skip (scene not yet in gate range)
//         - if scenePhase == 0x22  -> "this actor's phase": mark renderMode + run the phase-specific
//                                     sub-behavior (FUN_80077EBC) + render
//         - else                   -> spatial trigger check FUN_800778E4(obj, triggerParam); if it
//                                     reports OUT (0) latch stateEcho and skip; if IN, render
//     - on DESPAWN (state 2 or 3) calls Spawn::despawn(obj) and drops.
//     - state >= 4                 no-op.
//
// The FIELD SEMANTICS the handler reads/writes are named on class Actor (game/object/actor.h) — no more
// `obj[+0x42]` in the code, each access is a typed named method.
//
// STATE-1 TICK sub-behavior (guest FUN_801337E4) IS NOW OWNED NATIVE HERE (state_one_tick, below). RE'd
// from disas 0x801337E4..0x80133C10 on scratch/bin/field_ram_230.bin. Semantic model documented in
// docs/findings/sbs.md ("FUN_801337E4 RE'd semantic model") — a 5-way sub-state machine on subState
// (jumptable at 0x80109E58: INIT / MAIN OSCILLATOR / POST-COUNTER-A / TURN-DIRECTION / TURN-EXECUTE)
// driving a background-actor oscillate + occasionally-turn animation using Trig::rcos(oscPhase) and the
// PC-native class Rng (rngOf(c).next() implements FUN_8009A450 with seed at 0x80105EE8).
//
// STILL OPAQUE (recorded so a future RE arc closes them):
//   - FUN_8004766C   PER-OBJECT TILE-BASED MOVEMENT STEP. RE'd shape (disas 0x8004766C..0x80047774):
//                      * SNAPSHOT posX/posY/posZ (obj+0x2E/0x32/0x36 halfwords) into the shared TILE
//                        WORKSPACE scratchpad at 0x1F8001BC/0x1BE/0x1C0.
//                      * FUN_80047778(obj) — tile-clamp helper; adjusts the snapshot vs current tile
//                        bounds using scratch 0x1AA/0x1AC (tile origin X/Z), 0x1AE/0x1B0 (tile span),
//                        0x1C8 (tile-record ptr), 0x1FE (last-counterA byte). Also reads Actor::counterA
//                        and may call 0x800490E4 / 0x80048ECC / 0x80048FC4 on a threshold miss.
//                      * FUN_80049968(counterA) — per-type table lookup keyed by Actor::counterA.
//                        RE'd fully (disas 0x80049968..0x800499E4 — pure address arithmetic, no calls,
//                        no branches). Shape: base=*(u32)0x1F8001C8 (per-area tile-pool root),
//                        record=base+*(u16)(base+counterA*2)*2 (2-level halfword indirection),
//                        then publishes FIVE sub-pointers into the tile workspace:
//                            0x1F8001CC = record + 20               (record TAIL, after 20-byte hdr)
//                            0x1F8001D0 = record + *(u16)(rec+12)*2
//                            0x1F8001D4 = record + *(u16)(rec+14)*2
//                            0x1F8001D8 = record + *(u16)(rec+16)*2
//                            0x1F8001DC = record + *(u16)(rec+18)*2
//                        Consumed by FUN_80047CBC (uses 0x1CC + 0x1D0), FUN_80048034 (0x1D8), and
//                        the command-stream pump (0x1E0/0x1E4 also loaded from record). Cleanest
//                        port candidate of the tile-move leaves — will land as a static helper on
//                        the future `class TileMove` when the parent (FUN_8004766C) is ported.
//                      * FUN_80047CBC — tile-collision predicate; on 0 skip the rest and return.
//                      * When positive: pump a command/stream at *(0x1F8001E0) whose halfword @0 masks
//                        with 0x4000 — advances counterA (byte @[stream+0]) while set. Then call
//                        FUN_80048134 (writes delta X into 0x1C2) + FUN_80048034 (writes delta Z into
//                        0x1C6, uses scratch 0x1D8/0x1EC).
//                      * INTEGRATE: posX = snapX + deltaX (0x1BC + 0x1C2); posZ = snapZ + deltaZ
//                        (0x1C0 + 0x1C6). posY (obj+0x32) is NOT written back — Y is owned by the
//                        oscillator branch in state_one_tick. Writes both scratch slot and Actor field.
//                    This is the same "probe-result / tile-workspace" scratchpad region (0x1F8001A0..)
//                    that game/object/actor_sm_24448.cpp + game/ai/beh_camera_target_follow.cpp already
//                    reference for heading/floor-type — a FIELD-WIDE SHARED SURFACE. Native ownership
//                    of this cluster (5 leaves) needs its own arc — most likely a `class TileMove` on
//                    Engine with `Actor::tileMoveStep()` as the thin wrapper.
//   - 0x800E7E80     "pilot-actor" region read by state_one_tick's TURN sub-states (mode @+0x147, yaw
//                    @+0x58, state @+0x168). Likely Tomba's actor slot; future RE candidate for a
//                    `class PilotActor` view.
//   - 0x8014A6F4     per-type CLAMP-BAND table {lo, hi} for oscBase (case [2] pilot-consult clamp).
// Kept as `rec_dispatch` (recomp substrate) — the correct escape hatch until each is RE'd on its own.
//
// PORTED OWNERSHIP RULES (per project CLAUDE.md): CONTROL FLOW + every named-field write owned native,
// byte-for-byte; every still-opaque sub-behavior CALL stays a `rec_dispatch` leaf (a0/a1 set first).
// NO GTE, NO render packets here. Gated byte-exact (full RAM+scratchpad A/B vs rec_super_call) via
// channel "typed_table_seed_gateverify".

#include "core.h"
#include "game_ctx.h"
#include "cfg.h"
#include "spawn.h"            // class Spawn (eng(c).spawn.despawn)
#include "graphics_bind.h"    // GraphicsBind::recordInit / renderUpdate
#include "rng.h"              // class Rng (rngOf(c).next()) — FUN_8009A450 (seed 0x80105EE8)
#include "trig.h"             // class Trig (trigOf(c).rcos)  — FUN_80083F50 (Q12 cos LUT)
#include "object/actor.h"     // class Actor + scene_phase / osc_base_table
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {
constexpr uint32_t BEH_FN = 0x80133C14u;

enum class Sta : uint8_t { Init = 0, Active = 1, DespawnA = 2, DespawnB = 3 };

// Un-RE'd sub-behaviors still called via rec_dispatch by this handler. (FUN_80077768 turn-direction
// lookup is NATIVE now — Trig::angleCmp; see run_turn_setup.)
// FUN_8004766C = per-object TILE-BASED MOVEMENT STEP (snapshot pos → tile-clamp → per-type table
// lookup → collision predicate → command-stream pump → integrate deltaX/deltaZ back into posX/posZ).
// See top-of-file "STILL OPAQUE" block for the full RE'd shape + leaf cluster.
constexpr uint32_t SUB_TILE_MOVE_STEP = 0x8004766Cu;

// Pilot-actor region (0x800E7E80..) — read by state_one_tick's TURN sub-states.
constexpr uint32_t PILOT_BASE      = 0x800E7E80u;
constexpr uint32_t PILOT_YAW_OFF   = 0x58;   // halfword
constexpr uint32_t PILOT_MODE_OFF  = 0x147;  // byte
constexpr uint32_t PILOT_STATE_OFF = 0x168;  // byte

// Per-type CLAMP-BAND halfword pair {lo, hi} for oscBase in case [2]. Stride 4.
constexpr uint32_t TBL_A6F4 = 0x8014A6F4u;

// state_one_tick's subFlagX turn-setup path — shared by case [1] and case [4]. Reads obj[+0x56] /
// obj[+0x5F], sets subState=4, dispatches SUB_TURN_LOOKUP, writes targetDelta = ±256, clears subFlagX.
// Returns true when the turn-setup ran (caller should return to epilogue).
bool run_turn_setup(Actor& a) {
  Core* c = a.core();
  const uint32_t obj = a.addr();
  int16_t rotY = a.rotY();
  uint8_t facing = c->mem_r8(obj + 0x5F);            // per-actor "target facing" byte (angle >> 4)
  a.setSubState(4);
  // Turn direction: signed-half angle compare — "does target-facing lead current rotY by π/4..3π/4?"
  // Returns 1 for TURN LEFT (+256), 0 for TURN RIGHT (-256). Was rec_dispatch(0x80077768); now native
  // via class Trig (game/math/trig.h — Trig::angleCmp is FUN_80077768 owned static).
  int32_t leadCw = Trig::angleCmp((int32_t)facing << 4, (int32_t)rotY, 0);
  a.setTargetDelta((leadCw != 0) ? (int16_t)256 : (int16_t)-256);
  a.setSubFlagX(0);
  return true;
}

// state_one_tick — PC-native port of FUN_801337E4 (guest overlay 0x801337E4..0x80133C10). 5-way
// sub-state machine dispatched on Actor::subState via the guest jumptable at 0x80109E58. See file
// docstring + docs/findings/sbs.md for the semantic model.
void state_one_tick(Actor& a) {
  Core* c = a.core();
  const uint32_t obj = a.addr();

  // ---- Section A — reset transients when stateEcho != 0 -----------------------------------------
  if (a.stateEcho() != 0) {
    uint16_t seed = osc_base_table(c, a.type());   // per-type oscBase seed
    uint32_t rec  = a.renderRec();
    a.setStateEcho(0);
    a.setSubState(0);
    a.setOscBase(seed);
    c->mem_w16(rec + 0xC, 0);
    a.setTargetDelta(0);
  }

  // ---- Section B — dispatch on subState ---------------------------------------------------------
  uint8_t sub = a.subState();
  if (sub >= 5) return;                              // no-op epilogue

  // Case [0] INIT (0x80133868): clear oscPhase, advance to subState=1, then FALL INTO case [1] body.
  if (sub == 0) {
    a.setOscPhase((int16_t)0);
    a.setSubState(1);
    sub = 1;
  }

  // Case [1] MAIN OSCILLATOR TICK (0x80133874).
  if (sub == 1) {
    if (a.counterA() != 0) {
      // "trigger fired" → fall through into case [2] body with subState=2 (matches guest bne fall-in
      // at 0x8013387C with delay-slot v0=2).
      a.setSubState(2);
      sub = 2;
    } else if (a.subFlagX() != 0) {
      // Turn-setup path (case [1] subFlagX branch): sets targetDelta ± 256 and returns.
      run_turn_setup(a);
      return;
    } else {
      // Oscillator gate: throttle by retryDelay, then Trig::rcos(oscPhase) + PRNG-jittered accumulate.
      if (a.retryDelay() != 0) {
        a.setRetryDelay((uint8_t)(a.retryDelay() - 1));
        return;
      }
      // THE OSCILLATOR TICK — target-#4 hot path.
      int32_t cos_q12 = trigOf(c).rcos((int32_t)a.oscPhase());   // Trig::rcos(oscPhase) → Q12 signed
      c->mem_w16(a.renderRec() + 0xC, (uint16_t)((uint32_t)cos_q12 >> 5));   // guest srl (unsigned)
      int32_t r = rngOf(c).next();                               // Rng::next() → [0, 0x7FFF]
      uint16_t newPhase = (uint16_t)(a.oscPhase_u() + 68 + ((uint32_t)r >> 8));
      a.setOscPhase(newPhase);
      return;
    }
  }

  // Case [2] POST-COUNTER-A (0x8013392C). counterA==0 → advance to subState=3. counterA!=0 → consult
  // the pilot-actor region, write renderRec[+0xC] as ±pilotYaw, then (if pilotState >= 3) recompute
  // oscBase from osc_base_table ± (pilotState - 2) * 6 and clamp against the per-type CLAMP-BAND
  // pair {lo, hi} at TBL_A6F4[type].
  if (sub == 2) {
    if (a.counterA() == 0) {
      a.setSubState(3);
      return;
    }
    uint8_t  pilotMode  = c->mem_r8 (PILOT_BASE + PILOT_MODE_OFF);
    uint16_t pilotYaw   = c->mem_r16(PILOT_BASE + PILOT_YAW_OFF);
    uint32_t rec        = a.renderRec();
    uint16_t angleWrite = (pilotMode != 0) ? (uint16_t)(uint32_t)(-(int32_t)pilotYaw)
                                            : pilotYaw;
    c->mem_w16(rec + 0xC, angleWrite);

    uint8_t pilotState = c->mem_r8(PILOT_BASE + PILOT_STATE_OFF);
    if (pilotState < 3) return;

    // pilotState >= 3: recompute oscBase.
    uint16_t delta = (uint16_t)((pilotState - 2u) * 6u);
    uint16_t newOscBase = (pilotMode != 0)
      ? (uint16_t)(a.oscBase_u() - delta)
      : (uint16_t)(a.oscBase_u() + delta);
    a.setOscBase(newOscBase);

    // Clamp band {lo, hi} at TBL_A6F4[type*4]. Guest logic (from disas 0x801339E0..0x80133A38):
    //   (a) if (int16)newOscBase < (int16)hi  →  oscBase = hi
    //   (b) if (int16)lo < (int16)oscBase     →  oscBase = lo, exit
    uint8_t type = a.type();
    uint32_t tblRow = TBL_A6F4 + (uint32_t)type * 4u;
    uint16_t hi_u = c->mem_r16(tblRow + 2);
    int16_t  hi_s = (int16_t)hi_u;
    if ((int16_t)newOscBase < hi_s) a.setOscBase(hi_u);
    uint16_t lo_u = c->mem_r16(tblRow + 0);
    int16_t  lo_s = (int16_t)lo_u;
    if (lo_s < a.oscBase()) {
      a.setOscBase(lo_u);
    }
    return;
  }

  // Case [3] TURN-DIRECTION SEED (0x80133A3C). Picks targetDelta from {32, 48, 64, 128} keyed by
  // pilotState, negates when pilotMode != 0, advances subState=4.
  if (sub == 3) {
    uint8_t pilotState = c->mem_r8(PILOT_BASE + PILOT_STATE_OFF);
    int16_t seed;
    if (pilotState == 1)      seed = 48;
    else if (pilotState == 0) seed = 32;
    else if (pilotState == 2) seed = 64;
    else                      seed = 128;   // pilotState >= 3
    a.setTargetDelta(seed);
    uint8_t pilotMode = c->mem_r8(PILOT_BASE + PILOT_MODE_OFF);
    if (pilotMode != 0) a.setTargetDelta((int16_t)-a.targetDelta());
    a.setSubState(4);
    return;
  }

  // Case [4] TURN-EXECUTE (0x80133AB4). counterA path loops back to state 1. subFlagX path runs the
  // shared turn-setup. Else: consume targetDelta into renderRec[+0xC], wrap sum into signed 12-bit
  // [-2048, 2047] range, decay targetDelta toward zero by 8 (clamped) then adjust by ±20 based on
  // angle sign. When |angle|<32 && |delta|<20, reset both to 0 and subState=0 (loop back to INIT).
  if (sub == 4) {
    if (a.counterA() != 0) {
      a.setSubState(1);
      return;
    }
    if (a.subFlagX() != 0) {
      run_turn_setup(a);
      return;
    }
    uint32_t rec = a.renderRec();
    uint16_t curDelta_u = (uint16_t)a.targetDelta();     // lhu 68(s0) — unsigned in guest
    uint16_t curAngle_u = c->mem_r16(rec + 0xC);         // lhu 12(v0)

    // Sum + wrap-into-signed-12-bit — v1 = a1 + a0 (unsigned add), test (v1+2048) & 0xFFFF < 4097.
    uint16_t newAngle_u = (uint16_t)(curAngle_u + curDelta_u);
    uint16_t testU = (uint16_t)(newAngle_u + 2048u);
    if (!(testU < 4097u)) {
      uint16_t masked = (uint16_t)(newAngle_u & 0x0FFFu);
      newAngle_u = (masked >= 2049u) ? (uint16_t)(masked | 0xF000u) : masked;
    }

    // Decay targetDelta by 8 toward zero (clamp at 0 on overshoot).
    int16_t curDelta_s = (int16_t)curDelta_u;
    int16_t decayed;
    if (curDelta_s > 0) {
      int16_t stepped = (int16_t)(curDelta_s - 8);
      decayed = (stepped >= 0) ? stepped : (int16_t)0;
    } else {
      int16_t stepped = (int16_t)(curDelta_s + 8);
      decayed = (stepped <= 0) ? stepped : (int16_t)0;
    }
    // Additional ±20 based on angle sign — NOT clamped to zero (guest lets it cross).
    int16_t newAngle_s = (int16_t)newAngle_u;
    decayed = (int16_t)((newAngle_s > 0) ? (decayed - 20) : (decayed + 20));

    // Reset check: |angle| < 32 && |decayed| < 20 → zero both + subState=0.
    int16_t absA = (newAngle_s < 0) ? (int16_t)-newAngle_s : newAngle_s;
    if (absA < 32) {
      int16_t absD = (decayed < 0) ? (int16_t)-decayed : decayed;
      if (absD < 20) {
        newAngle_u = 0;
        decayed = 0;
        a.setSubState(0);
      }
    }
    a.setTargetDelta(decayed);
    c->mem_w16(rec + 0xC, newAngle_u);
    return;
  }
}
}  // namespace

void beh_typed_table_seed_gate(Core* c) {
  Actor a(c, c->r[4]);
  const Sta st = (Sta)a.state();

  // ---- dispatch (state -> branch) ------------------------------------------------------------------
  if (st != Sta::Active) {
    if ((uint8_t)st >= (uint8_t)Sta::DespawnA) {
      if ((uint8_t)st >= 4) return;                                // state>=4: no-op epilogue
      eng(c).spawn.despawn(a.addr());                           // 2 or 3: despawn
      return;
    }
    if (st != Sta::Init) return;                                   // impossible slot: no-op

    // ---- STATE 0 (INIT): allocate cull record + seed trigger box + oscillation params ------------
    c->r[4] = a.addr(); c->r[5] = 0xc; c->r[6] = 0x14;             // cls=0xc, sub=0x14
    eng(c).graphicsBind.recordInit();
    if (c->r[2] != 0) return;                                      // record pool busy — retry next tick

    a.setSceneMode(0x22);                                          // participates when scene_phase()==0x22
    a.setBoxX(0x1e); a.setBoxY(0x3c); a.setBoxZ(0x32); a.setBoxW(0x64);
    a.setState((uint8_t)Sta::Active);
    a.setAlive(1);
    a.setCounterA(0);
    a.setCounterB(0);
    c->r[4] = a.addr(); rec_dispatch(c, SUB_TILE_MOVE_STEP);       // init snap: seed posX/posZ via tile step
    a.setOscPhase((int16_t)0);                                     // target-#4 accumulator reset
    uint16_t seed = osc_base_table(c, a.type());                   // per-type oscBase seed
    a.setTriggerParam(-0xc8);                                      // -200 world units (Y offset?)
    a.setOscBase(seed);
    return;
  }

  // ---- STATE 1 (ACTIVE): tick the sub-state machine (native), then gate on global scene phase ----
  state_one_tick(a);                                               // native — was rec_dispatch(0x801337E4)
  const uint8_t phase = scene_phase(c);

  if (phase < 0x1c) {
    a.setStateEcho((uint16_t)(uint8_t)st);                         // scene not yet in gate range: latch and skip
    return;
  }

  if (phase == 0x22) {
    a.setRenderMode((uint8_t)st);                                  // "this actor's phase" branch
    eng(c).cull.enqueueVisibleClass4(a.addr());                  // FUN_80077EBC — Cull::enqueueVisibleClass4 (was rec_dispatch)
    // fall through to render tail
  } else {
    if (a.boundsCullYOffset(a.triggerParam()) == 0) {              // FUN_800778E4 — Actor::boundsCullYOffset (native)
      a.setStateEcho((uint16_t)(uint8_t)st);                       // OUT: latch stateEcho and skip
      return;
    }
    // IN: fall through to render tail
  }

  // ---- render tail: per-frame tile move (integrates posX/posZ) + render-state update ---------------
  c->r[4] = a.addr(); rec_dispatch(c, SUB_TILE_MOVE_STEP);
  c->r[4] = a.addr(); eng(c).graphicsBind.renderUpdate();
}
