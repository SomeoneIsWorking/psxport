// dualview_snapshot.cpp — save/restore FULL guest state for the dual-view (native | PSX side-by-side) render
// harness. Split out of native_boot.cpp (2026-07 restructure): a self-contained diagnostic render-compare
// utility with its own static buffers, unrelated to booting/frame-stepping.
#include "core.h"
#include "dualview_snapshot.h"
#include <string.h>

// ---- DUAL-VIEW guest-state snapshots (native | PSX side-by-side render of ONE game state) -------------
// Two snapshots of guest state (main RAM + scratchpad + GTE regs): "pre" = post-gameplay/pre-render
// (captured in ov_field_frame by dv_snapshot, before the native render consumes per-frame queues) — the
// PSX render pass runs from this; "post" = the real post-frame canonical state, restored after the PSX
// pass so the running game is unaffected by the extra render. See native_step_frame's dual-view block.
extern "C" { int g_dv_have_pre = 0; }
static uint8_t  s_dv_pre_ram[0x200000],  s_dv_post_ram[0x200000];
static uint8_t  s_dv_pre_spad[0x400],    s_dv_post_spad[0x400];
static uint32_t s_dv_pre_gc[32], s_dv_pre_gd[32], s_dv_post_gc[32], s_dv_post_gd[32];
static void dv_save(Core* c, uint8_t* ram, uint8_t* spad, uint32_t* gc, uint32_t* gd) {
  uint32_t gte_read_ctrl(uint32_t), gte_read_data(uint32_t);
  memcpy(ram, c->ram, 0x200000); memcpy(spad, c->scratch, 0x400);
  for (int i = 0; i < 32; i++) { gc[i] = gte_read_ctrl(i); gd[i] = gte_read_data(i); }
}
static void dv_load(Core* c, const uint8_t* ram, const uint8_t* spad, const uint32_t* gc, const uint32_t* gd) {
  void gte_write_ctrl(uint32_t,uint32_t), gte_write_data(uint32_t,uint32_t);
  memcpy(c->ram, ram, 0x200000); memcpy(c->scratch, spad, 0x400);
  for (int i = 0; i < 32; i++) { gte_write_ctrl(i, gc[i]); gte_write_data(i, gd[i]); }
}
// called from ov_field_frame (engine_stage.cpp) right before the native render. Captures the
// post-gameplay / pre-render guest state UNCONDITIONALLY: it feeds both the dual-view PSX pass AND the
// always-on "PSX render underneath" (user 2026-06-24: the native renderer must leave no guest-memory side
// effects; the PSX render runs from this snapshot to keep guest memory correct). RAM+scratchpad+GTE.
extern "C" void dv_snapshot(Core* c) { dv_save(c, s_dv_pre_ram, s_dv_pre_spad, s_dv_pre_gc, s_dv_pre_gd); g_dv_have_pre = 1; }
extern "C" void dv_capture_post(Core* c) { dv_save(c, s_dv_post_ram, s_dv_post_spad, s_dv_post_gc, s_dv_post_gd); }
extern "C" void dv_restore_pre (Core* c) { dv_load(c, s_dv_pre_ram,  s_dv_pre_spad,  s_dv_pre_gc,  s_dv_pre_gd ); }
extern "C" void dv_restore_post(Core* c) { dv_load(c, s_dv_post_ram, s_dv_post_spad, s_dv_post_gc, s_dv_post_gd); }
