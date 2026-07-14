// S2 boot driver: allocate ONE Core instance, load MAIN.EXE into its RAM, and run the boot stub
// (which draws SCEA then hands off to the native MAIN boot). The runtime is now object-oriented —
// the machine state lives in a `Core` (core.h), reached explicitly (no global). main() owns the
// instance; everything it calls receives `c`.
#include "core.h"
#include "game.h"
#include "cfg.h"
#include "fs_util.h"       // Fs::exists — MAIN.EXE presence probe for self-provisioning below
#include "sbs.h"           // class Sbs — the PSXPORT_SBS live-two-core divergence debugger
#include "platform_hle.h"  // class PlatformHle — HW-sync HLE table (VSync/CdSync/MDEC/ChangeThread)
#include "dualcore.h"      // class DualCore — NATIVE-render vs PSX-render RAM divergence harness
#include "actor_sm_reward.h"  // class ActorReward — reward/tally window actor SM family
#include "actor_zoned_attacker.h"  // class ActorZonedAttacker — 0x8014xxxx zoned-attacker sub-behavior cluster
#include "overlay_gt3gt4.h"        // class OverlayGt3Gt4 — A00-overlay GT3/GT4 packet-emitter cluster
#include "overlay_ground_gt3gt4.h" // class OverlayGroundGt3Gt4 — A00-overlay GROUND/SCENE GT3/GT4 cluster
#include "widescreen_margin_quad.h" // class WidescreenMarginQuad — A00-overlay widescreen-margin OT.GT4 emitter (0x8013CDD4)
#include "quad_rtpt_submit.h"      // class QuadRtptSubmit — 0x8003xxxx rope/flame quad rotate+RTPT submit
#include "node_xform.h"            // class NodeXform — per-object child-transform-propagate family
#include "graphics_bind.h"         // class GraphicsBind — object render-bind subsystem (recordArrayInit)
#include "cube_text_ledger.h"      // class CubeTextLedger — cube-text popup ledger (activate/deactivate/spawn)
#include "actor_tomba.h"           // class ActorTomba — Tomba's postInteractWalk sub-handler leaves
#include "actor_melee_engage.h"    // class ActorMeleeEngage — A00-overlay melee-engage/reposition/arm leaf
#include "melee_proximity.h"       // class MeleeProximity — melee-proximity/approach-anchor leaf
#include "cutscene_camera.h"       // class CutsceneCamera — resetFollowAccum/pushMode/restoreMode/snapToMasterOffsetY200/orbitTick
#include "sop_intro_events.h"      // RegisterSopIntroEventOverrides — SOP intro-cutscene sub-tick/sub-motion/timer cluster
#include "demo.h"                  // class Demo — DEMO main-menu title cursor sub-machine (registerOverrides)
#include <stdio.h>

// Free-function beh_* wide-RE clusters (verified+wired this pass) — same "class-ifying is a
// separate axis" acceptance behavior_dispatch.cpp's own table already uses for this family.
void RegisterBehToySpawnFamilyOverrides(Game* game);              // game/ai/beh_toy_spawn_family.cpp (0x80127420/801274BC/80127720/8012763C/80127510)
void RegisterBehActorTombaProximityCombatOverride(Game* game);    // game/ai/beh_actor_tomba_proximity_combat.cpp (0x800527C8)
#include <stdlib.h>
#include <string.h>

// C subsystems (compiled as C) reached across the boundary — declare with C linkage.
extern "C" {
  void watchdog_init(void); void mdec_init(void); void spu_init(void);
}

static uint32_t rd32(const uint8_t* p) { return p[0] | p[1]<<8 | p[2]<<16 | (uint32_t)p[3]<<24; }

void load_exe(const char* path, Core* c) {   // non-static: the dual-core harness loads two cores
  FILE* f = fopen(path, "rb");
  if (!f) { perror(path); exit(1); }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t* buf = (uint8_t*)malloc(n);
  if (fread(buf, 1, n, f) != (size_t)n) { fprintf(stderr, "short read\n"); exit(1); }
  fclose(f);
  uint32_t entry = rd32(buf+0x10), gp = rd32(buf+0x14);
  uint32_t load = rd32(buf+0x18), tsize = rd32(buf+0x1C), sp = rd32(buf+0x30);
  memcpy(&c->ram[load & 0x1FFFFF], buf + 0x800, tsize);
  free(buf);
  c->r[28] = gp;                       // gp
  c->r[29] = sp ? sp : 0x801FFFF0u;    // sp
  c->r[30] = c->r[29];                 // fp
  c->r[31] = 0xDEAD0000u;              // ra sentinel (top-level return)
  fprintf(stderr, "loaded %s: entry 0x%08X load 0x%08X text 0x%X sp 0x%08X\n",
          path, entry, load, tsize, c->r[29]);
}

