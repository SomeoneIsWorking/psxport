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
#include "overlay_router.h"
#include "core.h"
#include "game.h"              // PcScheduler::resident_ov (per-core resident-overlay-by-slot map)
#include "override_registry.h" // overrides::dispatch — native engine/game override interception
#include "recomp_iface.h"      // seam: psxport_recomp() -> main_dispatch / rec_func_index / overlay table
#include <lucent/log.h>
#include <stdio.h>
#include <stdlib.h> // abort() — the overlapping-placement fail-fast
#include <string.h>

void rec_dispatch_miss(Core *c, uint32_t addr);
#include "sbs.h" // class Sbs — coreId/frame() for diag tagging when running under the SBS harness

static const GuestProgramImage &program_image_for_routing(Core *c) {
  if (!c->guestProgramImage) {
    lucent::error("dispatch",
                  "REFUSING TO ROUTE guest code: the installed GameRuntime supplies no "
                  "GuestProgramImage, so MAIN's resident text range is unknown.");
    abort();
  }
  if (!c->guestProgramImage->residentText.valid()) {
    lucent::error("dispatch",
                  "REFUSING TO ROUTE guest code: GuestProgramImage::residentText is not a valid "
                  "half-open physical address range.");
    abort();
  }
  return *c->guestProgramImage;
}

// Overlap-slot bases (the addresses where mutually-exclusive overlays load) -> a 0..2 index into the
// per-core resident_ov[] map. Kept in sync with emit.py OVERLAY_BASES / overlay_base().
static int slot_index(Core *c, uint32_t base) {
  if (!c->cfg) {
    return -1;
  }
  for (int i = 0; i < 3; i++) {
    if ((c->cfg->overlaySlots[i].base & 0x1FFFFFFF) == (base & 0x1FFFFFFF)) {
      return i;
    }
  }
  return -1;
}

// Called by the overlay LOADERS right after an image is written to `dest`. If dest is a slot base,
// identify the just-loaded overlay by its raw-.BIN signature — which matches NOW, before the game
// mutates its header pointer table — and record it per-core. The router then routes by identity, robust
// to later header mutation. A non-overlay blob (raw area DATA to the AREA slot) matches nothing -> clear,
// and the router falls back to a live signature scan (which also won't match data == correct miss).
void overlay_note_load(Core *c, uint32_t dest) {
  int s = slot_index(c, dest);
  if (s < 0) {
    return;
  }
  const unsigned char *ram = c->ram + (dest & 0x1FFFFFFF);
  Sbs *sbs = c->game->sbs;
  int cid = sbs ? sbs->coreId(c) : -1;
  uint32_t fr = sbs ? sbs->frame() : 0;
  const RecompRegistry *R = psxport_recomp();
  for (int i = 0; i < R->overlay_count; i++) {
    const RecOverlay *o = &R->overlays[i];
    if ((o->base & 0x1FFFFFFF) != (dest & 0x1FFFFFFF)) {
      continue;
    }
    if (!o->sig || o->siglen == 0 || o->siglen > 32) {
      continue;
    }
    if (memcmp(o->sig, ram, o->siglen) == 0) {
      c->game->pcSched.resident_ov[s] = o;
      lucent::debug("ovload",
                    "core {} slot {} <- {} (frame {})",
                    cid < 0 ? '?'
                    : cid   ? 'B'
                            : 'A',
                    s,
                    o->name ? o->name : "(null)",
                    fr);
      return;
    }
  }
  c->game->pcSched.resident_ov[s] = 0; // unknown content in this slot -> fall back to signature scan
  lucent::debug("ovload",
                "core {} slot {} <- (none/unmatched, dest=0x{:08X}, frame {})",
                cid < 0 ? '?'
                : cid   ? 'B'
                        : 'A',
                s,
                dest,
                fr);
}

