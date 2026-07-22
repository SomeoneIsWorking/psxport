// class RenderSubstrate — the game-agnostic, per-Core RENDER SUBSTRATE.
//
// These 9 members are HOST-ONLY render-substrate state (compare-mode toggles, per-object walk-scope
// diag tags, packet-pool span/attribution trackers, the dual-view snapshot buffers, per-frame render
// counters, the native-depth / PGXP subpixel caches, and the per-frame projection constants). None of
// them hold guest memory, and none carry a ctor back-pointer to Core — they bind lazily (bind()/
// sCurrent) or are pure value state. They used to live on the game-side `class Render` umbrella
// (game/render/render.h), which forced the framework (runtime/recomp/) to include that whole game
// header just to reach a projection cache or a compare-mode toggle. Splitting them out onto a
// framework-owned value member (`Core::rsub`) lets the substrate reach them without pulling in the
// game render umbrella. Byte-neutral: SBS compares guest RAM / scratchpad / GTE, never C++ layout.
#pragma once
#include "render_mode.h"
#include "render_diag.h"
#include "ot_attr.h"
#include "dualview_snapshot.h"
#include "render_stats.h"
#include "proj_prim.h"
#include "pgxp.h"
#include "proj_params.h"
class Core;

class RenderSubstrate {
public:
  RenderMode        mode;              // compare-mode toggles (psxRender / dualview)
  RenderDiag        diag;              // per-object walk-scope tags (currentNode, currentGeomblk)
  OtAttr            otAttr;            // OT/GTE submission attribution (`debug otattr`; ot_attr.h)
  DualviewSnapshot  dualviewSnapshot;  // dual-view render harness's per-Core RAM+scratchpad+GTE snapshots
  RenderStats       stats;             // per-frame render diag counters (ndepth / projprim)
  ProjPrim          projprim;          // vertex-depth cache for native depth path (per-Core; SBS-safe)
  Pgxp              pgxp;               // PGXP-lite subpixel cache (per-Core; PGXP_pushSXYZ2f target)
  ProjParams        projParams;        // camview + per-frame projection constants (per-Core)
};
