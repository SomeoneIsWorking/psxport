// class BehaviorDispatch — the engine's per-object BEHAVIOR-HANDLER dispatcher. See header.
//
// The native object-behavior table: guest handler address → its PC-native reimplementation + a short
// name. This is the SINGLE registry of which per-object behaviors the engine owns natively (readable
// C) vs. still run as the recomp substrate. Introspectable via `nativeName()` so the `ents` REPL
// diagnostic can flag, per live object, whether its logic is owned. Add a row when you own a new
// behavior; `dispatchNative` + the ents flag both read this table.
//
// Was three free functions in engine_tomba2.cpp — dispatch_obj_method / dispatch_native_behavior /
// behavior_native_name. Promoted here as `class BehaviorDispatch` on Engine so callers reach the
// dispatcher through the object graph (`c->engine.behaviors.dispatchObj(obj, h)`), matching the
// Collision / Bit / Spawn class-instance pattern.
#include "behavior_dispatch.h"
#include "core.h"
#include "game.h"                 // Fps60::current_object (was g_current_object)
#include "scene/engine.h"         // class Engine (for Core::engine)
#include <cstdint>

extern "C" void rec_dispatch(Core* c, uint32_t addr);   // run a function by address (override or interp)

// Behavior entry-point declarations. Each `ov_beh_*_run` lives in its own game/ai/beh_*.cpp file.
// (Kept as free-function pointers in the table below — class-ifying each individual behavior is a
// separate axis; the dispatcher-level class-ification here doesn't require it.)
void ov_sm40558(Core* c);          // 0x80040558 (entity.h)
void ov_beh_scene_ui_trigger_run(Core* c);           // 0x800739AC
void ov_beh_typed_init_scene_trigger_run(Core* c);   // 0x80073CD8
void ov_beh_pickup_collect_trigger_run(Core* c);     // 0x800741DC
void ov_beh_substate_edge_orchestrator_run(Core* c); // 0x8012EB54 (overlay)
void ov_beh_jumptable_release_trigger_run(Core* c);  // 0x80124E74 (overlay)
void ov_beh_typed_table_seed_gate_run(Core* c);      // 0x80133C14 (overlay)
void ov_beh_typed_jumptable_pair_run(Core* c);       // 0x80138FC8 (overlay)
void ov_beh_cull_substate_orchestrator_run(Core* c); // 0x8013259C (overlay)
void ov_beh_id_compare_motion_dispatch_run(Core* c); // 0x80145230 (overlay)
void ov_beh_jumptable_flag_gate_run(Core* c);        // 0x8012D4EC (overlay)
void ov_beh_cull_tick_render_run(Core* c);           // 0x8012D404 (overlay)
void ov_beh_sibling_angle_track_run(Core* c);        // 0x801395C0 (overlay)
void ov_beh_visibility_gate_dispatch_run(Core* c);   // 0x8004C238 (resident)
void ov_beh_record_list_scanner_run(Core* c);        // 0x8004CE14 (resident)
void ov_beh_area_event_dispatch_run(Core* c);        // 0x80071A3C (resident)
void ov_beh_pad_child_linker_run(Core* c);           // 0x8006F2D0 (resident)
void ov_beh_scatter_record_dither_run(Core* c);      // 0x8013C538 (overlay)
void ov_beh_area_threshold_ptr_swap_run(Core* c);    // 0x8013C3F4 (overlay)
void ov_beh_scatter_ramp_machine_run(Core* c);       // 0x8013C9C0 (overlay)
void ov_beh_pure_inner_dispatch_run(Core* c);        // 0x80136D9C (overlay)
void ov_beh_anim_trigger_gates_run(Core* c);         // 0x80129C00 (overlay)
void ov_beh_box_seed_phase_gate_run(Core* c);        // 0x8012A0B8 (overlay)
void ov_beh_typed_anim_spawn_run(Core* c);           // 0x8012DA04 (overlay)
void ov_beh_id_routed_dispatch_run(Core* c);         // 0x80121978 (overlay)
void ov_beh_pure_substate_dispatch_run(Core* c);     // 0x80125E0C (overlay)
void ov_beh_linked_advance_branch_run(Core* c);      // 0x80128760 (overlay)
void ov_beh_typed_init_exit_poker_run(Core* c);      // 0x80118240 (overlay)
void ov_beh_child_trig_motion_run(Core* c);          // 0x8013A900 (overlay)
void ov_beh_prng_velocity_machine_run(Core* c);      // 0x80117658 (overlay)
void ov_beh_quad_record_table_seed_run(Core* c);     // 0x80135D64 (overlay)
void ov_beh_flagbit_timer_machine_run(Core* c);      // 0x8013B2E4 (overlay)
void ov_beh_two_child_steer_run(Core* c);            // 0x80131D08 (overlay)
void ov_beh_single_child_cull_run(Core* c);          // 0x80132400 (overlay)
void ov_beh_twin_record_steer_run(Core* c);          // 0x80133D6C (overlay)
void ov_beh_multi_record_phase_machine_run(Core* c); // 0x80134FD8 (overlay)
void ov_beh_sine_motion_sfx_run(Core* c);            // 0x80136158 (overlay)
void ov_beh_box_rearm_sub_run(Core* c);              // 0x8013ADBC (overlay)
void ov_beh_node3_router_run(Core* c);               // 0x8011CBD0 (overlay)
void ov_beh_actor_move_sm_run(Core* c);              // 0x8011D988 (overlay)
void ov_beh_variant_actor_sm_run(Core* c);           // 0x8011D578 (overlay)
void ov_beh_lift_platform_run(Core* c);              // 0x8013A330 (overlay)
void ov_beh_event_record_machine_run(Core* c);       // 0x80136954 (overlay)
void ov_beh_typed_variant_router_run(Core* c);       // 0x8011C164 (overlay)
void ov_beh_camera_target_follow_run(Core* c);       // 0x80059ED8 (resident)
void ov_beh_cube_text_spawn_run(Core* c);            // 0x8003AD48 (resident)
void ov_beh_area_transition_machine_run(Core* c);    // 0x80127798 (overlay)
void ov_beh_rand_phase_cull_run(Core* c);            // 0x8002918C (resident)
void ov_beh_pos_history_trail_run(Core* c);          // 0x80029B40 (resident)
void ov_beh_variant_overlay_lifecycle_run(Core* c);  // 0x8007DC38 (resident)