// Which overlay currently occupies `base`? The overlays overlap, so we identify the resident one by
// comparing its image signature (first bytes of the .BIN, baked into the table at emit time) against
// guest RAM at the base — set there by the synchronous overlay loader. Cached on a content match so
// the hot path (per-frame overlay dispatch) is a single memcmp; the cache self-corrects when the
// resident overlay (or the running core's RAM) changes.
static const RecOverlay *resident_overlay(Core *c, uint32_t base) {
  const RecompRegistry *R = psxport_recomp();
  const unsigned char *ram = c->ram + (base & 0x1FFFFFFF);
  int s = slot_index(c, base);
  const RecOverlay *cached = (s >= 0 && c->game) ? c->game->pcSched.resident_ov[s] : nullptr;
  // Trust the load-time IDENTITY only while the live RAM still matches its signature. The cache becomes
  // STALE when the slot is reloaded with a different overlay by a path that bypasses overlay_note_load,
  // or when a noted load was a transient preload later overwritten by another overlay. A 32-byte memcmp
  // against the cached overlay's signature catches that; on a mismatch we re-scan for the overlay whose
  // image is ACTUALLY resident now and refresh the cache. (Root cause of the 0x8010BF54 recomp-MISS,
  // later-275: at GAME setup A00 was noted into the MODE slot, then SOP was reloaded there for the
  // never-dismissed intro narration; the cache still said A00, so the SOP narration renderer 0x8010BF54
  // mis-routed to A00's switch — which has no function entry there, only a mid-function label — and the
  // dispatch fell to a fail-fast miss. The live RAM at the base matched sig_sop exactly the whole time.)
  if (cached && cached->sig && cached->siglen > 0 && cached->siglen <= 32 &&
      memcmp(cached->sig, ram, cached->siglen) == 0) {
    return cached;
  }
  // Signature scan: find the overlay whose image is in the slot right now (the overlays overlap at a base,
  // so they are distinguished only by content). On a hit, refresh the per-core load-time identity so the
  // common path (cached match above) stays a single memcmp.
  for (int i = 0; i < R->overlay_count; i++) {
    const RecOverlay *o = &R->overlays[i];
    if (o->base != base || !o->sig || o->siglen == 0 || o->siglen > 32) {
      continue;
    }
    if (memcmp(o->sig, ram, o->siglen) == 0) {
      if (s >= 0 && c->game) {
        c->game->pcSched.resident_ov[s] = o;
      }
      return o;
    }
  }
  // No signature matched (e.g. the resident overlay mutated its own header pointer table after load). The
  // load-time identity is the best remaining guess for the truly-resident overlay; fall back to it.
  return cached;
}

static bool fixed_overlay_contains(const RecOverlay &overlay, uint32_t physicalAddress) {
  if (overlay.end <= overlay.base) {
    return false;
  }
  const uint32_t physicalBase = overlay.base & 0x1FFFFFFFu;
  const uint32_t imageSize = overlay.end - overlay.base;
  return physicalAddress - physicalBase < imageSize;
}

FixedOverlayResolution overlay_resolve_fixed(Core *c, uint32_t addr) {
  const RecompRegistry *registry = psxport_recomp();
  const uint32_t physicalAddress = addr & 0x1FFFFFFFu;
  FixedOverlayResolution resolution;
  uint32_t narrowestImageSize = UINT32_MAX;

  for (int i = 0; i < registry->overlay_count; ++i) {
    const RecOverlay &range = registry->overlays[i];
    if (range.relocatable || !fixed_overlay_contains(range, physicalAddress)) {
      continue;
    }
    resolution.addressInOverlayRange = true;

    const RecOverlay *resident = resident_overlay(c, range.base);
    if (!resident || resident->relocatable || !fixed_overlay_contains(*resident, physicalAddress)) {
      continue;
    }
    if (resident == resolution.overlay) {
      continue;
    }

    const uint32_t imageSize = resident->end - resident->base;
    if (imageSize < narrowestImageSize) {
      resolution.overlay = resident;
      resolution.ambiguous = false;
      narrowestImageSize = imageSize;
      continue;
    }
    if (imageSize == narrowestImageSize) {
      resolution.overlay = nullptr;
      resolution.ambiguous = true;
    }
  }
  return resolution;
}

// ---- RELOCATABLE MODULES: the live placement registry (overlay_router.h) -------------------------
//
// Core::ovBase[i] is module i's live base, or 0 when it is not resident; Core::ovDelta[i] is the
// difference from its link base, which the module's own recompiled code adds to every address it
// composes. The two are written together and only here.
int overlay_live_index(Core *c, uint32_t addr) {
  if (!c->ovLive) {
    return -1;
  }
  const RecompRegistry *R = psxport_recomp();
  const uint32_t a = addr & 0x1FFFFFFFu;
  for (int i = 0; i < R->overlay_count && i < Core::kRecMaxOverlays; i++) {
    if (!c->ovBase[i]) {
      continue;
    }
    const uint32_t lo = c->ovBase[i] & 0x1FFFFFFFu;
    const uint32_t hi = lo + (R->overlays[i].end - R->overlays[i].base);
    if (a >= lo && a < hi) {
      return i;
    }
  }
  return -1;
}

