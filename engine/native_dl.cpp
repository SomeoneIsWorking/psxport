// Native classified display list — per-frame arena + node-keyed lookup. See native_dl.h.
// De-globalization (2026-06-19): the arena state lives on Game as NdlState (native_dl.h); the public
// ndl_* API takes Core* and forwards to core->game->ndl. ndl_active() stays a free config-cache.
#include "native_dl.h"
#include "game.h"
#include "cfg.h"
#include <string.h>
#include <stdio.h>

static int s_active = -1;   // config-cache (PSXPORT_DL_GUESTPKT), identical across cores -> stays shared
int ndl_active(void) {
  if (s_active < 0) s_active = cfg_on("PSXPORT_DL_GUESTPKT") ? 0 : 1;
  return s_active;
}

void NdlState::reset() {
  s_n = 0;
  memset(s_banchor, 0, sizeof s_banchor);
  s_consumed = 0;
}

// open-addressing probe for bucket-anchor `otaddr` (never 0 — otaddr = ot_base + idx*4, ot_base high).
uint32_t NdlState::slot(uint32_t otaddr) {
  uint32_t h = (otaddr >> 2) & NDL_MASK;
  while (s_banchor[h] && s_banchor[h] != otaddr) h = (h + 1) & NDL_MASK;
  return h;
}

NativePrim* NdlState::alloc(uint32_t otaddr) {
  if (s_consumed) reset();                       // first prim after a draw -> new frame
  if (s_n >= NDL_MAX) {
    static int warned = 0;
    if (!warned++) fprintf(stderr, "[ndl] WARN: display-list arena full (%d) — dropping prims\n", NDL_MAX);
    return 0;
  }
  uint32_t h = slot(otaddr);
  if (!s_banchor[h]) { s_banchor[h] = otaddr; s_bhead[h] = -1; }
  NativePrim* p = &s_prim[s_n];
  p->node  = otaddr;
  p->bnext = s_bhead[h];                          // prepend (LIFO: head renders first = guest AddPrim order)
  s_bhead[h] = s_n;
  s_n++;
  return p;
}

NativePrim* NdlState::lookup(uint32_t addr) {
  if (!s_n) return 0;
  uint32_t h = slot(addr);
  return s_banchor[h] ? &s_prim[s_bhead[h]] : 0;
}

NativePrim* NdlState::next(const NativePrim* p) {
  return (p && p->bnext >= 0) ? &s_prim[p->bnext] : 0;
}

void NdlState::mark_consumed() { if (s_n) s_consumed = 1; }

// ---- Public API: thin free-function wrappers forwarding to the per-instance NdlState (core->game->ndl) ----
NativePrim* ndl_alloc(Core* core, uint32_t otaddr)      { return core->game->ndl.alloc(otaddr); }
NativePrim* ndl_lookup(Core* core, uint32_t addr)       { return core->game->ndl.lookup(addr); }
NativePrim* ndl_next(Core* core, const NativePrim* p)   { return core->game->ndl.next(p); }
void        ndl_mark_consumed(Core* core)               { core->game->ndl.mark_consumed(); }
