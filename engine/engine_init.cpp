// PC-native engine STARTUP/INIT — reimplementing the engine's own init from the game's main entry point
// (crt0 -> FUN_80050b08 = ov_game_main). The engine boot does platform/PSX-library setup (ResetGraph,
// CdInit, VSync, DMA, heap — owned by the native platform) and then sets up the engine's OWN state:
// frame/double-buffer pacing, the display + GTE projection, and the camera. Those engine pieces are pure
// state setters with no PSX mechanism to preserve, so they are reimplemented PC-native here (readable C
// that sets the same engine state), replacing the 1:1 `rec_dispatch` transcription in ov_game_main.
//
// Per the boundary (CLAUDE.md): this is ENGINE init → reimplement PC-native. The values/widths are the
// engine's interface state that the rest of the engine + the retained PSX content read back, so they are
// set EXACTLY as the original (verified against the MAIN.EXE disassembly — store widths matter; e.g. the
// vblank counter is a halfword, the pacing/parity flags are bytes). RE source: scratch/decomp +
// MAIN.EXE disasm (tools/recomp decode). later-159.
#include "core.h"
#include <stdint.h>

void rec_dispatch(Core*, uint32_t);   // run a guest fn (for the few sub-bits not yet PC-native)

// FUN_80051794 — set an identity 3x3 rotation matrix (0x1000 = 1.0 fixed on the diagonal) + zero
// translation: 8 contiguous words [R11|R12, R13|R21, R22|R23, R31|R32, R33, 0, 0, 0]. (PSX "InitMatrix".)
static inline void eng_identity_matrix(Core* c, uint32_t p) {
  c->mem_w32(p + 0,  0x1000); c->mem_w32(p + 4,  0);
  c->mem_w32(p + 8,  0x1000); c->mem_w32(p + 12, 0);
  c->mem_w32(p + 16, 0x1000); c->mem_w32(p + 20, 0);
  c->mem_w32(p + 24, 0);      c->mem_w32(p + 28, 0);
}

// FUN_80050a0c — engine frame-state init: zero the vblank counter and the double-buffer / frame-pacing
// flags the main loop reads (DAT_1f800235 = frame-rate divisor, DAT_1f800135 = buffer parity,
// DAT_1f80019c = buffer-swap mode). Last write = FUN_8009a480(0x45) -> DAT_80105ee8 = 0x45 (a word).
void eng_init_framestate(Core* c) {
  c->mem_w16(0x800E809C, 0);     // vblank counter (sh)
  c->mem_w8 (0x1F800235, 2);     // frame-rate divisor / vblank pacing target
  c->mem_w8 (0x1F800135, 0);     // double-buffer parity
  c->mem_w8 (0x1F80019C, 0);     // buffer-swap mode flag
  c->mem_w8 (0x1F80023B, 0);
  c->mem_w8 (0x1F800233, 0);
  c->mem_w8 (0x1F800236, 0);
  c->mem_w8 (0x1F800234, 0);
  c->mem_w8 (0x1F80019B, 0);
  c->mem_w8 (0x1F80027E, 0);
  c->mem_w32(0x80105EE8, 0x45);  // FUN_8009a480(0x45)
}

// FUN_800509b4 — engine DISPLAY + GTE-projection init, PC-native. Sets the GTE projection control regs
// (the camera/projection the renderer reads: H, OFX/OFY, ZSF, DQ) and the projection-plane global. The
// libgte InitGeom (FUN_80083ff8) only does these CR writes + installs a cop2-unusable exception handler
// (FUN_80085810) that our always-on native GTE does not need, so it reduces to the CR writes here.
// FUN_80050738 (the PSX double-buffer draw/disp env structs) is still dispatched — those structs are read
// by native_step_frame's PutDrawEnv/PutDispEnv; a PC-native single display env is the next display step.
void eng_init_display(Core* c) {
  c->mem_w16(0x800E7E70, 0);     // DAT_800e7e70
  c->mem_w16(0x800E7E72, 8);     // DAT_800e7e72
  // InitGeom (FUN_80083ff8): GTE projection control registers.
  gte_write_ctrl(29, 0x155);            // ZSF3
  gte_write_ctrl(30, 0x100);            // ZSF4
  gte_write_ctrl(26, 1000);             // H (replaced by SetGeomScreen below)
  gte_write_ctrl(27, 0xFFFFEF9E);       // DQA
  gte_write_ctrl(28, 0x01400000);       // DQB
  gte_write_ctrl(24, 0);                // OFX
  gte_write_ctrl(25, 0);                // OFY
  // SetGeomOffset(160,120) (FUN_800846d0): screen-center projection offset, (x,y) << 16.
  gte_write_ctrl(24, (uint32_t)160 << 16);
  gte_write_ctrl(25, (uint32_t)120 << 16);
  c->mem_w16(0x801003F8, 350);          // DAT_801003f8 = projection plane H (eng_init_camera reads this)
  // SetGeomScreen(350) (FUN_800846f0): projection plane distance H.
  gte_write_ctrl(26, 350);
  rec_dispatch(c, 0x80050738u);         // FUN_80050738: draw/disp env structs (still PSX; native next)
}

// FUN_80050a80 — engine CAMERA init: identity camera-rotation matrix at scratchpad 0x1F8000F8 (the same
// camera matrix the renderer reads as SCR+0xF8) and a second matrix at 0x1F800118, then the camera
// position/state fields. _DAT_1f8000ec = 0x1000 (1.0 scale); _DAT_1f8000ee = H*-5 and _DAT_1f8000d8 =
// H*-0x50000 where H = DAT_801003f8 (the projection plane, signed halfword = 350, set by FUN_800509b4).
void eng_init_camera(Core* c) {
  eng_identity_matrix(c, 0x1F8000F8);
  eng_identity_matrix(c, 0x1F800118);
  int32_t h = (int16_t)c->mem_r16(0x801003F8);   // projection plane H (set earlier; signed lh)
  c->mem_w32(0x1F8000DC, 0); c->mem_w32(0x1F8000E0, 0); c->mem_w32(0x1F8000E4, 0);
  c->mem_w32(0x1F8000D0, 0); c->mem_w32(0x1F8000D4, 0);
  c->mem_w16(0x1F8000E8, 0); c->mem_w16(0x1F8000EA, 0);
  c->mem_w16(0x1F8000EC, 0x1000);
  c->mem_w16(0x1F8000F0, 0); c->mem_w16(0x1F8000F2, 0); c->mem_w16(0x1F8000F4, 0);
  c->mem_w16(0x1F8000EE, (uint16_t)(h * -5));
  c->mem_w32(0x1F8000D8, (uint32_t)(h * -0x50000));
}
