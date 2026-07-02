#ifndef PROJPRIM_STATE_H
#define PROJPRIM_STATE_H
// ProjPrimState — per-instance vertex-depth cache used by the native depth path.
// engine_submit writes each vertex's view-space Z keyed by the packet vertex word's GUEST ADDRESS
// (projprim_set_pz); the renderer's gp0_exec looks up the depth at each read address
// (projprim_lookup_pz). This was a file-scope cache in gte_beetle.cpp — process-wide, so the two
// SBS cores' submits + lookups clobbered each other (A pane rendered from B's stale depths, and
// vice versa; deglobalized 2026-07-03 to unbreak SBS gameplay mode).
#include <stdint.h>
#define PROJPRIM_PP_MAX  65536
#define PROJPRIM_PP_HASH 16384
struct ProjPrimEnt { uint32_t addr; float pz; int next; };
struct ProjPrimState {
  ProjPrimEnt entries[PROJPRIM_PP_MAX];
  int         head[PROJPRIM_PP_HASH];
  int         n = 0, inited = 0, overflow = 0;
  long        set_ct = 0, hit_ct = 0, miss_ct = 0;
};
#endif
