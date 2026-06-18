// Native classified display list — per-frame arena + node-keyed lookup. See native_dl.h.
#include "native_dl.h"
#include "cfg.h"
#include <string.h>
#include <stdio.h>

// Field main OT is ~514 polys + overlay emitters; size generously so the arena never drops geometry.
#define NDL_MAX   32768
#define NDL_HASH  65536           // power-of-two open-addressing table (load factor < 0.5)
#define NDL_MASK  (NDL_HASH - 1)

static NativePrim s_prim[NDL_MAX];
static uint32_t   s_banchor[NDL_HASH];  // bucket-anchor otaddr (phys), 0 = empty slot
static int32_t    s_bhead[NDL_HASH];    // head prim index for that anchor (LIFO chain via NativePrim.bnext)
static int        s_n = 0;
static int        s_consumed = 1;       // start consumed so the first alloc begins a clean frame

static int s_active = -1;
int ndl_active(void) {
  if (s_active < 0) s_active = cfg_on("PSXPORT_DL_GUESTPKT") ? 0 : 1;
  return s_active;
}

static void ndl_reset(void) {
  s_n = 0;
  memset(s_banchor, 0, sizeof s_banchor);
  s_consumed = 0;
}

// open-addressing probe for bucket-anchor `otaddr` (never 0 — otaddr = ot_base + idx*4, ot_base high).
static uint32_t ndl_slot(uint32_t otaddr) {
  uint32_t h = (otaddr >> 2) & NDL_MASK;
  while (s_banchor[h] && s_banchor[h] != otaddr) h = (h + 1) & NDL_MASK;
  return h;
}

NativePrim* ndl_alloc(uint32_t otaddr) {
  if (s_consumed) ndl_reset();                  // first prim after a draw -> new frame
  if (s_n >= NDL_MAX) {
    static int warned = 0;
    if (!warned++) fprintf(stderr, "[ndl] WARN: display-list arena full (%d) — dropping prims\n", NDL_MAX);
    return 0;
  }
  uint32_t h = ndl_slot(otaddr);
  if (!s_banchor[h]) { s_banchor[h] = otaddr; s_bhead[h] = -1; }
  NativePrim* p = &s_prim[s_n];
  p->node  = otaddr;
  p->bnext = s_bhead[h];                         // prepend (LIFO: head renders first = guest AddPrim order)
  s_bhead[h] = s_n;
  s_n++;
  return p;
}

NativePrim* ndl_lookup(uint32_t addr) {
  if (!s_n) return 0;
  uint32_t h = ndl_slot(addr);
  return s_banchor[h] ? &s_prim[s_bhead[h]] : 0;
}

NativePrim* ndl_next(const NativePrim* p) {
  return (p && p->bnext >= 0) ? &s_prim[p->bnext] : 0;
}

void ndl_mark_consumed(void) { if (s_n) s_consumed = 1; }
