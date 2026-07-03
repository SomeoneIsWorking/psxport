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
#include "sbs.h"    // class Sbs — coreId/frame() for diag tagging when running under the SBS harness

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
  Sbs*     sbs = c->game->sbs;
  int      cid = (dbg && sbs) ? sbs->coreId(c) : -1;
  uint32_t fr  = (dbg && sbs) ? sbs->frame()   :  0;
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
  const unsigned char* ram = c->ram + (base & 0x1FFFFFFF);
  int s = slot_index(base);
  const RecOverlay* cached = (s >= 0) ? c->game->sched.resident_ov[s] : 0;
  // Trust the load-time IDENTITY only while the live RAM still matches its signature. The cache becomes
  // STALE when the slot is reloaded with a different overlay by a path that bypasses overlay_note_load,
  // or when a noted load was a transient preload later overwritten by another overlay. A 32-byte memcmp
  // against the cached overlay's signature catches that; on a mismatch we re-scan for the overlay whose
  // image is ACTUALLY resident now and refresh the cache. (Root cause of the 0x8010BF54 recomp-MISS,
  // later-275: at GAME setup A00 was noted into the MODE slot, then SOP was reloaded there for the
  // never-dismissed intro narration; the cache still said A00, so the SOP narration renderer 0x8010BF54
  // mis-routed to A00's switch — which has no function entry there, only a mid-function label — and the
  // dispatch fell to a fail-fast miss. The live RAM at the base matched sig_sop exactly the whole time.)
  if (cached && cached->siglen <= 32 && memcmp(cached->sig, ram, cached->siglen) == 0)
    return cached;
  // Signature scan: find the overlay whose image is in the slot right now (the overlays overlap at a base,
  // so they are distinguished only by content). On a hit, refresh the per-core load-time identity so the
  // common path (cached match above) stays a single memcmp.
  for (int i = 0; i < g_rec_overlay_count; i++) {
    const RecOverlay* o = &g_rec_overlays[i];
    if (o->base != base || o->siglen > 32)
      continue;
    if (memcmp(o->sig, ram, o->siglen) == 0) {
      if (s >= 0) c->game->sched.resident_ov[s] = o;
      return o;
    }
  }
  // No signature matched (e.g. the resident overlay mutated its own header pointer table after load). The
  // load-time identity is the best remaining guess for the truly-resident overlay; fall back to it.
  return cached;
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

// Is `addr` a real function ENTRY in whatever recompiled module owns it right now (MAIN, or the overlay
// currently resident in addr's slot)? Used by the native render walk to REFUSE executing a node whose
// render-fn is a DANGLING pointer into an overlay that has since been evicted — e.g. a SOP intro-narration
// render node that survives into the A00 field: its renderer 0x8010BF54 is a valid SOP entry but, once A00
// is resident in the MODE slot, that address is mid-function in A00 (no entry) and dispatching it would
// fail-fast. The engine owns its render visibility, so it skips such stale content rather than run it.
// (later-275.) Returns 0 only when addr falls in a recompiled module range but is not an entry there;
// addresses outside every recompiled range return 1 (let rec_dispatch route them — native/HLE leaves).
extern "C" int rec_func_index(uint32_t);
extern "C" void guest_backtrace_to(Core*, FILE*);   // sync_overrides.cpp — heuristic guest stack walk
int rec_addr_has_entry(Core* c, uint32_t addr) {
  uint32_t a = addr & 0x1FFFFFFF;
  if (a >= REC_MAIN_LO && a < REC_MAIN_HI) return rec_func_index(addr) >= 0;
  for (int i = 0; i < g_rec_overlay_count; i++) {
    const RecOverlay* o = &g_rec_overlays[i];
    if (a < (o->base & 0x1FFFFFFF) || a >= (o->end & 0x1FFFFFFF)) continue;
    const RecOverlay* res = resident_overlay(c, o->base);
    return res && res->idx && res->idx(addr) >= 0;
  }
  return 1;
}

void interp_run(Core* c, uint32_t addr);   // interp.cpp — pure-interpreter engine (oracle Core)

