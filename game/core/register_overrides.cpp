// register_overrides.cpp — the Tomba!2 override-registration block (game_iface.h seam).
//
// MOVED here VERBATIM from runtime/recomp/boot.cpp (2026-07-17 framework/game decoupling): this is
// the clean game→framework seam already (every call is an overrides::install via a subsystem's
// registerOverrides), it just used to live in the framework boot file, dragging ~25 game #includes
// into runtime/recomp/. It now runs via the GameHooks::registerOverrides callback (game_hooks.cpp),
// so the framework never names any game type.
//
// register_engine_overrides — call each subsystem's registerOverrides(), which installs its native
// handlers into the ONE process-global override registry (overrides::install, override_registry.h:
// one { native, gen } entry per address, oracle-leg-gated dispatch shared by the g_<mod>_override[]
// thunks and rec_dispatch). Installing is idempotent — re-registering the same address just
// overwrites the same global slot — so it's safe to call once per harness-owned Game (main(),
// dc_boot_init) and twice under SBS full (mA + mB). See the ordering note in native_boot.cpp: this
// MUST run before crt0_setup/game_init so the init prefix can reach a registered thunk.
#include "core.h"
#include "game_ctx.h"
#include "game.h"
#include "engine.h"
#include "actor_sm_reward.h"       // class ActorReward — reward/tally window actor SM family
#include "actor_zoned_attacker.h"  // class ActorZonedAttacker — 0x8014xxxx zoned-attacker sub-behavior cluster
#include "overlay_gt3gt4.h"        // class OverlayGt3Gt4 — A00-overlay GT3/GT4 packet-emitter cluster
#include "input.h"                 // class Input — SPU voice-table init leaf (0x80093650)
#include "overlay_ground_gt3gt4.h" // class OverlayGroundGt3Gt4 — A00-overlay GROUND/SCENE GT3/GT4 cluster
#include "tile_grid_layer.h"       // class TileGridLayer — A00-overlay field scroll-wrap + tile-grid sprite emitter (0x8011534C/0x80115598)
#include "widescreen_margin_quad.h" // class WidescreenMarginQuad — A00-overlay widescreen-margin OT.GT4 emitter (0x8013CDD4)
#include "hud_gauge_emitter.h"     // class HudGaugeEmitter — self-contained HUD gauge emitter (0x8004FD30/0x8004FB4C)
#include "quad_rtpt_submit.h"      // class QuadRtptSubmit — 0x8003xxxx rope/flame quad rotate+RTPT submit
#include "node_xform.h"            // class NodeXform — per-object child-transform-propagate family
#include "graphics_bind.h"         // class GraphicsBind — object render-bind subsystem (recordArrayInit)
#include "cube_text_ledger.h"      // class CubeTextLedger — cube-text popup ledger (deactivate/spawn)
#include "scene/scene_events.h"    // class SceneEvents — scene-event ARM (FUN_80040B48 sole owner)
#include "math/trig.h"             // class Trig — rsin/ratan2/angleCmp override wiring
#include "actor_tomba.h"           // class ActorTomba — Tomba's postInteractWalk sub-handler leaves
#include "actor_melee_engage.h"    // class ActorMeleeEngage — A00-overlay melee-engage/reposition/arm leaf
#include "melee_proximity.h"       // class MeleeProximity — melee-proximity/approach-anchor leaf
#include "cutscene_camera.h"       // class CutsceneCamera — resetFollowAccum/pushMode/restoreMode/snapToMasterOffsetY200/orbitTick
#include "sop_intro_events.h"      // RegisterSopIntroEventOverrides — SOP intro-cutscene sub-tick/sub-motion/timer cluster
#include "demo.h"                  // class Demo — DEMO main-menu title cursor sub-machine (registerOverrides)

// Free-function beh_* wide-RE clusters (verified+wired) — same "class-ifying is a separate axis"
// acceptance behavior_dispatch.cpp's own table already uses for this family.
void RegisterBehToySpawnFamilyOverrides(Game* game);              // game/ai/beh_toy_spawn_family.cpp (0x80127420/801274BC/80127720/8012763C/80127510)
void RegisterEngineAnimLeafOverrides(Game* game);                 // game/core/engine.cpp (0x8004190C animTick / 0x80054D14 walkStart)
void RegisterBehActorTombaProximityCombatOverride(Game* game);    // game/ai/beh_actor_tomba_proximity_combat.cpp (0x800527C8)
void register_field_owned_leaves();                               // BYTE-FAITHFUL batch of 94 field-spine leaves

