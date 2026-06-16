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
// Accessed as a raw pointer in the cull/submit code (FUN_8007712c & the submit wrappers).
// Layout is PARTIAL — only the confirmed fields are named; the rest is opaque padding until RE'd.
// Do NOT assume sizeof matches the game's object yet (full size is an open RE item).
typedef struct Tomba2Object {
  uint8_t  _pad0;        // +0x00
  uint8_t  render_flag;  // +0x01  cleared to 0 at cull entry each frame
  uint8_t  _pad2[10];    // +0x02
  uint8_t  type;         // +0x0c  entity type (cull switch + handler dispatch key)
  uint8_t  _pad0d;       // +0x0d
  uint16_t model_id;     // +0x0e  & 0x3fff (set by FUN_80077b38)
  uint8_t  _pad10[0x1c]; // +0x10
  uint16_t pos_x;        // +0x2e
  uint16_t _pad30;       // +0x30
  uint16_t pos_y;        // +0x32
  uint16_t _pad34;       // +0x34
  uint16_t pos_z;        // +0x36
  uint8_t  _pad38a;      // shadow: +0x38 is a pointer; kept opaque below
} Tomba2Object;          // INCOMPLETE — see docs/engine_re.md "Open RE items"

// Field-offset constants (authoritative until the struct is fully RE'd — use these, not the struct
// above, when reading game RAM, to avoid mis-sized padding bugs).
enum {
  T2OBJ_RENDER_FLAG = 0x01,
  T2OBJ_TYPE        = 0x0c,
  T2OBJ_MODEL_ID    = 0x0e,  // u16, & 0x3fff
  T2OBJ_POS_X       = 0x2e,  // u16
  T2OBJ_POS_Y       = 0x32,  // u16
  T2OBJ_POS_Z       = 0x36,  // u16
  T2OBJ_MODEL_PTR   = 0x38,  // ptr to model data
};

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