int overlay_place(Core *c, const char *name, uint32_t base, uint32_t size) {
  const RecompRegistry *R = psxport_recomp();
  for (int i = 0; i < R->overlay_count && i < Core::kRecMaxOverlays; i++) {
    const RecOverlay *o = &R->overlays[i];
    if (!o->name || !name || strcmp(o->name, name) != 0) {
      continue;
    }
    if (!o->relocatable) {
      lucent::error("ovload",
                    "module '{}' was placed at 0x{:08X} at runtime, but it was recompiled "
                    "at the FIXED base 0x{:08X}. Its emitted code has that address baked "
                    "in, so running it anywhere else executes the wrong bytes.",
                    name,
                    base,
                    o->base);
      return -1;
    }
    // The invariant the base-relative design rests on: no two resident modules overlap. Checked on
    // every placement, because the alternative is one module's code silently running as another's.
    const uint32_t lo = base & 0x1FFFFFFFu, hi = lo + size;
    for (int j = 0; j < R->overlay_count && j < Core::kRecMaxOverlays; j++) {
      if (j == i || !c->ovBase[j]) {
        continue;
      }
      const uint32_t jlo = c->ovBase[j] & 0x1FFFFFFFu;
      const uint32_t jhi = jlo + (R->overlays[j].end - R->overlays[j].base);
      if (lo < jhi && jlo < hi) {
        lucent::error("ovload",
                      "\nmodule '{}' is being placed at 0x{:08X}..0x{:08X} but '{}' is "
                      "already live at 0x{:08X}..0x{:08X}.\n"
                      "  Two resident modules cannot share guest bytes: whichever loaded "
                      "last owns the code, and the other module's still-live method table "
                      "points into it. The game's own allocator cannot produce this — it "
                      "means the loader intercept named the wrong allocation as the module "
                      "body, or a body was freed without evicting it from this registry.",
                      name,
                      base,
                      base + size,
                      R->overlays[j].name,
                      c->ovBase[j],
                      c->ovBase[j] + (R->overlays[j].end - R->overlays[j].base));
        fflush(stderr);
        abort();
      }
    }
    if (!c->ovBase[i]) {
      ++c->ovLive;
    }
    c->ovBase[i] = base;
    c->ovDelta[i] = (int32_t)(base - o->base);
    lucent::debug("ovload",
                  "module '{}' live at 0x{:08X}..0x{:08X} (delta {:+#x} from link base "
                  "0x{:08X})",
                  name,
                  base,
                  base + size,
                  c->ovDelta[i],
                  o->base);
    return i;
  }
  lucent::error("ovload",
                "no recompiled module is named '{}' — a call into it will fail as a "
                "dispatch miss. Is it listed in the seed file's overlay_base_patterns / "
                "overlay_seeds?",
                name ? name : "(null)");
  return -1;
}

int overlay_evict_at(Core *c, uint32_t base) {
  const RecompRegistry *R = psxport_recomp();
  for (int i = 0; i < R->overlay_count && i < Core::kRecMaxOverlays; i++) {
    if (c->ovBase[i] != base) {
      continue;
    }
    lucent::debug("ovload", "module '{}' evicted from 0x{:08X}", R->overlays[i].name, base);
    c->ovBase[i] = 0;
    c->ovDelta[i] = 0;
    --c->ovLive;
    return i;
  }
  return -1;
}

// Diagnostic for the miss path (hle.cpp): for a slot-range address, report which overlay is currently
// resident in that slot (the one rec_dispatch routed to before its switch fell to the miss default).
// Returns the overlay name, or "none" (slot empty / unmatched), or 0 if addr is not in any slot range.
const char *overlay_router_resident_name(Core *c, uint32_t addr) {
  const RecompRegistry *R = psxport_recomp();
  const int live = overlay_live_index(c, addr);
  if (live >= 0) {
    return R->overlays[live].name;
  }
  const FixedOverlayResolution fixed = overlay_resolve_fixed(c, addr);
  if (fixed.ambiguous) {
    return "ambiguous";
  }
  if (fixed.overlay) {
    return fixed.overlay->name;
  }
  if (fixed.addressInOverlayRange) {
    return "none";
  }
  return nullptr;
}

