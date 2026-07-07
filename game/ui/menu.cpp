// game/ui/menu.cpp — PC-native in-game OPTIONS MENU subsystem.
// Replaces the game's in-game Options submenu controller (FUN_8007B45C, the Messages / Sound / Screen
// adjust / Controls page) with our PC-native (RmlUi) overlay: while the pause menu's page-3 handler
// runs, show OUR overlay (g_mods toggles) and own the same back-navigation + menu SFX. Falls back to
// the real menu (super-call) when the overlay isn't up (headless / window-less). Extracted verbatim
// from game_tomba2.cpp (one behavior, byte-identical) into its own module for PC-game code structure.
#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include "menu.h"
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

// ---- Replace the game's in-game Options menu with our PC-native (RmlUi) menu (later-112) ----
// RE: the in-game pause menu is a task in the GAME overlay. Its body is the dispatcher at 0x8010810C,
// which reads the page byte task+0x6B (task ptr = *(u32)0x1F800138), bounds-checks <0xC, and jumps
// through a 12-entry table at 0x801062EC. Page 1 draws the main pause menu "Options / Load data /
// Quit game" (FUN_8007eae4) and, on Cross over "Options", sets task+0x6B = 3. Page 3's handler
// (0x801082C0) calls FUN_8007b45c — the game's Options submenu controller (Messages / Sound /
// Screen adjust / Controls), the options the user deemed not worth keeping. So we REPLACE that
// controller: while page 3 runs, show OUR overlay (g_mods toggles) instead, and own the SAME
// back-navigation FUN_8007b45c uses, including its menu SFX:
//   Circle (0x2000)   -> task+0x6B = 1 (back to the pause menu); SFX FUN_80074590(0x14, 0xFFF7, 0).
//   Triangle (0x1000) -> task+0x6B = 2 (close the pause menu);   SFX FUN_80074590(0x11, 0, 0).
// Faithful fallback: if our overlay isn't actually up (headless / window-less), super-call the real
// menu so nothing is lost. The overlay is up by default for windowed runs (no flag needed).
#define T2_PAD_EDGE    0x800E7E68u  // DAT_800e7e68 — this-frame pressed-button edges (u16, active-high)
#define T2_TASK_PTR    0x1F800138u  // _DAT_1f800138 — current task struct pointer (scratchpad word)
#define T2_MENU_CURSOR 0x800BF808u  // DAT_800bf808 — shared menu cursor byte
#define T2_MENU_DIRTY  0x1F800136u  // DAT_1f800136 — "menu changed" flag the pause task watches
#define T2_SFX_FN      0x80074590u  // FUN_80074590 — the menu sound-effect trigger
#define PAD_TRIANGLE   0x1000u
#define PAD_CIRCLE     0x2000u

// Invoke a guest function with up to 3 args, preserving the override's a0-a2.
static void t2_call3(Core* c, uint32_t addr, uint32_t a0, uint32_t a1, uint32_t a2) {
  uint32_t s4 = c->r[4], s5 = c->r[5], s6 = c->r[6];
  c->r[4] = a0; c->r[5] = a1; c->r[6] = a2;
  rec_dispatch(c, addr);
  c->r[4] = s4; c->r[5] = s5; c->r[6] = s6;
}
