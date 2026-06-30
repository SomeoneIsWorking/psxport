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
#include "game.h"              // SchedulerState::resident_ov (per-core resident-overlay-by-slot map)
#include "overlay_table.h"     // generated: REC_MAIN_LO/HI, main_dispatch, g_rec_overlays[]
#include "cfg.h"               // cfg_dbg("ovload") — per-core MODE-slot overlay residency trace
#include <string.h>
#include <stdio.h>

void rec_dispatch_miss(Core* c, uint32_t addr);
extern "C" int sbs_core_id(Core*) __attribute__((weak));
extern "C" uint32_t sbs_frame_num() __attribute__((weak));

// Overlap-slot bases (the addresses where mutually-exclusive overlays load) -> a 0..2 index into the
// per-core resident_ov[] map. Kept in sync with emit.py OVERLAY_BASES / overlay_base().
static int slot_index(uint32_t base) {
  switch (base & 0x1FFFFFFF) {
    case 0x00106228: return 0;   // STAGE slot (START/DEMO/GAME)
    case 0x00108F9C: return 1;   // MODE slot  (SOP / A0* field area code)
    case 0x0018A000: return 2;   // AREA slot  (OPN; also raw area DATA — no code overlay)
    default: return -1;
  }
}

// Called by the overlay LOADERS right after an image is written to `dest`. If dest is a slot base,
// identify the just-loaded overlay by its raw-.BIN signature — which matches NOW, before the game
// mutates its header pointer table — and record it per-core. The router then routes by identity, robust
// to later header mutation. A non-overlay blob (raw area DATA to the AREA slot) matches nothing -> clear,
// and the router falls back to a live signature scan (which also won't match data == correct miss).
void overlay_note_load(Core* c, uint32_t dest) {
  int s = slot_index(dest);
  if (s < 0) return;
  const unsigned char* ram = c->ram + (dest & 0x1FFFFFFF);
  int dbg = cfg_dbg("ovload");
  int cid = (dbg && sbs_core_id) ? sbs_core_id(c) : -1;
  uint32_t fr = (dbg && sbs_frame_num) ? sbs_frame_num() : 0;
  for (int i = 0; i < g_rec_overlay_count; i++) {
    const RecOverlay* o = &g_rec_overlays[i];
    if ((o->base & 0x1FFFFFFF) != (dest & 0x1FFFFFFF)) continue;
    if (memcmp(o->sig, ram, o->siglen) == 0) {
      c->game->sched.resident_ov[s] = o;
      if (dbg) fprintf(stderr, "[ovload] core %c slot %d <- %s (frame %u)\n",
                       cid < 0 ? '?' : cid ? 'B' : 'A', s, o->name, fr);
      return;
    }
  }
  c->game->sched.resident_ov[s] = 0;   // unknown content in this slot -> fall back to signature scan
  if (dbg) fprintf(stderr, "[ovload] core %c slot %d <- (none/unmatched, dest=0x%08X, frame %u)\n",
                   cid < 0 ? '?' : cid ? 'B' : 'A', s, dest, fr);
}

// Which overlay currently occupies `base`? The overlays overlap, so we identify the resident one by
// comparing its image signature (first bytes of the .BIN, baked into the table at emit time) against
// guest RAM at the base — set there by the synchronous overlay loader. Cached on a content match so
// the hot path (per-frame overlay dispatch) is a single memcmp; the cache self-corrects when the
// resident overlay (or the running core's RAM) changes.
static const RecOverlay* resident_overlay(Core* c, uint32_t base) {
  // Prefer the IDENTITY recorded at load time (robust to runtime header mutation). Fall back to the
  // content-signature scan only when no load was noted for this slot (an overlay loaded by a path that
  // didn't route through overlay_note_load).
  int s = slot_index(base);
  if (s >= 0 && c->game->sched.resident_ov[s]) return c->game->sched.resident_ov[s];
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

// Diagnostic for the miss path (hle.cpp): for a slot-range address, report which overlay is currently
// resident in that slot (the one rec_dispatch routed to before its switch fell to the miss default).
// Returns the overlay name, or "none" (slot empty / unmatched), or 0 if addr is not in any slot range.
const char* overlay_router_resident_name(Core* c, uint32_t addr) {
  uint32_t a = addr & 0x1FFFFFFF;
  for (int i = 0; i < g_rec_overlay_count; i++) {
    const RecOverlay* o = &g_rec_overlays[i];
    if (a >= (o->base & 0x1FFFFFFF) && a < (o->end & 0x1FFFFFFF)) {
      const RecOverlay* res = resident_overlay(c, o->base);
      return res ? res->name : "none";
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
      // overlay slot but NO resident overlay signature matches -> fail fast. Dump what IS resident so a
      // miss-loop can see whether the slot holds an unexpected overlay or a relocated/clobbered image.
      const unsigned char* ram = c->ram + (o->base & 0x1FFFFFFF);
      fprintf(stderr, "[overlay-router] addr 0x%08X in slot 0x%08X but NO resident overlay matches.\n"
                      "  resident[0..16] =", addr, o->base);
      for (int k = 0; k < 16; k++) fprintf(stderr, " %02X", ram[k]);
      fprintf(stderr, "\n");
      for (int j = 0; j < g_rec_overlay_count; j++) {
        const RecOverlay* q = &g_rec_overlays[j];
        if (q->base != o->base) continue;
        int nmatch = 0; for (unsigned k = 0; k < q->siglen && k < 16; k++) nmatch += (q->sig[k] == ram[k]);
        fprintf(stderr, "  cand %-6s sig[0..16] =", q->name);
        for (unsigned k = 0; k < 16 && k < q->siglen; k++) fprintf(stderr, " %02X", q->sig[k]);
        fprintf(stderr, "   (%d/16 match)\n", nmatch);
      }
      break;   // fail fast below
    }
  }
  rec_dispatch_miss(c, addr);
}
