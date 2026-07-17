// recomp_iface.h — THE framework↔generated-substrate seam.
//
// The PSX-generic framework in runtime/recomp/ must reach the GENERATED recompiled game (the
// generated/ shards: main_dispatch, rec_func_index, the overlay table, the per-module override
// setters, and a couple of specific gen bodies) WITHOUT naming those generated symbols directly —
// exactly as game_iface.h decouples the framework from the game's C++ classes. The game fills one
// RecompRegistry with pointers into its generated substrate and installs it at startup; every
// framework call site reaches the substrate through psxport_recomp()->field.
//
// This is the LINK-level half of the split: after conversion NO framework .cpp carries an undefined
// reference to main_dispatch / rec_func_index / g_rec_overlays / g_rec_overlay_count /
// shard_set_override / ov_*_set_override / gen_func_8009A420 — the standalone libpsxport.a archive
// stops depending on generated/ symbols (proved by tools/smoke/psxport_smoke.cpp).
#pragma once
#include <stdint.h>

class Core;   // runtime/recomp/core.h

// A native-override handler / gen body: guest ABI, args in c->r[4..7], return in c->r[2].
// (Identical to override_registry.h's OverrideFn and platform_hle.h's; a local alias avoids an
// include cycle — the generated overlay_table.h includes THIS header.)
typedef void (*RecOverrideFn)(Core*);

// RecOverlay — descriptor of one recompiled OVERLAY module (MODE-slot field code, GAME/DEMO/... ).
// DEFINED HERE (framework-owned) rather than in the generated overlay_table.h, so the framework owns
// the type it routes through; the generated overlay_table.h now #includes this header instead of
// redefining the struct. The overlay TABLE itself (g_rec_overlays / g_rec_overlay_count) stays
// generated and is reached through RecompRegistry::overlays below.
struct RecOverlay {
  uint32_t base, end;             // guest address range this overlay occupies when resident
  const char* name;              // overlay file stem (DEMO/GAME/...), for diagnostics
  void (*disp)(Core*, uint32_t); // this overlay's address->fn switch
  int  (*idx)(uint32_t);         // addr -> function-entry index, or -1 if addr is not an entry
  const unsigned char* sig;      // first 32 bytes of the overlay image (resident-ID signature)
  unsigned siglen;
};

// RecompRegistry — the game's generated substrate, presented to the framework as function/table
// pointers. Filled by the game (game/core/recomp_register.cpp) from the generated symbols and
// installed once at startup, before any Core runs a frame.
struct RecompRegistry {
  void (*main_dispatch)(Core* c, uint32_t addr);   // generated: MAIN.EXE addr->fn switch
  int  (*rec_func_index)(uint32_t addr);           // generated: MAIN addr -> entry index, -1 if none
  const RecOverlay* overlays;                       // generated: g_rec_overlays[]
  int  overlay_count;                               // generated: g_rec_overlay_count
  void (*shard_set_override)(uint32_t, RecOverrideFn);      // generated MAIN module override setter
  void (*ov_a00_set_override)(uint32_t, RecOverrideFn);     // generated A00-overlay override setter
  void (*ov_game_set_override)(uint32_t, RecOverrideFn);    // generated GAME-overlay override setter
  void (*guestMemset_gen)(Core*);                   // generated gen_func_8009A420 (guest memset body)
};

// Install once at startup (before the first frame). Process-global; both SBS cores share it. Returns
// nullptr until installed (harmless: nothing dispatches a frame before the game installs it).
void                  psxport_install_recomp(const RecompRegistry*);
const RecompRegistry* psxport_recomp();
