// class Collision — PC-native COLLISION-GRID subsystem owned by Engine.
//
// PROPER OOP: one instance per Core, reached as `eng(c).collision.method(obj, …)`. Back-pointer
// `core` wired once at Core construction time (same pattern as Animation / Placement / Asset).
//
// SCOPE: the collision-grid family that resolves an object's position against the level's spatial
// grid — the list-tail resolver (FUN_80031780), the grid row-pointer setup (FUN_80049968), the cell
// query / neighbor-walk (FUN_80047CBC), the resolve loop (FUN_800498C8), and the per-step
// origin/index setup (FUN_8004798C). Pure control flow over scratchpad + object/grid memory; no GTE,
// no render packets. Diagnostic A/B gates (listscan / gridsetup / gridquery / gridresolve / gridstep)
// are REPL channels, unchanged.
//
// Was the five free functions ov_list_scan_31780 / ov_grid_setup_49968 / ov_grid_query_47cbc /
// ov_grid_resolve_498c8 / ov_grid_step_4798c, taking their MIPS a0 argument via c->r[4] taxi. Now
// real instance methods with explicit typed uint32_t arguments; gridQuery/gridResolve return the
// resolved value directly instead of via c->r[2].
#pragma once
#include <stdint.h>
struct Core;

class Collision {
public:
  Core* core = nullptr;

  // listScan(obj): FUN_80031780 — list-tail resolver / reset for the 8-byte-stride linked list
  //   rooted at obj+52. Walks entries until a terminator tag (bit30|bit31) is hit; either clears
  //   the list (bit30 set) or updates the tail pointer at obj+56.
  void listScan(uint32_t obj);

  // gridSetup(layer): FUN_80049968 — collision-grid row-pointer setup. `layer` is masked to a
  //   uint8 grid/layer index; writes 5 scratchpad row pointers (0x1F8001CC..DC).
  void gridSetup(uint32_t layer);

  // gridQuery(): FUN_80047CBC — collision-grid cell query / neighbor-walk. Reads scratchpad probe
  //   coords and walks cells; returns 0 (off-grid/blocked) or 1 (resolved).
  int  gridQuery();

  // gridResolve(obj): FUN_800498C8 — resolve loop pairing step + setup + query for a probe object.
  //   Returns 0 (query blocked / off-grid) or 1 (resolved / terminal cell reached).
  int  gridResolve(uint32_t obj);

  // gridStep(obj): FUN_8004798C — per-step grid-origin/index setup. Reloads the grid for the
  //   probe object's recorded id if needed, then clamps + recomputes probe coords.
  void gridStep(uint32_t obj);

  // --- field-collision leaf cluster (installed by address via registerOverrides) ---

  // lineCross(flag): FUN_80045810 — per-line WALL intersection; writes crossing GR_CROSS (0x1A4).
  void lineCross(uint32_t flag);

  // floorPick(): FUN_80048034 — find the lowest floor line above the probe; writes GR_BEST_LINE.
  void floorPick();

  // slopeLocalB(): FUN_80048134 — slope-local delta (variant B) folded through map orientation.
  void slopeLocalB();

  // slopeLocalAdvance(): FUN_80048360 — same orientation fold, then advance the probe by the step.
  void slopeLocalAdvance();

  // flatNormal(obj): FUN_80049760 — GR_NORMAL_ANGLE = ratan2(segment endpoints); stores
  //   rcos/rsin>>4 into obj+0x48/+0x4C. Ready-FRAME leaf (mirrors the 32-byte guest stack frame).
  void flatNormal(uint32_t obj);

  // registerOverrides(): install the five field-collision leaves by guest address into the ONE
  //   override registry (overrides::install), so every caller — substrate included — reaches native.
  void registerOverrides();
};
