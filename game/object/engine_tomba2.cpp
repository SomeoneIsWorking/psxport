// Tomba2Engine — the PC-native engine layer (Phase 1: the entity-list walk).
//
// Reimplements Tomba!2's per-frame object driver FUN_8007a904 in native C. The engine OWNS the
// walk; the per-object handlers (gameplay/render logic) STAY as recompiled PSX code, invoked via
// rec_dispatch on the same guest entity structs. This is the seam the user wants: native engine,
// PSX gameplay in PSX guest memory. RE: docs/engine_re.md "entity-list walk".
//
// Faithful to the recomp body (the oracle): two doubly-linked lists (heads T2_OBJLIST_HEAD_1/2),
// each walked via next@+0x24; per node, clear render_flag@+1 then call handler@+0x1c(node) with the
// node in a0. `next` is read BEFORE the handler runs (a handler may unlink/free its own node) and is
// held in a host local, so it survives the handler clobbering guest registers.
//
// The native walk IS the engine now — registered unconditionally (no gating). Was verified native==recomp:
// VRAM bit-identical at frames 4000 and 4720 of real gameplay (1 MB cmp PASS each) — see docs/journal.md.
#include "core.h"
#include "game.h"   // Fps60State::current_object (was g_current_object)
#include "cfg.h"
#include "tomba2_types.h"
#include "margin_render.hpp"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

extern void     rec_dispatch(Core* c, uint32_t addr);   // run a function by address (override or interp)

static int  s_dbg   = -1;
static long s_walks = 0;