#include <map>
#include <vector>
#include <algorithm>
#include <cstdlib>
// recdep (later-286): RECOMP-DEPENDENCY meter — histogram every substrate function rec_dispatch routes to
// (the native port; the oracle interp path is excluded), dumped top-40 at exit. The metric for the
// "minimize recomp" goal: rank which substrate fns to own natively next. Gated on cfg_dbg("recdep").
static std::map<uint32_t,uint64_t> s_recdep;
extern "C" void recdep_dump() {
  if (!cfg_dbg("recdep")) return;
  std::vector<std::pair<uint64_t,uint32_t>> v;
  for (auto& kv : s_recdep) v.push_back({kv.second, kv.first});
  std::sort(v.rbegin(), v.rend());
  fprintf(stderr, "[recdep] top substrate dispatch targets (addr: calls), %zu unique:\n", v.size());
  for (size_t i = 0; i < v.size() && i < 40; i++)
    fprintf(stderr, "  0x%08X : %llu\n", v[i].second, (unsigned long long)v[i].first);
}
void rec_dispatch(Core* c, uint32_t addr) {
  // ORACLE Core (later-278): interpret the target instead of routing to a recompiled body. The
  // interpreter handles overlay/non-recompiled code natively (no fail-fast miss), which is exactly why
  // the oracle uses it. The native port Core (use_interp==0) takes the substrate route below.
  if (c->use_interp) { interp_run(c, addr); return; }
  if (cfg_dbg("recdep")) { static int reg=0; if(!reg){reg=1;atexit(recdep_dump);} s_recdep[(addr & 0x1FFFFFFF) | 0x80000000]++; }
  // Attack (a) probe: attribute rec_dispatch calls to specific overlay handlers. Env=hex address, e.g.
  // PSXPORT_DISPWATCH=0x8013B2E4. Prints per-core when reached; distinguishes "B never dispatches this
  // handler" (real gap) from "B dispatches but the resident overlay isn't A00" (loader gap).
  static uint32_t s_dw = (uint32_t)-1, s_dw_ra = 0;
  if (s_dw == (uint32_t)-1) {
    const char* e = getenv("PSXPORT_DISPWATCH");
    s_dw = 0;
    if (e && *e) {
      // Accept "0xADDR" or "0xADDR:ra=0xRA" — the :ra=... suffix filters by the CALLER's ra
      // (c->r[31] at dispatch entry), so a target function like FUN_80083F50 with 30+ call sites
      // can be filtered to just the one from a specific caller PC. Extension per workflow-first:
      // an origin-revealing DISPWATCH lets a propagation-shape target-#4-style investigation
      // name the caller's arg without hand-grepping the recomp for every rec_dispatch site.
      s_dw = (uint32_t)strtoul(e, nullptr, 0);
      const char* rk = strstr(e, ":ra=");
      if (rk) s_dw_ra = (uint32_t)strtoul(rk + 4, nullptr, 0);
    }
  }
  if (s_dw && (addr & 0x1FFFFFFF) == (s_dw & 0x1FFFFFFF)
           && (!s_dw_ra || (c->r[31] & 0x1FFFFFFF) == (s_dw_ra & 0x1FFFFFFF))) {
    Sbs* sbs = c->game ? c->game->sbs : 0;
    int cid = sbs ? sbs->coreId(c) : -1;
    uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6];
    bool a0_addr = (a0 & 0x1FFFFFFF) < 0x200000 && a0 != 0;
    uint32_t s0 = a0_addr ? c->mem_r8(a0 + 4) : 0xff;
    uint32_t n3 = a0_addr ? c->mem_r8(a0 + 3) : 0xff;
    // Origin-revealing line: caller ra + full a0/a1/a2 (raw + as-signed-int16 for LUT lookups where
    // small negative args are meaningful). Frame number when sbs is available.
    uint32_t frame = sbs ? sbs->frame() : 0;
    fprintf(stderr, "[dispwatch] f%u core=%c addr=%08X ra=%08X a0=%08X (s16=%d) a1=%08X a2=%08X node.s0=%u.n3=%u stage=%08X\n",
            frame, cid < 0 ? '?' : (cid ? 'B' : 'A'), addr, c->r[31], a0, (int)(int16_t)a0, a1, a2, s0, n3, c->mem_r32(0x801fe00c));
    // Also dump the guest stack so the CALLER CHAIN — not just the immediate ra — is visible. Cheap
    // when the hit is rare (dispwatch is targeted) and doing it by hand each time is what led to the
    // "no call stack" workflow gap (docs/known-bugs.md GAP-1). Pipe stderr through tools/symres.py to
    // resolve addresses to FUN_/native names automatically.
    guest_backtrace_to(c, stderr);
  }
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