// Is `addr` a real function ENTRY in whatever recompiled module owns it right now (MAIN, or the overlay
// currently resident in addr's slot)? Used by the native render walk to REFUSE executing a node whose
// render-fn is a DANGLING pointer into an overlay that has since been evicted — e.g. a SOP intro-narration
// render node that survives into the A00 field: its renderer 0x8010BF54 is a valid SOP entry but, once A00
// is resident in the MODE slot, that address is mid-function in A00 (no entry) and dispatching it would
// fail-fast. The engine owns its render visibility, so it skips such stale content rather than run it.
// (later-275.) Returns 0 only when addr falls in a recompiled module range but is not an entry there;
// addresses outside every recompiled range return 1 (let rec_dispatch route them — native/HLE leaves).
extern "C" void guest_backtrace_to(Core *, FILE *); // sync_overrides.cpp — heuristic guest stack walk
int rec_addr_has_entry(Core *c, uint32_t addr) {
  uint32_t a = addr & 0x1FFFFFFF;
  const RecompRegistry *R = psxport_recomp();
  const GuestProgramImage &image = program_image_for_routing(c);
  if (image.residentText.containsPhysical(a)) {
    return R->rec_func_index(addr) >= 0;
  }
  const int live = overlay_live_index(c, addr);
  if (live >= 0) {
    return R->overlays[live].idx(addr - (uint32_t)c->ovDelta[live]) >= 0;
  }
  const FixedOverlayResolution fixed = overlay_resolve_fixed(c, addr);
  if (fixed.overlay) {
    return fixed.overlay->idx && fixed.overlay->idx(addr) >= 0;
  }
  if (fixed.addressInOverlayRange) {
    return 0;
  }
  return 1;
}

void interp_run(Core *c, uint32_t addr); // interp.cpp — pure-interpreter engine (oracle Core)