namespace {
struct NativeBeh { uint32_t addr; void (*fn)(Core*); const char* name; };
constexpr NativeBeh kTable[] = {
  { 0x80040558u, ov_sm40558,                          "sm40558" },
  { 0x8004CE14u, ov_beh_record_list_scanner_run,      "record_list_scanner" },
  { 0x8006F2D0u, ov_beh_pad_child_linker_run,         "pad_child_linker" },
  { 0x80071A3Cu, ov_beh_area_event_dispatch_run,      "area_event_dispatch" },
  { 0x800739ACu, ov_beh_scene_ui_trigger_run,         "scene_ui_trigger" },
  { 0x80073CD8u, ov_beh_typed_init_scene_trigger_run, "typed_init_scene_trigger" },
  { 0x800741DCu, ov_beh_pickup_collect_trigger_run,   "pickup_collect_trigger" },
  { 0x8012EB54u, ov_beh_substate_edge_orchestrator_run,"substate_edge_orchestrator" },
  { 0x80124E74u, ov_beh_jumptable_release_trigger_run,"jumptable_release_trigger" },
  { 0x80133C14u, ov_beh_typed_table_seed_gate_run,    "typed_table_seed_gate" },
  { 0x80138FC8u, ov_beh_typed_jumptable_pair_run,     "typed_jumptable_pair" },
  { 0x8013259Cu, ov_beh_cull_substate_orchestrator_run,"cull_substate_orchestrator" },
  { 0x80145230u, ov_beh_id_compare_motion_dispatch_run,"id_compare_motion_dispatch" },
  { 0x8012D4ECu, ov_beh_jumptable_flag_gate_run,      "jumptable_flag_gate" },
  { 0x8012D404u, ov_beh_cull_tick_render_run,         "cull_tick_render" },
  { 0x801395C0u, ov_beh_sibling_angle_track_run,      "sibling_angle_track" },
  { 0x8013C538u, ov_beh_scatter_record_dither_run,    "scatter_record_dither" },
  { 0x8013C3F4u, ov_beh_area_threshold_ptr_swap_run,  "area_threshold_ptr_swap" },
  { 0x8013C9C0u, ov_beh_scatter_ramp_machine_run,     "scatter_ramp_machine" },
  { 0x80136D9Cu, ov_beh_pure_inner_dispatch_run,      "pure_inner_dispatch" },
  { 0x80129C00u, ov_beh_anim_trigger_gates_run,       "anim_trigger_gates" },
  { 0x8012A0B8u, ov_beh_box_seed_phase_gate_run,      "box_seed_phase_gate" },
  { 0x8012DA04u, ov_beh_typed_anim_spawn_run,         "typed_anim_spawn" },
  { 0x80121978u, ov_beh_id_routed_dispatch_run,       "id_routed_dispatch" },
  { 0x80125E0Cu, ov_beh_pure_substate_dispatch_run,   "pure_substate_dispatch" },
  { 0x80128760u, ov_beh_linked_advance_branch_run,    "linked_advance_branch" },
  { 0x80118240u, ov_beh_typed_init_exit_poker_run,    "typed_init_exit_poker" },
  { 0x8013A900u, ov_beh_child_trig_motion_run,        "child_trig_motion" },
  { 0x80117658u, ov_beh_prng_velocity_machine_run,    "prng_velocity_machine" },
  { 0x80135D64u, ov_beh_quad_record_table_seed_run,   "quad_record_table_seed" },
  { 0x8013B2E4u, ov_beh_flagbit_timer_machine_run,    "flagbit_timer_machine" },
  { 0x80131D08u, ov_beh_two_child_steer_run,          "two_child_steer" },
  { 0x80132400u, ov_beh_single_child_cull_run,        "single_child_cull" },
  { 0x80133D6Cu, ov_beh_twin_record_steer_run,        "twin_record_steer" },
  { 0x80134FD8u, ov_beh_multi_record_phase_machine_run,"multi_record_phase_machine" },
  { 0x80136158u, ov_beh_sine_motion_sfx_run,          "sine_motion_sfx" },
  { 0x8013ADBCu, ov_beh_box_rearm_sub_run,            "box_rearm_sub" },
  { 0x8011CBD0u, ov_beh_node3_router_run,             "node3_router" },
  { 0x8011D988u, ov_beh_actor_move_sm_run,            "actor_move_sm" },
  { 0x8011D578u, ov_beh_variant_actor_sm_run,         "variant_actor_sm" },
  { 0x8013A330u, ov_beh_lift_platform_run,            "lift_platform" },
  { 0x80136954u, ov_beh_event_record_machine_run,     "event_record_machine" },
  { 0x8011C164u, ov_beh_typed_variant_router_run,     "typed_variant_router" },
  { 0x80059ED8u, ov_beh_camera_target_follow_run,     "camera_target_follow" },
  { 0x8003AD48u, ov_beh_cube_text_spawn_run,          "cube_text_spawn" },
  { 0x80127798u, ov_beh_area_transition_machine_run,  "area_transition_machine" },
  { 0x8004C238u, ov_beh_visibility_gate_dispatch_run, "visibility_gate_dispatch" },  // resident
  { 0x8002918Cu, ov_beh_rand_phase_cull_run,          "rand_phase_cull" },           // resident
  { 0x80029B40u, ov_beh_pos_history_trail_run,        "pos_history_trail" },          // resident
  { 0x8007DC38u, ov_beh_variant_overlay_lifecycle_run,"variant_overlay_lifecycle" },  // resident
};
}  // namespace

void BehaviorDispatch::dispatchObj(uint32_t obj, uint32_t handler) {
  Core* c = this->core;
  uint32_t prev = c->game->fps60.current_object;
  c->game->fps60.current_object = obj;
  c->r[4] = obj;                                     // $a0 — the behaviors read the object here
  if (!dispatchNative(handler)) rec_dispatch(c, handler);
  c->game->fps60.current_object = prev;
}

bool BehaviorDispatch::dispatchNative(uint32_t handler) {
  Core* c = this->core;
  for (const NativeBeh& b : kTable)
    if (b.addr == handler) { b.fn(c); return true; }
  return false;
}

const char* BehaviorDispatch::nativeName(uint32_t handler) const {
  for (const NativeBeh& b : kTable) if (b.addr == handler) return b.name;
  return nullptr;
}