// Call one node's handler exactly as the recomp does: a0 = node, jalr *(node+0x1c). Tag g_current_object
// = node around the WHOLE handler so fps60 attributes ALL geometry it submits to this entity (the
// interpolation identity), independent of whether projection happens inside the cull subtree.
// Owned per-object BEHAVIOR handlers (node+0x1C). Each was RE'd 1:1 from its recomp body and carries its
// own full RAM+scratchpad A/B verify gate (sm40558 / obj739acverify / obj741dcverify / obj73cd8verify) —
// they were written but ORPHANED (call_handler always rec_dispatched the raw address). Routing them here
// makes the native bodies LIVE: native-only when the gate channel is off, A/B-vs-recomp when it is on.
// This is the start of owning the entity-behavior layer (the object SYSTEM's logic), top-down by hotness.
void ov_sm40558(Core* c);          // 0x80040558 (entity.h)
void ov_beh_scene_ui_trigger_run(Core* c);    // 0x800739AC (beh_scene_ui_trigger.cpp)
void ov_beh_typed_init_scene_trigger_run(Core* c);    // 0x80073CD8 (beh_typed_init_scene_trigger.cpp)
void ov_beh_pickup_collect_trigger_run(Core* c);    // 0x800741DC (beh_pickup_collect_trigger.cpp)
void ov_beh_substate_edge_orchestrator_run(Core* c); // 0x8012EB54 (beh_substate_edge_orchestrator.cpp — overlay)
void ov_beh_jumptable_release_trigger_run(Core* c); // 0x80124E74 (beh_jumptable_release_trigger.cpp — overlay)
void ov_beh_typed_table_seed_gate_run(Core* c); // 0x80133C14 (beh_typed_table_seed_gate.cpp — overlay)
void ov_beh_typed_jumptable_pair_run(Core* c); // 0x80138FC8 (beh_typed_jumptable_pair.cpp — overlay)
void ov_beh_cull_substate_orchestrator_run(Core* c); // 0x8013259C (beh_cull_substate_orchestrator.cpp — overlay)
void ov_beh_id_compare_motion_dispatch_run(Core* c); // 0x80145230 (beh_id_compare_motion_dispatch.cpp — overlay)
void ov_beh_jumptable_flag_gate_run(Core* c); // 0x8012D4EC (beh_jumptable_flag_gate.cpp — overlay)
void ov_beh_cull_tick_render_run(Core* c); // 0x8012D404 (beh_cull_tick_render.cpp — overlay)
void ov_beh_sibling_angle_track_run(Core* c); // 0x801395C0 (beh_sibling_angle_track.cpp — overlay)
void ov_beh_visibility_gate_dispatch_run(Core* c); // 0x8004C238 (beh_visibility_gate_dispatch.cpp — resident)
void ov_beh_record_list_scanner_run(Core* c); // 0x8004CE14 (beh_record_list_scanner.cpp — resident)
void ov_beh_area_event_dispatch_run(Core* c); // 0x80071A3C (beh_area_event_dispatch.cpp — resident)
void ov_beh_pad_child_linker_run(Core* c); // 0x8006F2D0 (beh_pad_child_linker.cpp — resident)
void ov_beh_scatter_record_dither_run(Core* c); // 0x8013C538 (beh_scatter_record_dither.cpp — overlay)
void ov_beh_area_threshold_ptr_swap_run(Core* c); // 0x8013C3F4 (beh_area_threshold_ptr_swap.cpp — overlay)
void ov_beh_scatter_ramp_machine_run(Core* c); // 0x8013C9C0 (beh_scatter_ramp_machine.cpp — overlay)
void ov_beh_pure_inner_dispatch_run(Core* c); // 0x80136D9C (beh_pure_inner_dispatch.cpp — overlay)
void ov_beh_anim_trigger_gates_run(Core* c); // 0x80129C00 (beh_anim_trigger_gates.cpp — overlay)
void ov_beh_box_seed_phase_gate_run(Core* c); // 0x8012A0B8 (beh_box_seed_phase_gate.cpp — overlay)
void ov_beh_typed_anim_spawn_run(Core* c); // 0x8012DA04 (beh_typed_anim_spawn.cpp — overlay)
void ov_beh_id_routed_dispatch_run(Core* c); // 0x80121978 (beh_id_routed_dispatch.cpp — overlay)
void ov_beh_pure_substate_dispatch_run(Core* c); // 0x80125E0C (beh_pure_substate_dispatch.cpp — overlay)
void ov_beh_linked_advance_branch_run(Core* c); // 0x80128760 (beh_linked_advance_branch.cpp — overlay)
void ov_beh_typed_init_exit_poker_run(Core* c); // 0x80118240 (beh_typed_init_exit_poker.cpp — overlay)
void ov_beh_child_trig_motion_run(Core* c); // 0x8013A900 (beh_child_trig_motion.cpp — overlay)
void ov_beh_prng_velocity_machine_run(Core* c); // 0x80117658 (beh_prng_velocity_machine.cpp — overlay)
void ov_beh_quad_record_table_seed_run(Core* c); // 0x80135D64 (beh_quad_record_table_seed.cpp — overlay)
void ov_beh_flagbit_timer_machine_run(Core* c); // 0x8013B2E4 (beh_flagbit_timer_machine.cpp — overlay)
void ov_beh_two_child_steer_run(Core* c); // 0x80131D08 (beh_two_child_steer.cpp — overlay)
void ov_beh_single_child_cull_run(Core* c); // 0x80132400 (beh_single_child_cull.cpp — overlay)
void ov_beh_twin_record_steer_run(Core* c); // 0x80133D6C (beh_twin_record_steer.cpp — overlay)
void ov_beh_multi_record_phase_machine_run(Core* c); // 0x80134FD8 (beh_multi_record_phase_machine.cpp — overlay)
void ov_beh_sine_motion_sfx_run(Core* c); // 0x80136158 (beh_sine_motion_sfx.cpp — overlay)
void ov_beh_box_rearm_sub_run(Core* c); // 0x8013ADBC (beh_box_rearm_sub.cpp — overlay)
void ov_beh_node3_router_run(Core* c); // 0x8011CBD0 (beh_node3_router.cpp — overlay)
void ov_beh_actor_move_sm_run(Core* c); // 0x8011D988 (beh_actor_move_sm.cpp — overlay)
void ov_beh_variant_actor_sm_run(Core* c); // 0x8011D578 (beh_variant_actor_sm.cpp — overlay)
void ov_beh_lift_platform_run(Core* c); // 0x8013A330 (beh_lift_platform.cpp — overlay)
void ov_beh_event_record_machine_run(Core* c); // 0x80136954 (beh_event_record_machine.cpp — overlay)
void ov_beh_typed_variant_router_run(Core* c); // 0x8011C164 (beh_typed_variant_router.cpp — overlay)
void ov_beh_camera_target_follow_run(Core* c); // 0x80059ED8 (beh_camera_target_follow.cpp — resident)
void ov_beh_cube_text_spawn_run(Core* c); // 0x8003AD48 (beh_cube_text_spawn.cpp — resident)
void ov_beh_area_transition_machine_run(Core* c); // 0x80127798 (beh_area_transition_machine.cpp — overlay)
void ov_beh_rand_phase_cull_run(Core* c); // 0x8002918C (beh_rand_phase_cull.cpp — resident)
void ov_beh_pos_history_trail_run(Core* c); // 0x80029B40 (beh_pos_history_trail.cpp — resident)
void ov_beh_variant_overlay_lifecycle_run(Core* c); // 0x8007DC38 (beh_variant_overlay_lifecycle.cpp — resident)
// The native object-behavior table: guest handler address -> its PC-native reimplementation + a short
// name. This is the SINGLE registry of which per-object behaviors the engine owns natively (readable C)
// vs. still run as the recomp substrate. Introspectable via behavior_native_name() so the `ents`
// diagnostic can flag, per live object, whether its logic is owned (drive/diagnose easier). Add a row
// when you own a new behavior; dispatch_native_behavior + the ents flag both read this table.
struct NativeBeh { uint32_t addr; void (*fn)(Core*); const char* name; };
static const NativeBeh g_native_beh[] = {
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
static bool dispatch_native_behavior(Core* c, uint32_t h) {
  for (const NativeBeh& b : g_native_beh)
    if (b.addr == h) { b.fn(c); return true; }
  return false;
}
// Introspection for the `ents` diagnostic: the native behavior name for handler `h`, or nullptr if the
// object's logic still runs as the recomp substrate (not yet owned).
const char* behavior_native_name(uint32_t h) {
  for (const NativeBeh& b : g_native_beh) if (b.addr == h) return b.name;
  return nullptr;
}

static inline void call_handler(Core* c, uint32_t node) {
  uint32_t prev = c->game->fps60.current_object;
  c->game->fps60.current_object = node;
  c->r[4] = node;                                  // $a0
  uint32_t h = c->mem_r32(node + T2OBJ_HANDLER);
  // DIAG `behhist`: tally distinct per-object behavior handlers (node+0x1C) so we know the concrete
  // entity-behavior set to own natively (the bulk of the still-PSX object SYSTEM logic). (later-230)
  if (cfg_dbg("behhist")) {
    static uint32_t addr[64]; static long cnt[64]; static int nh=0; static long w=0;
    int i=0; for(; i<nh; i++) if(addr[i]==h) break;
    if(i==nh && nh<64){ addr[nh]=h; cnt[nh]=0; nh++; }
    if(i<64) cnt[i]++;
    if((++w % 300)==0){ fprintf(stderr,"[behhist] distinct=%d handlers:\n", nh);
      for(int j=0;j<nh;j++) fprintf(stderr,"   %08X  x%ld\n", addr[j], cnt[j]); }
  }
  if (!dispatch_native_behavior(c, h)) rec_dispatch(c, h);   // owned behavior → native; else PSX leaf
  c->game->fps60.current_object = prev;
}

static void walk_list(Core* c, uint32_t head, long* count) {
  for (uint32_t n = head; n; ) {
    uint32_t next = c->mem_r32(n + T2OBJ_NEXT);       // read next BEFORE the handler may unlink the node
    c->mem_w8(n + T2OBJ_RENDER_FLAG, 0);              // engine clears the per-frame render flag
    call_handler(c, n);
    n = next; (*count)++;
  }
}

// Native FUN_8007a904. Second list head is re-read fresh after list 1 (handlers in list 1 may mutate
// it) — matches the recomp body, which reloads DAT_800f2624 across the first loop.
// Non-static: called DIRECTLY from the native field per-frame update (engine_stage.cpp ov_field_frame).
void ov_objwalk(Core* c) {
  long nodes = 0;
  walk_list(c, c->mem_r32(T2_OBJLIST_HEAD_1), &nodes);
  walk_list(c, c->mem_r32(T2_OBJLIST_HEAD_2), &nodes);

  // Native widescreen margin (later-133): render the objects the wide frustum re-included (collected by
  // ov_object_cull during the walk) via per-node flushes here — after all handlers ran, before the OT is
  // submitted. No +1 poke -> the handlers stayed in their culled branch -> gameplay 0-diff.
  margin_render_flush(c);

  if (s_dbg < 0) s_dbg = cfg_dbg("engine") ? 1 : 0;
  if (s_dbg && (s_walks % 300) == 0)
    fprintf(stderr, "[engine] objwalk #%ld: %ld nodes\n", s_walks, nodes);
  s_walks++;
}

// Route ONE object method natively-or-substrate, mirroring call_handler's dispatch (a0 = object, fps60
// current-object tracking, native behavior if owned else the recomp leaf). Shared by the field-frame
// object dispatchers below. No guest-sp munging: the handler allocates its own frame below the current
// sp, exactly as the guest `jalr` did.
static inline void dispatch_obj_method(Core* c, uint32_t obj, uint32_t h) {
  uint32_t prev = c->game->fps60.current_object;
  c->game->fps60.current_object = obj;
  c->r[4] = obj;                                     // $a0
  if (!dispatch_native_behavior(c, h)) rec_dispatch(c, h);
  c->game->fps60.current_object = prev;
}

// Native FUN_80069B28 — a second per-frame object-list walk (head 0x800F2738; method ptr at node+0x1C,
// next at node+0x24). Read next BEFORE the call (a handler may unlink the node). Faithful to the recomp
// body: unlike ov_objwalk it does NOT clear a render flag. A direct child of the native ov_field_frame
// (was `d0(c, 0x80069b28)`); owning it removes the dispatcher body from the substrate.
void ov_list_walk_69b28(Core* c) {
  for (uint32_t n = c->mem_r32(0x800F2738u); n; ) {
    uint32_t h    = c->mem_r32(n + 0x1Cu);
    uint32_t next = c->mem_r32(n + 0x24u);
    dispatch_obj_method(c, n, h);
    n = next;
  }
}

// Native FUN_8007B04C — the MID-TRANSITION state-3 entity walker (docs/findings/scene.md hut-door
// freeze; decomp scratch/decomp/ram_f1000_all.c L56987-L57017). Walks both entity lists (heads
// T2_OBJLIST_HEAD_1 then T2_OBJLIST_HEAD_2, re-read AFTER list 1 in case handlers mutated it),
// clears each node's per-frame render flag, and — GATED on node[0x28] & 0x80 — dispatches the
// per-object handler via dispatch_obj_method (native beh if owned, substrate otherwise).
//
// Called from engine_stage.cpp's ov_field_frame_x (was `d0(c, 0x8007b04cu)`). Owning it here routes
// mid-transition handler calls through dispatch_native_behavior — so the sub-scene swap state
// machine (SceneTransition::stepSwapWaiter) runs UNDER NATIVE CODE during the transition, not
// hidden inside substrate func_80138FC8. Prerequisite for observing / fixing the case-3
// SECOND-branch deadlock (docs/findings/scene.md).
void ov_state3_walk_8007b04c(Core* c) {
  uint32_t l2 = c->mem_r32(T2_OBJLIST_HEAD_2);           // captured up-front (decomp: iVar3 = DAT_800f2624)
  uint32_t n  = c->mem_r32(T2_OBJLIST_HEAD_1);
  while (n) {
    uint32_t next = c->mem_r32(n + T2OBJ_NEXT);
    c->mem_w8 (n + T2OBJ_RENDER_FLAG, 0);
    if (c->mem_r8(n + 0x28) & 0x80) {
      uint32_t h = c->mem_r32(n + T2OBJ_HANDLER);
      dispatch_obj_method(c, n, h);
    }
    l2 = c->mem_r32(T2_OBJLIST_HEAD_2);                  // re-read each iteration (handler may mutate)
    n  = next;
  }
  n = l2;
  while (n) {
    uint32_t next = c->mem_r32(n + T2OBJ_NEXT);
    c->mem_w8 (n + T2OBJ_RENDER_FLAG, 0);
    if (c->mem_r8(n + 0x28) & 0x80) {
      uint32_t h = c->mem_r32(n + T2OBJ_HANDLER);
      dispatch_obj_method(c, n, h);
    }
    n = next;
  }
}

// Native FUN_80026368 — iterate the 8-slot fixed object array at 0x80100400 (stride 0x4C); for each
// ACTIVE slot (byte[0] != 0) dispatch its method by type byte[2] through the jump table 0x8009D314
// (a0 = slot). Faithful: no type bound-check (the guest indexes the table raw); inactive slots still
// advance. A direct child of the native ov_field_frame (was `d0(c, 0x80026368)`).
void ov_arr8_dispatch_26368(Core* c) {
  for (int i = 0; i < 8; i++) {
    uint32_t slot = 0x80100400u + (uint32_t)i * 0x4Cu;
    if (c->mem_r8(slot) == 0) continue;
    uint32_t type = c->mem_r8(slot + 2);
    uint32_t h = c->mem_r32(0x8009D314u + type * 4u);
    dispatch_obj_method(c, slot, h);
  }
}

void engine_tomba2_init(void) {
  if (cfg_dbg("engine"))
    fprintf(stderr, "[engine] native object-list walk active (FUN_8007a904)\n");
}
