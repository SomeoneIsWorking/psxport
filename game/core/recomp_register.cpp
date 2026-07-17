// recomp_register.cpp — the Tomba!2 game-side glue that fills the framework↔generated-substrate seam
// (runtime/recomp/recomp_iface.h) from THIS game's generated substrate symbols.
//
// The framework (runtime/recomp/*) no longer names generated/ symbols directly: it reaches the
// recompiled MAIN.EXE dispatch, the overlay table, the per-module override setters, and the guest-
// memset gen body through psxport_recomp()->field. This file is the ONE place those generated symbols
// are named — game code referring to generated code, which is allowed (the coupling the seam breaks is
// framework→generated, not game→generated). Installed once at startup by tomba_install_recomp(),
// called from main() (boot.cpp) alongside tomba_install_game_config(), before the first Core runs.
#include "core.h"            // Core, rec_func_index (declared extern "C" in core.h's extern-C block)
#include "recomp_iface.h"    // RecompRegistry / RecOverlay / psxport_install_recomp
#include "overlay_table.h"   // generated: main_dispatch, g_rec_overlays, g_rec_overlay_count

// The generated per-module override setters + the guest-memset gen body. Declared with the exact
// signatures the recompiler emits (compiled as C++ -> C++ linkage), so these resolve to the generated
// definitions in generated/shard_disp.c / ov_a00_disp.c / ov_game_disp.c / shard_1.c.
extern void shard_set_override   (uint32_t, void(*)(Core*));   // generated/shard_disp.c    (MAIN)
extern void ov_a00_set_override  (uint32_t, void(*)(Core*));   // generated/ov_a00_disp.c   (A00 overlay)
extern void ov_game_set_override (uint32_t, void(*)(Core*));   // generated/ov_game_disp.c  (GAME overlay)
extern void gen_func_8009A420    (Core*);                      // generated/shard_1.c       (guest memset)

static const RecompRegistry g_tomba_recomp = {
  /* main_dispatch        */ main_dispatch,
  /* rec_func_index       */ rec_func_index,
  /* overlays             */ g_rec_overlays,
  /* overlay_count        */ g_rec_overlay_count,
  /* shard_set_override   */ shard_set_override,
  /* ov_a00_set_override  */ ov_a00_set_override,
  /* ov_game_set_override */ ov_game_set_override,
  /* guestMemset_gen      */ gen_func_8009A420,
};

void tomba_install_recomp() { psxport_install_recomp(&g_tomba_recomp); }