void register_engine_overrides(Game* game) {
  Core* c = &game->core;
  // PcScheduler primitives: the framework class supplies the native handlers; the game passes the
  // generated substrate bodies + override setter (linked here, game-side) — P1.7c decoupling.
  extern void shard_set_override(uint32_t, void (*)(Core*));
  extern void gen_func_80051F80(Core*), gen_func_80051F14(Core*), gen_func_80044BD4(Core*),
              gen_func_80052010(Core*), gen_func_80051FB4(Core*);
  game->pcSched.registerOverrides({ shard_set_override, gen_func_80051F80, gen_func_80051F14,
                                    gen_func_80044BD4, gen_func_80052010, gen_func_80051FB4 });
  mathOf(c).registerOverrides();                // GTE matMul/applyMatlv/applyMatrixLV/rotmat/rotX/Y/Z (0x80084110 etc.)
  eng(c).animation.registerOverrides();   // loadFrame/advanceLinkChain/attach/applyFrame (0x80076904 etc.)
  eng(c).areaSlots.registerOverrides();   // primeCountdown/updateCell (0x80074A38/0x8007496C)
  eng(c).musicCoord.registerOverrides();  // setGain2 (0x80075D24)
  ActorReward::registerOverrides(game);      // reward/tally window actor SM family
  eng(&game->core).installFieldTransitions();  // ov_game field-transition sub-machine handlers
  ActorZonedAttacker::registerOverrides(game); // 0x8014xxxx zoned-attacker sub-behavior cluster
  eng(c).spawn.registerTypedChildOverrides();     // A00-overlay typed-child spawners
  eng(c).releaseTriggerMotion.registerOverrides(); // release-trigger sub-motion cluster
  OverlayGt3Gt4::registerOverrides(game);            // A00-overlay GT3/GT4 packet emitters (0x801465EC/801467BC)
  OverlayGroundGt3Gt4::registerOverrides(game);      // A00-overlay GROUND/SCENE GT3/GT4 + entity loop (0x8013FB88/8013FE58/801401B8)
  TileGridLayer::registerOverrides(game);            // A00-overlay field scroll-wrap + tile-grid sprite emitter (0x8011534C/0x80115598)
  WidescreenMarginQuad::registerOverrides(game);     // A00-overlay widescreen-margin OT.GT4 quad emitter (0x8013CDD4)
  HudGaugeEmitter::registerOverrides(game);          // self-contained HUD gauge emitter (0x8004FD30/0x8004FB4C)
  QuadRtptSubmit::registerOverrides(game);           // rope/flame quad rotate+RTPT submit (0x8003B054/8003B320)
  NodeXform::registerOverrides(game);                // seedBlock/propagateRotmat/propagateAxis/buildAxis/copyMatrixBlock/buildFromChild/worldPosFromLocal/worldPosFromComposed (0x800517BC/80051300/80051464/80051C8C/80051B34/80051614/80051D90/80051D20)
  GraphicsBind::registerOverrides(game);             // recordArrayInit (0x800519E0)
  eng(c).cull.registerOverrides();                // cullWrapper family (0x8007778C/800777FC/80077ACC/800779D0/80077A4C/800778E4)
  eng(c).collision.registerOverrides();           // field-collision leaf cluster (0x80045810/80048034/80048134/80048360/80049760)
  eng(c).bgSceneTransitionSm.registerOverrides(); // BG scene-transition opcode leaves (0x80042758/80042884)
  eng(c).audioDispatch.registerOverrides();       // field-audio BGM start/override leaves (0x80075024/80075070)
  eng(c).sfx.registerOverrides();                 // SFX trigger wrapper (0x80074810)
  Input::registerOverrides(game);                    // SPU voice-table init leaf (0x80093650)
  SceneEvents::registerOverrides(game);              // scene-event ARM FUN_80040B48 (sole owner; deduped from cube_text_ledger)
  CubeTextLedger::registerOverrides(game);           // cube-text popup ledger deactivate/spawn (0x80040C00/80040AA4)
  ActorTomba::registerOverrides(game);               // postInteractWalk sub-handlers (0x80020364/800205CC/800235A0/80022C78)
  ActorMeleeEngage::registerOverrides(game);         // A00-overlay melee-engage/reposition/arm leaf (0x80112188)
  MeleeProximity::registerOverrides(game);           // melee-proximity/approach-anchor leaf (0x8001F9DC)
  Engine::registerFieldSeqSchedulerTick();           // per-frame field sequence scheduler (0x80075A80)
  Engine::registerAnnouncerCuePush();                // announcer/message cue queue push (0x8004FA38)
  Engine::registerSpawnType6Node();                  // type-6 pool-node spawn helper (0x800310F4)
  register_field_owned_leaves();                      // BYTE-FAITHFUL batch of 94 field-spine leaves
  Engine::registerFieldTargetCursor();               // field target-select cursor (0x800251F0)
  CutsceneCamera::registerOverrides(game);           // resetFollowAccum/pushMode/restoreMode/snapToMasterOffsetY200/orbitTick (0x8006E8F8/8006E1C0/8006E1E4/8006EA00/8006EF38)
  RegisterBehToySpawnFamilyOverrides(game);          // toy/child spawner leaves (0x80127420/801274BC/80127720/8012763C/80127510)
  RegisterEngineAnimLeafOverrides(game);             // Engine::animTick/walkStart fallthrough native-ize (0x8004190C/80054D14)
  Trig::registerOverrides(game);                     // Trig::rsin/ratan2/angleCmp fallthrough native-ize (0x80083E80/80085690/80077768)
  RegisterBehActorTombaProximityCombatOverride(game);// enemy-vs-Tomba proximity-combat FSM (0x800527C8)
  eng(c).sequencer.registerOverrides();           // libsnd SsSeqCalled cluster (0x80090BD0 etc.)
  eng(c).script.registerOverrides();              // cutscene-script opcodes 05/06/34/36/31 (0x80042090/800420AC/80042E10/80043108/80041468)
  RegisterSopIntroEventOverrides(game);               // SOP intro-cutscene sub-tick/sub-motion/timer cluster (0x8010AF60/8010B078/8010B11C/8010B2D4/8010B44C/8010BEAC — sopLiftedSubtick 0x8010B588 deliberately unwired, docs/findings/ai.md)
  Demo::registerOverrides(game);      // main-menu title cursor sub-machine (0x80106AC4) — the r16/r17
  // register-liveness gap that blocked this wire (docs/findings/ai.md "Demo::s3SubMachine r16
  // register-liveness SBS divergence") is FIXED (2026-07-10): s3SubMachine's own port was missing the
  // `r17 = 0x1F800000` scratch-register prep ov_demo_gen_80106AC4:333 does right before calling
  // 0x80106824 — that instruction's only purpose is a post-call re-read of *0x1F800138, but 0x80106824
  // spills the INCOMING r17 to its own guest stack (sp+36) before restoring it, so the value must
  // match for byte-exact SBS. Root cause was never r16 (that was already correct) — see the finding.
}