#include <algorithm>
#include <cstdlib>
#include <map>
#include <vector>
// recdep (later-286): RECOMP-DEPENDENCY meter — histogram every substrate function rec_dispatch routes to
// (the native port; the oracle interp path is excluded), dumped top-40 at exit. The metric for the
// "minimize recomp" goal: rank which substrate fns to own natively next.
//
// EITHER channel arms the meter: `recdep` (top-40) or `recdep-all` (full histogram). recdep_enabled()
// exists because the two used to be independent, so `PSXPORT_DEBUG=recdep-all` alone armed nothing and
// printed nothing — and an empty histogram is indistinguishable from "the run dispatched nothing".
// A diagnostic whose failure mode is silence has to be impossible to arm halfway.
// The histogram lives on Core (c->idiag.recdep); the atexit hook needs a static Core* because
// atexit handlers take no context — documented signal/atexit exception, set once on first use.
static Core *s_recdepCore = nullptr;
static bool recdep_enabled() {
  return lucent::channel_on("recdep") || lucent::channel_on("recdep-all");
}
// The dump must be EMITTED on a channel that is actually on — lucent::debug drops a line whose channel
// is off, so hard-coding "recdep" here would have left `recdep-all` alone collecting a histogram it then
// silently discarded at exit. Same defect as the arming one, one layer down.
static const char *recdep_chan() {
  return lucent::channel_on("recdep") ? "recdep" : "recdep-all";
}
extern "C" void recdep_dump() {
  if (!recdep_enabled() || !s_recdepCore) {
    return;
  }
  std::vector<std::pair<uint64_t, uint32_t>> v;
  v.reserve(s_recdepCore->idiag.recdep.size());
  for (auto &kv : s_recdepCore->idiag.recdep) {
    v.push_back({kv.second, kv.first});
  }
  std::sort(v.rbegin(), v.rend());
  // PSXPORT_DEBUG=recdep-all dumps the FULL histogram instead of the top-40 — needed to see rare
  // (low-call-count) dispatch targets in a specific address band that the top-40 truncation hides.
  size_t cap = lucent::channel_on("recdep-all") ? v.size() : 40;
  const char *chan = recdep_chan();
  // ANNOTATE WHAT IS ALREADY OWNED. This counter sits AFTER overrides::dispatch (so registry-owned
  // addresses never reach it) but BEFORE main_dispatch -> func_X consults g_<mod>_override[], which
  // is where PlatformHle's handlers live. A PlatformHle-owned address therefore lands in this
  // histogram at full call count while running a native handler, not the recompiled body — and the
  // list is read as "what to own next". That misreads directly into wasted work: 0x800834A0
  // (gpuTimeoutArm) sat at the TOP of this histogram at 33,152 calls while PlatformHle had owned it
  // all along, and porting it would have been a double-install. Say so on the line instead.
  PlatformHle *hle = s_recdepCore->game ? &s_recdepCore->game->platform_hle : nullptr;
  lucent::debug(chan, "top substrate dispatch targets (addr: calls), {} unique:", v.size());
  for (size_t i = 0; i < v.size() && i < cap; i++) {
    const bool owned = hle && hle->lookup(v[i].second) != nullptr;
    lucent::debug(chan,
                  "  0x{:08X} : {}{}",
                  v[i].second,
                  v[i].first,
                  owned ? "   <-- ALREADY OWNED by PlatformHle (not a porting target)" : "");
  }
}
void rec_dispatch(Core *c, uint32_t addr) {
  // ORACLE Core (later-278): interpret the target instead of routing to a recompiled body. The
  // interpreter handles overlay/non-recompiled code natively (no fail-fast miss), which is exactly why
  // the oracle uses it. The native port Core (use_interp==0) takes the substrate route below.
  if (c->use_interp) {
    if (overrides::dispatchOracle(c, addr)) {
      return;
    }
    interp_run(c, addr);
    return;
  }
  // TEMP PROBE (Job 2 investigation, docs/findings/animation.md): r29-at-entry for Animation::attach
  // on both SBS cores, to determine whether the isDeadStackScratch residual is caused by an upstream
  // native caller failing to descend r29 like the substrate (vs. attach's own frame needing a mirror).
  if ((addr & 0x1FFFFFFFu) == 0x00077C40u) {
    Sbs *sbs = c->game ? c->game->sbs : nullptr;
    int cid = sbs ? sbs->coreId(c) : -1;
    lucent::debug("animstack",
                  "f{} core={} r29={:08X} ra={:08X} a0={:08X}",
                  sbs ? sbs->frame() : 0,
                  cid < 0 ? '-' : (cid ? 'B' : 'A'),
                  c->r[29],
                  c->r[31],
                  c->r[4]);
  }
  // Intercepting HERE (not only at the g_<mod>_override[] wrapper) makes an override fire even when
  // its overlay image is not resident in guest RAM — the gen bodies are always linked. dispatch()
  // owns the oracle gate: the psx_fallback / inSubstrateLeg leg runs the gen body, so SBS core B stays
  // the pure substrate reference. Hit counting + `dispatch`/`ovhit` tracing live inside the registry.
  if (overrides::dispatch(c, addr)) {
    return;
  }
  if (recdep_enabled()) {
    if (!s_recdepCore) {
      s_recdepCore = c;
      atexit(recdep_dump);
    }
    c->idiag.recdep[(addr & 0x1FFFFFFF) | 0x80000000]++;
  }
  // Attack (a) probe: attribute rec_dispatch calls to specific overlay handlers. Env=hex address, e.g.
  // PSXPORT_DISPWATCH=0x8013B2E4. Prints per-core when reached; distinguishes "B never dispatches this
  // handler" (real gap) from "B dispatches but the resident overlay isn't A00" (loader gap).
  uint32_t &s_dw = c->idiag.dispwatch;
  uint32_t &s_dw_ra = c->idiag.dispwatch_ra;
  if (s_dw == (uint32_t)-1) {
    const char *e = getenv("PSXPORT_DISPWATCH");
    s_dw = 0;
    if (e && *e) {
      // Accept "0xADDR" or "0xADDR:ra=0xRA" — the :ra=... suffix filters by the CALLER's ra
      // (c->r[31] at dispatch entry), so a target function like FUN_80083F50 with 30+ call sites
      // can be filtered to just the one from a specific caller PC. Extension per workflow-first:
      // an origin-revealing DISPWATCH lets a propagation-shape target-#4-style investigation
      // name the caller's arg without hand-grepping the recomp for every rec_dispatch site.
      s_dw = (uint32_t)strtoul(e, nullptr, 0);
      const char *rk = strstr(e, ":ra=");
      if (rk) {
        s_dw_ra = (uint32_t)strtoul(rk + 4, nullptr, 0);
      }
    }
  }
  if (s_dw && (addr & 0x1FFFFFFF) == (s_dw & 0x1FFFFFFF) &&
      (!s_dw_ra || (c->r[31] & 0x1FFFFFFF) == (s_dw_ra & 0x1FFFFFFF))) {
    Sbs *sbs = c->game ? c->game->sbs : 0;
    int cid = sbs ? sbs->coreId(c) : -1;
    uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6];
    bool a0_addr = (a0 & 0x1FFFFFFF) < 0x200000 && a0 != 0;
    uint32_t s0 = a0_addr ? c->mem_r8(a0 + 4) : 0xff;
    uint32_t n3 = a0_addr ? c->mem_r8(a0 + 3) : 0xff;
    // Origin-revealing line: caller ra + full a0/a1/a2 (raw + as-signed-int16 for LUT lookups where
    // small negative args are meaningful). Frame number when sbs is available.
    uint32_t frame = sbs ? sbs->frame() : 0;
    lucent::info(
        "dispwatch",
        "f{} core={} addr={:08X} ra={:08X} a0={:08X} (s16={}) a1={:08X} a2={:08X} node.s0={}.n3={} stage={:08X}",
        frame,
        cid < 0 ? '?' : (cid ? 'B' : 'A'),
        addr,
        c->r[31],
        a0,
        (int)(int16_t)a0,
        a1,
        a2,
        s0,
        n3,
        c->mem_r32(0x801fe00c));
    // Also dump the guest stack so the CALLER CHAIN — not just the immediate ra — is visible. Cheap
    // when the hit is rare (dispwatch is targeted) and doing it by hand each time is what led to the
    // "no call stack" workflow gap (docs/known-bugs.md GAP-1). Pipe stderr through tools/symres.py to
    // resolve addresses to FUN_/native names automatically.
    guest_backtrace_to(c, stderr);
  }
  // THE OT-ATTRIBUTION PUSHES THAT USED TO LIVE HERE ARE GONE, and their absence is the fix rather than a
  // regression. They wrapped the generated dispatch switches, which reach a guest function through its
  // WRAPPER — and the wrapper now scopes attribution itself (tools/recomp/emit.py), on direct calls as
  // well as indirect. Keeping these would push the same address TWICE for every indirect dispatch, which
  // breaks otattrCaller(): the caller of a span would be read back as the span's own function.
  //
  // They were also channel-gated, which made the guest leg's identity depend on whether a debug channel
  // happened to be on. The wrapper's push is unconditional and was measured free (+0.0% user CPU), so the
  // attribution no longer changes shape between a diagnostic run and a normal one.
  uint32_t a = addr & 0x1FFFFFFF;
  const RecompRegistry *R = psxport_recomp();
  const GuestProgramImage &image = program_image_for_routing(c);
  if (image.residentText.containsPhysical(a)) {
    R->main_dispatch(c, addr); // pushes/pops inside the wrapper now — see the note above
    return;
  }
  // A RELOCATABLE module owns whatever range it was placed at, so it is routed by live base. Its
  // own switch takes the LIVE address (that is what a dispatch miss must report) and subtracts the
  // delta itself, so nothing is translated here.
  const int live = overlay_live_index(c, addr);
  if (live >= 0) {
    R->overlays[live].disp(c, addr); // pushes/pops inside the wrapper now
    return;
  }
  const FixedOverlayResolution fixed = overlay_resolve_fixed(c, addr);
  if (fixed.overlay) {
    fixed.overlay->disp(c, addr); // pushes/pops inside the wrapper now
    return;
  }
  if (fixed.addressInOverlayRange) {
    lucent::error("overlay-router",
                  "addr 0x{:08X} has {} fixed-overlay signature match; refusing to guess.",
                  addr,
                  fixed.ambiguous ? "an AMBIGUOUS" : "NO resident");
    for (int i = 0; i < R->overlay_count; ++i) {
      const RecOverlay &candidate = R->overlays[i];
      if (candidate.relocatable || !fixed_overlay_contains(candidate, a)) {
        continue;
      }
      const unsigned char *ram = c->ram + (candidate.base & 0x1FFFFFFFu);
      const bool signatureMatches = candidate.sig && candidate.siglen > 0 && candidate.siglen <= 32 &&
                                    memcmp(candidate.sig, ram, candidate.siglen) == 0;
      lucent::info("overlay-router",
                   "  candidate '{}' range 0x{:08X}..0x{:08X}, signature {}",
                   candidate.name ? candidate.name : "(null)",
                   candidate.base,
                   candidate.end,
                   signatureMatches ? "matches" : "does not match");
    }
  }
  rec_dispatch_miss(c, addr);
}
