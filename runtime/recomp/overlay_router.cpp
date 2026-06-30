// Global cross-module dispatch router for the recomp substrate (the overlay model, later-257).
//
// The static recompiler emits ONE module per image whose code shares guest addresses: MAIN.EXE
// (main_dispatch) and each OVERLAPPING stage overlay \BIN\*.BIN (ov_<tag>_dispatch — they all load
// to the SAME base 0x80106228, so a given address is different code per resident overlay). Every
// recompiled body calls THIS rec_dispatch for any target outside its own function set; we range-route
// the address to the correct module's switch:
//   - MAIN text range            -> main_dispatch (generated)
//   - an overlay slot range      -> the CURRENTLY RESIDENT overlay's switch, identified by matching a
//                                   content signature against guest RAM at the slot base
//   - anything else (BIOS A0/B0/C0 vectors, platform-HLE leaves, a genuine miss) -> rec_dispatch_miss
//
// This replaces the old generated `rec_dispatch` (which was just MAIN's switch and aborted on any
// overlay address). The interpreter is gone (later-254): a slot address with no matching resident
// overlay, or an address no module recompiled, still FAILS FAST in rec_dispatch_miss by design.
#include "core.h"
#include "overlay_table.h"     // generated: REC_MAIN_LO/HI, main_dispatch, g_rec_overlays[]
#include <string.h>

void rec_dispatch_miss(Core* c, uint32_t addr);

// Which overlay currently occupies `base`? The overlays overlap, so we identify the resident one by
// comparing its image signature (first bytes of the .BIN, baked into the table at emit time) against
// guest RAM at the base — set there by the synchronous overlay loader. Cached on a content match so
// the hot path (per-frame overlay dispatch) is a single memcmp; the cache self-corrects when the
// resident overlay (or the running core's RAM) changes.
static const RecOverlay* resident_overlay(Core* c, uint32_t base) {
  const unsigned char* ram = c->ram + (base & 0x1FFFFFFF);
  static uint32_t cache_base = 0;
  static const RecOverlay* cache_ov = 0;
  static unsigned char cache_sig[32];
  if (cache_ov && cache_base == base && cache_ov->siglen <= sizeof(cache_sig)
      && memcmp(cache_sig, ram, cache_ov->siglen) == 0)
    return cache_ov;
  for (int i = 0; i < g_rec_overlay_count; i++) {
    const RecOverlay* o = &g_rec_overlays[i];
    if (o->base != base || o->siglen > sizeof(cache_sig))
      continue;
    if (memcmp(o->sig, ram, o->siglen) == 0) {
      cache_base = base; cache_ov = o;
      memcpy(cache_sig, ram, o->siglen);
      return o;
    }
  }
  return 0;
}

void rec_dispatch(Core* c, uint32_t addr) {
  uint32_t a = addr & 0x1FFFFFFF;
  if (a >= REC_MAIN_LO && a < REC_MAIN_HI) { main_dispatch(c, addr); return; }
  for (int i = 0; i < g_rec_overlay_count; i++) {
    const RecOverlay* o = &g_rec_overlays[i];
    if (a >= (o->base & 0x1FFFFFFF) && a < (o->end & 0x1FFFFFFF)) {
      const RecOverlay* res = resident_overlay(c, o->base);
      if (res) { res->disp(c, addr); return; }
      break;   // overlay slot but no resident overlay matches -> fail fast below
    }
  }
  rec_dispatch_miss(c, addr);
}
