// game/ai/sop_intro_events.h — native ports for a cluster of small SOP intro-cutscene state-machine
// leaves. See sop_intro_events.cpp for full RE + §9 re-verify notes per function.
//
// Wired via the shared override registry (RegisterSopIntroEventOverrides, called from
// runtime/recomp/boot.cpp's register_engine_overrides()). sopBeatAdvanceWalk/sopBeatAdvanceNarration
// are reached as op-0x3E (call-fnptr) entries of the pilot's cutscene script at 0x8010CA28, via
// ScriptInterp::callFnptr — whose return value in r[2] is the script stepper's pause/advance code,
// so the override wrappers MUST set c->r[2] (2026-07-10 prologue-vortex root cause #2; the earlier
// "animation-event fn-ptr table 0x8010CA60" claim was falsified by the workflow evidence pass).
#pragma once
#include <cstdint>
class Core;
class Game;

void RegisterSopIntroEventOverrides(Game* game);

// FUN_8010AF60 — per-object scene-beat delay sequencer (beat 2->3 hold, then beat 3->6 hold).
// a0 = node (r4). No return value used by any known caller (draft signature keeps it faithful: u32).
uint32_t sopBeatAdvanceWalk(Core* c);

// FUN_8010B078 — per-object scene-beat-5 (narration) entry sequencer: snaps BG transform block then
// holds 10 frames before latching the narration-ready flag byte. a0 = node (r4).
uint32_t sopBeatAdvanceNarration(Core* c);

// FUN_8010B11C — per-object "orbit path" sub-motion: interpolates position on a circular path around
// a snapshot origin over one full revolution (phase 0x4e wraps at 0x1c00), returns 1 exactly once the
// path completes (state cycles 0->1->2->3->0). a0 = node (r4).
uint32_t sopOrbitPathStep(Core* c);

// FUN_8010B44C — spawn-with-parent + install sopIntroEffectTick as the child's per-frame handler
// (node+0x1C). a0 = parent (r4). Returns the new child node ptr (0 on pool exhaustion).
uint32_t sopIntroEffectSpawn(Core* c);

// FUN_8010B2D4 — the child spawned by sopIntroEffectSpawn's own per-frame tick (dispatched via its
// node+0x1C, so a0 = the child node itself, same ABI as beh_sop_overlay_shadow). Model-attach + orbit-
// gated animation start + script-VM-driven running state + despawn.
void sopIntroEffectTick(Core* c);

// FUN_8010B588 — the "lifted" SOP-intro actor's OWN deeper per-frame sub-tick (called directly via
// rec_dispatch from beh_sop_intro_lifted's state_running, a0 = the lifted actor's node, r4). A 6-state
// SM gated on the shared scene-beat global (0x800BF9B4) that installs three progressively "bigger"
// anim/scene-record stages as the beat advances 2 -> 3 -> 6, plus a script-VM-driven idle state (0/1/6).
void sopLiftedSubtick(Core* c);

// FUN_8010BEAC — generic 4-state "orbiting spark" effect tick. Its address is installed as a handler
// pointer in a MAIN.EXE-resident per-type table (0x800A22B8, NOT SOP-overlay-local data) — i.e. this is
// NOT SOP-scene-specific despite living in the SOP.BIN overlay's address range; the actual spawner was
// not traced this pass (see CONFIDENCE note in the .cpp). a0 = node (r4).
void beh_orbit_spark_effect(Core* c);