// rec_dispatch_miss now lives in hle.c (routes A0/B0/C0 to the HLE BIOS).

// register_engine_overrides — wire every EngineOverrides-based native handler onto ONE Game
// instance's OWN table (per-Game; NOT the process-global g_override[]/g_ov_<mod>_override[]
// tables some of these ALSO dual-wire via shard_set_override).
//
// CRITICAL GAP found + fixed here (2026-07-08, docs/findings/tooling.md "SBS/DualCore/Selftest
// never populate their own Game's EngineOverrides table"): this call used to live ONLY inline in
// main() below, run once against a single THROWAWAY Game before main() decided which harness to
// hand off to. That throwaway Game's side effects on the PROCESS-GLOBAL g_override[] tables (via
// shard_set_override — PlatformHle sync HLEs, ActorReward, and PcScheduler after today's fix)
// persist for every Game created afterward, so those specific overrides happened to still work
// under SBS/DualCore/Selftest. But `rec_dispatch`'s EngineOverrides check
// (`c->game->engine_overrides.run(c, addr)`) reads the CALLING Core's OWN Game — and SBS/
// DualCore/Selftest construct their own Game instances (sbs.cpp mA/mB, dualcore.cpp, selftest.cpp)
// and NEVER called this registration block on them. Their `engine_overrides` table was completely
// EMPTY. Result: every override wired ONLY via EngineOverrides::register_ (Animation,
// ActorZonedAttacker, Spawn, ReleaseTriggerMotion — anything without its own shard_set_override
// dual-registration) was COMPLETELY INERT in SBS/DualCore/Selftest: `run()` always returned false
// on both sides, so BOTH SBS cores silently ran the identical SUBSTRATE body for those addresses
// — a byte-exact "pass" that proved nothing (native vs itself would have been the same shape had
// it fired; native never fired at all). Fix: call this once per Game, right after each harness's
// own Game/Core is constructed (see sbs.cpp, dualcore.cpp, selftest.cpp), not just on main()'s
// single throwaway instance.
void register_engine_overrides(Game* game) {
  Core* c = &game->core;
  game->pcSched.registerOverrides();         // yield/spawn/spawn-and-wait/close (0x80051F80 etc.)
  c->math.registerOverrides();                // GTE matMul/applyMatlv/applyMatrixLV/rotmat/rotX/Y/Z (0x80084110 etc.)
  c->engine.animation.registerOverrides();   // loadFrame/advanceLinkChain/attach/applyFrame (0x80076904 etc.)
  c->engine.areaSlots.registerOverrides();   // primeCountdown/updateCell (0x80074A38/0x8007496C)
  c->engine.musicCoord.registerOverrides();  // setGain2 (0x80075D24)
  ActorReward::registerOverrides(game);      // reward/tally window actor SM family
  ActorZonedAttacker::registerOverrides(game); // 0x8014xxxx zoned-attacker sub-behavior cluster
  c->engine.spawn.registerTypedChildOverrides();     // A00-overlay typed-child spawners
  c->engine.releaseTriggerMotion.registerOverrides(); // release-trigger sub-motion cluster
  OverlayGt3Gt4::registerOverrides(game);            // A00-overlay GT3/GT4 packet emitters (0x801465EC/801467BC)
  OverlayGroundGt3Gt4::registerOverrides(game);      // A00-overlay GROUND/SCENE GT3/GT4 + entity loop (0x8013FB88/8013FE58/801401B8)
  WidescreenMarginQuad::registerOverrides(game);     // A00-overlay widescreen-margin OT.GT4 quad emitter (0x8013CDD4)
  QuadRtptSubmit::registerOverrides(game);           // rope/flame quad rotate+RTPT submit (0x8003B054/8003B320)
  NodeXform::registerOverrides(game);                // seedBlock/propagateRotmat/propagateAxis/buildAxis/copyMatrixBlock/buildFromChild/worldPosFromLocal/worldPosFromComposed (0x800517BC/80051300/80051464/80051C8C/80051B34/80051614/80051D90/80051D20)
  GraphicsBind::registerOverrides(game);             // recordArrayInit (0x800519E0)
  c->engine.cull.registerOverrides();                // cullWrapper family (0x8007778C/800777FC/80077ACC/800779D0/80077A4C/800778E4)
  CubeTextLedger::registerOverrides(game);           // cube-text popup ledger activate/deactivate/spawn (0x80040B48/80040C00/80040AA4)
  ActorTomba::registerOverrides(game);               // postInteractWalk sub-handlers (0x80020364/800205CC/800235A0/80022C78)
  ActorMeleeEngage::registerOverrides(game);         // A00-overlay melee-engage/reposition/arm leaf (0x80112188)
  MeleeProximity::registerOverrides(game);           // melee-proximity/approach-anchor leaf (0x8001F9DC)
  CutsceneCamera::registerOverrides(game);           // resetFollowAccum/pushMode/restoreMode/snapToMasterOffsetY200/orbitTick (0x8006E8F8/8006E1C0/8006E1E4/8006EA00/8006EF38)
  RegisterBehToySpawnFamilyOverrides(game);          // toy/child spawner leaves (0x80127420/801274BC/80127720/8012763C/80127510)
  RegisterBehActorTombaProximityCombatOverride(game);// enemy-vs-Tomba proximity-combat FSM (0x800527C8)
  c->engine.sequencer.registerOverrides();           // libsnd SsSeqCalled cluster (0x80090BD0 etc.)
  c->engine.script.registerOverrides();              // cutscene-script opcodes 05/06/34/36/31 (0x80042090/800420AC/80042E10/80043108/80041468)
  RegisterSopIntroEventOverrides(game);               // SOP intro-cutscene sub-tick/sub-motion/timer cluster (0x8010AF60/8010B078/8010B11C/8010B2D4/8010B44C/8010BEAC — sopLiftedSubtick 0x8010B588 deliberately unwired, docs/findings/ai.md)
  Demo::registerOverrides(game);      // main-menu title cursor sub-machine (0x80106AC4) — the r16/r17
  // register-liveness gap that blocked this wire (docs/findings/ai.md "Demo::s3SubMachine r16
  // register-liveness SBS divergence") is FIXED (2026-07-10): s3SubMachine's own port was missing the
  // `r17 = 0x1F800000` scratch-register prep ov_demo_gen_80106AC4:333 does right before calling
  // 0x80106824 — that instruction's only purpose is a post-call re-read of *0x1F800138, but 0x80106824
  // spills the INCOMING r17 to its own guest stack (sp+36) before restoring it, so the value must
  // match for byte-exact SBS. Root cause was never r16 (that was already correct) — see the finding.
}

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "scratch/bin/tomba2/MAIN.EXE";
  Game* game = new Game();    // the whole machine (owns the Core + every subsystem's state — no globals)
  Core* c = &game->core;      // the CPU/RAM handle threaded through the interp (2 MB RAM lives in Game)
  // Self-provision MAIN.EXE: anyone with just a CHD (drop-in *.chd in the repo root, or
  // PSXPORT_TOMBA2_DISC / .env) can run the binary directly — no prior ./run.sh extraction step.
  // The disc backend resolves the CHD itself (disc.c resolve_disc_path); overlays and all other
  // content are read from the disc at runtime, so MAIN.EXE is the only file to materialize.
  if (!Fs::exists(path)) {
    fprintf(stderr, "[boot] %s missing — extracting from disc\n", path);
    if (!disc_extract_file(&game->disc, "\\MAIN.EXE", path)) {
      fprintf(stderr, "[boot] extraction failed: provide a disc (PSXPORT_TOMBA2_DISC, .env, or a "
                      "*.chd in the working directory) or run ./run.sh\n");
      return 1;
    }
  }
  // Default: pc_skip=true — the native shortcut path that ./run.sh has always used. Set
  // PSXPORT_PC_SKIP=0 to route everything through the fiber substrate (slow; audit mode).
  { const char* e = getenv("PSXPORT_PC_SKIP");
    if (e && *e && strcmp(e, "0") == 0) game->pc_skip = false; }
  c->game->gpu_gpu.tritest();                  // PSXPORT_VK_TRITEST=1: GPU triangle-rasterizer self-test, then exit (needs the GpuGpuState)
  void watchdog_init(void);
  watchdog_init();            // PSXPORT_WATCHDOG=<sec>: abort+backtrace if a frame stalls
  load_exe(path, c);
  void games_tomba2_init(void);
  void card_overrides_init(Game*);
  void threads_init(Core*);
  void threads_register_overrides(void);
  void gte_init(void);
  void mdec_init(void);
  void spu_init(void);
  gte_init();               // GTE (COP2) coprocessor, lifted from Beetle
  mdec_init();              // MDEC video decoder (FMV), lifted from Beetle
  spu_init();               // SPU audio core, lifted from Beetle
  game->spu_audio.init();   // SDL audio output sink (PSXPORT_NOAUDIO to disable)
  game->gpu.gpu_native_init();   // native GPU renderer (parses the game's GP0 stream)
  game->cd.overridesInit(); // native CD: drive-ready + by-LBA read (S3)
  games_tomba2_init();      // Tomba2 per-game overrides (vblank pacing)
  game->platform_hle.initBuiltins();   // HW sync/wait stalls -> native non-stall (VSync/CdSync/MDEC)
  // register_engine_overrides(game) is called further down, ONLY on the plain (no-harness) path —
  // NOT here. It used to run unconditionally on this `game`, even though PSXPORT_DUALCORE/
  // SELFTEST/SBS below construct and drive their OWN separate Game instances (dc_boot_init calls
  // register_engine_overrides on those), leaving THIS registration a dead throwaway whenever one of
  // those harnesses is selected. That throwaway registration corrupted `ovhit`'s target selection
  // (docs/findings/animation.md "ovhit tooling caveat", 2026-07-10 fix): it always registers FIRST
  // (before any harness-owned Game exists) and is psx_fallback=0, so a "first non-fallback
  // registrant wins" pick — or any other order-based heuristic — always lands on this inert Game
  // instead of the real SBS core A. Registering it only when it's actually the Game that ends up
  // driven removes the ambiguity at the source instead of working around it downstream.
  c->game->pad.overridesInit();    // native controller input (per-VBlank pad read override)
  card_overrides_init(game);// native memory card (synchronous file-backed libcard I/O)
  threads_init(c);          // native BIOS threads (ucontext); main = slot 0
  threads_register_overrides();
  c->r[4] = 1; c->r[5] = 0;  // a0=argc-ish, a1=argv (BIOS sets these; minimal)

  // Replicate the REAL PSX boot path. The disc's boot executable is the SCUS_944.54 *stub* (not
  // MAIN.EXE): it draws the SCEA "…America Presents" screen itself, then BIOS-LoadExec's
  // cdrom:\MAIN.EXE;1 and jumps to MAIN's entry. We run the stub as the real entry (interpreted —
  // it isn't recompiled) and intercept its LoadExec to hand off to the native MAIN boot
  // (native_boot.c, later 33/34). See docs/journal.md "later 34" + [[psxport-scea-boot-stub]].
  // PSXPORT_DUALCORE: NATIVE-render vs PSX-render guest-RAM divergence harness (dualcore.cpp). Diagnostic
  // (user 2026-06-24): the native renderer corrupts guest RAM the gameplay reads — this runs the same
  // native-gameplay game twice (native render vs PSX render) and diffs guest RAM per frame to find the
  // corrupting write. It creates its own Game instances, so the primary `c`/`game` here is left unused.
  if (cfg_on("PSXPORT_DUALCORE")) {
    DualCore dc;
    dc.run(path);
    return 0;
  }
  // PSXPORT_SELFTEST: headless TDD regression (selftest.cpp) — boots a single full-PSX (psx_fallback) core,
  // drives it like a player, and asserts a behavior (e.g. mash-Start reaches field free-roam). Exit code is
  // the pass/fail (0/1), so CI/`./run.sh` can gate on it. Owns its own Game instance.
  if (cfg_str("PSXPORT_SELFTEST")) {
    int selftest_run(const char* exe_path);
    return selftest_run(path);
  }
  // PSXPORT_SBS: LIVE side-by-side two-core divergence debugger (sbs.cpp). Two native-boot cores in
  // lockstep with identical input, differing only by mode (render / gameplay / both); auto-pauses on the
  // first guest-RAM divergence and serves the divergence + guest backtraces over the debug server. Like
  // DUALCORE it owns its own Game instances, so the primary `c`/`game` above is left unused.
  // Setting PSXPORT_SBS_MODE is enough to enable the harness (no need to ALSO set PSXPORT_SBS=1) —
  // a mode selection with the harness off was just a foot-gun. PSXPORT_SBS=1 alone still works (default mode).
  if (cfg_on("PSXPORT_SBS") || cfg_str("PSXPORT_SBS_MODE")) {
    Sbs::run(path);
    return 0;
  }
  // Plain (no-harness) path: THIS `game` is the one actually driven, so register its
  // EngineOverrides table here (see the comment above where this used to run unconditionally).
  register_engine_overrides(game);     // ALL EngineOverrides clusters (PcScheduler/Animation/
                                        // ActorReward/ActorZonedAttacker/Spawn/ReleaseTriggerMotion/
                                        // GT3GT4/Math)
  game->stub.run(path);                  // stub draws SCEA, then hands off to native MAIN boot
  fprintf(stderr, "[boot] boot stub returned\n");
  return 0;
}
