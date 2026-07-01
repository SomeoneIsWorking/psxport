// Tomba2Engine — native types mirroring Tomba! 2's own engine structures.
//
// These mirror the real in-RAM layout of the game's engine objects so the native engine
// (engine_tomba2.c, being lifted from the recompiled MIPS) can read/write the SAME RAM as the
// not-yet-lifted recomp code during the incremental transition. Field offsets are RE'd in
// docs/engine_re.md — VERIFY there before trusting any field. PSX RAM is little-endian.
#ifndef TOMBA2_TYPES_H
#define TOMBA2_TYPES_H
#include <stdint.h>

// ---- the drawable entity (object) ----------------------------------------------------
// Active entities = a doubly-linked list of pool nodes, stride 0xD0 (208 B). Two lists, heads
// T2_OBJLIST_HEAD_1/2. The engine walks them in FUN_8007a904 (native target, Phase 1). Confirmed
// from gameplay RAM dumps + the cull/submit code; see docs/engine_re.md. Use the offset constants
// (T2OBJ_*) to read guest RAM — they're authoritative; the struct below is partial/opaque-padded.
#define T2OBJ_STRIDE 0xD0u         // bytes per pool node

enum {
  T2OBJ_RENDER_FLAG = 0x01,  // u8  cleared each frame by the walk + cull
  T2OBJ_STATE       = 0x04,  // u8  per-type substate (handlers switch on it)
  T2OBJ_TYPE        = 0x0c,  // u8  entity type (cull switch)
  T2OBJ_MODEL_ID    = 0x0e,  // u16 & 0x3fff
  T2OBJ_HANDLER     = 0x1c,  // ptr per-type update/render fn (called with node in a0; gameplay = PSX)
  T2OBJ_PREV        = 0x20,  // ptr prev node
  T2OBJ_NEXT        = 0x24,  // ptr next node (list walk follows this; 0 = end)
  T2OBJ_ID          = 0x2a,  // u16 per-object id (high half of the u16 pair at +0x28; +0x28=state)
  T2OBJ_POS_X       = 0x2e,  // u16
  T2OBJ_POS_Y       = 0x32,  // u16
  T2OBJ_POS_Z       = 0x36,  // u16
  T2OBJ_MODEL_PTR   = 0x38,  // ptr to model data
};

// Entity-list heads (guest globals) + the engine's per-frame object walk.
#define T2_OBJLIST_HEAD_1 0x800FB168u  // DAT_800fb168
#define T2_OBJLIST_HEAD_2 0x800F2624u  // DAT_800f2624
#define T2_OBJWALK_FN     0x8007A904u  // FUN_8007a904: walks both lists, calls each handler@+0x1c

// ---- camera (PSX scratchpad/IO mirror addresses, RE'd) -------------------------------
#define T2_CAM_POS_X   0x1F8000D2u  // u16
#define T2_CAM_POS_Y   0x1F8000D6u  // u16
#define T2_CAM_POS_Z   0x1F8000DAu  // u16
#define T2_CAM_FWD_X   0x1F8000E8u  // s16 forward vector (cull depth dot)
#define T2_CAM_FWD_Y   0x1F8000EAu
#define T2_CAM_FWD_Z   0x1F8000ECu

// ---- task scheduler table (FUN_80051e60) ---------------------------------------------
#define T2_TASK_TABLE  0x801FE000u  // base of the cooperative-task table
#define T2_TASK_STRIDE 0x38u        // bytes per task slot
#define T2_TASK_END    0x801FE14Fu  // last byte walked (~6 slots)

#endif // TOMBA2_TYPES_H
