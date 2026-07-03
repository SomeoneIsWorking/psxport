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
// `obj[+0x42]` in the code, each access is a typed named method. Fields introduced by this handler's RE:
//   state / alive / renderMode / type / counterA / sceneMode / counterB / oscBase / oscPhase /
//   triggerParam / stateEcho / boxX/Y/Z/W. `oscPhase` is the SBS target-#4 accumulator; its zero-init
//   here is the state-1-branch reset that ov_a00_gen_801337E4's PRNG (FUN_8009A450, seed at 0x80105EE8)
//   walks each active tick.
//
// STILL OPAQUE (recorded so a future RE arc closes them; the un-owned sub-behaviors here index Actor
// fields the RE didn't yet name completely):
//   - FUN_801337E4   state-1 sub-state machine (5-way jumptable at 0x80109E58). RE'd in this session's
//                    docs finding (docs/findings/sbs.md target-#4 upstream — the full semantic model of
//                    subState 0..4, the oscillator tick body = Trig::rcos(oscPhase)>>5 into
//                    renderRec[+0xC] + oscPhase += 68 + (c->rng.next()>>8), and the turn/scan sub-states
//                    that consult a pilot-actor region at 0x800E7E80). Actor's subState / subFlagX /
//                    retryDelay / targetDelta / renderRec / counterA fields are named from that RE.
//                    NATIVE PORT PENDING: still called via rec_dispatch here so the beh_ handler stays
//                    small; the port lands in the next RE arc using the now-named Actor surface (plus
//                    class Rng already at c->rng.next() and class Trig at c->trig.rcos).
//   - FUN_8004766C   per-object update called after box seed + inside render tail (matrix build?)
//   - FUN_80077EBC   scenePhase==0x22 sub-behavior body
//   - FUN_800778E4   spatial trigger check taking (obj, triggerParam) -> 0/nonzero (IN/OUT)
//   - FUN_80077768   sub-behavior FUN_801337E4 calls with (obj[+0x5F]<<4, obj[+0x56], 0) —
//                    target-direction lookup (returns nonzero → targetDelta = +256, zero → -256).
// Kept as `rec_dispatch` (recomp substrate) — the correct escape hatch until each is RE'd on its own.
//
// PORTED OWNERSHIP RULES (per project CLAUDE.md): CONTROL FLOW + every named-field write owned native,
// byte-for-byte; every still-opaque sub-behavior CALL stays a `rec_dispatch` leaf (a0/a1 set first).
// NO GTE, NO render packets here. Gated byte-exact (full RAM+scratchpad A/B vs rec_super_call) via
// channel "typed_table_seed_gateverify".

#include "core.h"
#include "cfg.h"
#include "spawn.h"            // class Spawn (c->engine.spawn.despawn)
#include "graphics_bind.h"    // GraphicsBind::recordInit / renderUpdate
#include "object/actor.h"     // class Actor + scene_phase / osc_base_table
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {
constexpr uint32_t BEH_FN = 0x80133C14u;

// State-byte enum for this handler class. Matches the sibling handlers' convention (0=init, 1=active,
// 2/3=despawn, >=4=no-op) — named per the sceneUiTrigger / typedInitSceneTrigger RE.
enum class Sta : uint8_t { Init = 0, Active = 1, DespawnA = 2, DespawnB = 3 };

// Opaque sub-behaviors this handler still calls into via rec_dispatch (see file docstring "STILL OPAQUE").
constexpr uint32_t SUB_STATE1_TICK    = 0x801337E4u;  // oscPhase accumulator body (SBS target-#4 upstream)
constexpr uint32_t SUB_OBJ_UPDATE     = 0x8004766Cu;  // per-object update (matrix build?)
constexpr uint32_t SUB_PHASE22_BODY   = 0x80077EBCu;  // scenePhase==0x22 sub-behavior
constexpr uint32_t SUB_TRIGGER_CHECK  = 0x800778E4u;  // spatial trigger check (obj, triggerParam) -> IN/OUT
}  // namespace

void beh_typed_table_seed_gate(Core* c) {
  Actor a(c, c->r[4]);
  const Sta st = (Sta)a.state();

  // ---- dispatch (state -> branch) ------------------------------------------------------------------
  if (st != Sta::Active) {
    if ((uint8_t)st >= (uint8_t)Sta::DespawnA) {
      if ((uint8_t)st >= 4) return;                                // state>=4: no-op epilogue
      c->engine.spawn.despawn(a.addr());                           // 2 or 3: despawn
      return;
    }
    if (st != Sta::Init) return;                                   // impossible slot: no-op

    // ---- STATE 0 (INIT): allocate cull record + seed trigger box + oscillation params ------------
    c->r[4] = a.addr(); c->r[5] = 0xc; c->r[6] = 0x14;             // cls=0xc, sub=0x14
    c->engine.graphicsBind.recordInit();
    if (c->r[2] != 0) return;                                      // record pool busy — retry next tick

    a.setSceneMode(0x22);                                          // participates when scene_phase()==0x22
    a.setBoxX(0x1e); a.setBoxY(0x3c); a.setBoxZ(0x32); a.setBoxW(0x64);
    a.setState((uint8_t)Sta::Active);
    a.setAlive(1);
    a.setCounterA(0);
    a.setCounterB(0);
    c->r[4] = a.addr(); rec_dispatch(c, SUB_OBJ_UPDATE);           // per-object update (opaque)
    a.setOscPhase(0);                                              // target-#4 accumulator reset
    uint16_t seed = osc_base_table(c, a.type());                   // per-type oscBase seed
    a.setTriggerParam(-0xc8);                                      // -200 world units (Y offset?)
    a.setOscBase(seed);
    return;
  }

  // ---- STATE 1 (ACTIVE): tick oscPhase, then gate on global scene phase --------------------------
  c->r[4] = a.addr(); rec_dispatch(c, SUB_STATE1_TICK);            // oscPhase accumulator body (opaque)
  const uint8_t phase = scene_phase(c);

  if (phase < 0x1c) {
    a.setStateEcho((uint16_t)(uint8_t)st);                         // scene not yet in gate range: latch and skip
    return;
  }

  if (phase == 0x22) {
    a.setRenderMode((uint8_t)st);                                  // "this actor's phase" branch
    c->r[4] = a.addr(); rec_dispatch(c, SUB_PHASE22_BODY);         // scenePhase==0x22 sub-behavior (opaque)
    // fall through to render tail
  } else {
    c->r[4] = a.addr(); c->r[5] = (uint32_t)(int32_t)a.triggerParam();
    rec_dispatch(c, SUB_TRIGGER_CHECK);                            // spatial trigger check (opaque)
    if (c->r[2] == 0) {                                            // OUT: latch stateEcho and skip
      a.setStateEcho((uint16_t)(uint8_t)st);
      return;
    }
    // IN: fall through to render tail
  }

  // ---- render tail: per-object update + render-state update ---------------------------------------
  c->r[4] = a.addr(); rec_dispatch(c, SUB_OBJ_UPDATE);
  c->r[4] = a.addr(); c->engine.graphicsBind.renderUpdate();
}
