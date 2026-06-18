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
static int32_t    s_hash[NDL_HASH];   // node>>2 bucket -> prim index + 1 (0 = empty)
static int        s_n = 0;
static int        s_consumed = 1;     // start consumed so the first alloc begins a clean frame

static int s_active = -1;
int ndl_active(void) {
  if (s_active < 0) s_active = cfg_on("PSXPORT_DL_GUESTPKT") ? 0 : 1;
  return s_active;
}

static void ndl_reset(void) {
  s_n = 0;
  memset(s_hash, 0, sizeof s_hash);
  s_consumed = 0;
}

NativePrim* ndl_alloc(uint32_t node) {
  if (s_consumed) ndl_reset();                  // first prim after a draw -> new frame
  if (s_n >= NDL_MAX) {
    static int warned = 0;
    if (!warned++) fprintf(stderr, "[ndl] WARN: display-list arena full (%d) — dropping prims\n", NDL_MAX);
    return 0;
  }
  NativePrim* p = &s_prim[s_n];
  p->node = node;
  uint32_t h = (node >> 2) & NDL_MASK;
  while (s_hash[h]) h = (h + 1) & NDL_MASK;      // open addressing; load factor guarantees a free slot
  s_hash[h] = s_n + 1;
  s_n++;
  return p;
}

NativePrim* ndl_lookup(uint32_t node) {
  if (!s_n) return 0;
  uint32_t h = (node >> 2) & NDL_MASK;
  while (s_hash[h]) {
    NativePrim* p = &s_prim[s_hash[h] - 1];
    if (p->node == node) return p;
    h = (h + 1) & NDL_MASK;
  }
  return 0;
}

void ndl_mark_consumed(void) { if (s_n) s_consumed = 1; }
