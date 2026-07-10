#include "engine_overrides.h"
#include "core.h"
#include "game.h"
#include "sbs.h"
#include "cfg.h"
#include "verify_harness.h"
#include <cstdio>
#include <cstdlib>

// `ovhit` dump registry — bounded fixed-size array of every EngineOverrides instance that ever
// registered anything, NOT a single "first wins" pointer. The old design bound to whichever Game's
// EngineOverrides::register_ fired FIRST and dumped ONLY that one; under SBS full, dc_boot_init
// runs register_engine_overrides() on both Game A and Game B, and their relative init order is not
// guaranteed to put A (the pc_faithful side whose overrides actually FIRE) first — so the dump
// would sometimes land on B's table, which stays all-zero by design (B never consults run()),
// making every address read 0 even for long-verified overrides (docs/findings/animation.md
// "ovhit tooling caveat"). Recording every registrant and picking the print target BY ROLE
// (psx_fallback flag) at dump time fixes that regardless of registration order.
//
// SANCTIONED ATEXIT/SIGNAL EXCEPTION (same as overlay_router.cpp's recdep hook and sbs.cpp's
// alloc-trace hook): atexit() takes no context, so a small bounded static registry is required to
// reach the live instances from the atexit callback. At most 2 Games exist in any run today (SBS
// full's A/B pair) but the registry is sized generously.
namespace {
constexpr int kMaxOvhitTargets = 8;
struct OvhitTarget { EngineOverrides* ov; Game* game; };
OvhitTarget s_ovhitTargets[kMaxOvhitTargets];
int  s_ovhitCount = 0;
bool s_ovhitAtexitHooked = false;

void ovhit_dump_atexit() {
  if (!cfg_dbg("ovhit") || s_ovhitCount == 0) return;
  // Pick the primary: the first registrant whose Game is NOT psx_fallback (the pc_faithful side —
  // the only side whose EngineOverrides::run() ever actually fires). If every registrant is
  // psx_fallback (e.g. a lone PSXPORT_GATE=1 run with no SBS peer), fall back to the first one so
  // the (expected all-zero) table still prints rather than silently vanishing.
  int primaryIdx = -1;
  for (int i = 0; i < s_ovhitCount; i++)
    if (s_ovhitTargets[i].game && !s_ovhitTargets[i].game->psx_fallback) { primaryIdx = i; break; }
  if (primaryIdx < 0) primaryIdx = 0;
  const OvhitTarget& primary = s_ovhitTargets[primaryIdx];

  // Peer: a DIFFERENT registrant sharing the same `sbs` facade as the primary (i.e. its SBS
  // opposite core), used as the substrate-parity source. In non-SBS runs game->sbs is null on
  // every instance, so peer stays null and dumpHitCounts() prints A-only.
  const EngineOverrides* peer = nullptr;
  if (primary.game && primary.game->sbs) {
    for (int i = 0; i < s_ovhitCount; i++) {
      if (i == primaryIdx) continue;
      if (s_ovhitTargets[i].game && s_ovhitTargets[i].game->sbs == primary.game->sbs) {
        peer = s_ovhitTargets[i].ov;
        break;
      }
    }
  }
  const char* role = (primary.game && primary.game->psx_fallback) ? "psx_fallback (no live overrides expected)"
                    : peer ? "core A / pc_faithful (SBS)" : "single";
  fprintf(stderr, "[ovhit] EngineOverrides primary = %s%s\n", role, peer ? " (B(gen) column = SBS core-B substrate parity)" : "");
  primary.ov->dumpHitCounts(peer);

  if (s_ovhitCount > 2)
    fprintf(stderr, "[ovhit] note: %d EngineOverrides instances registered this run (only the "
                    "primary+peer pair above are shown; extra instances are usually DualCore/"
                    "Selftest harness cores sharing no `sbs` facade with the primary).\n", s_ovhitCount);
}
}  // namespace

void EngineOverrides::register_(uint32_t addr, const char* name, EngineOverrideFn fn) {
  uint32_t k = (addr & 0x1FFFFFFF) | 0x80000000u;
  for (int i = 0; i < mN; i++) {
    if (mAddr[i] == k) {
      fprintf(stderr, "[engine-ov] DUPLICATE registration 0x%08X (%s vs %s)\n", addr, name, mName[i]);
      abort();
    }
  }
  if (mN >= kMax) { fprintf(stderr, "[engine-ov] table full registering 0x%08X (%s)\n", addr, name); abort(); }
  mAddr[mN] = k; mName[mN] = name; mFn[mN] = fn; mN++;
  if (k < mLo) mLo = k;
  if (k > mHi) mHi = k;
  bool seen = false;
  for (int i = 0; i < s_ovhitCount; i++) if (s_ovhitTargets[i].ov == this) { seen = true; break; }
  if (!seen) {
    if (s_ovhitCount < kMaxOvhitTargets) s_ovhitTargets[s_ovhitCount++] = { this, game };
    if (!s_ovhitAtexitHooked) { s_ovhitAtexitHooked = true; atexit(ovhit_dump_atexit); }
  }
}

bool EngineOverrides::run(Core* c, uint32_t addr) {
  uint32_t k = (addr & 0x1FFFFFFF) | 0x80000000u;
  if (k < mLo || k > mHi) return false;
  // Self-check (2026-07-10): overlay_router.cpp already gates the call to run() on
  // !psx_fallback && !inSubstrateLeg — this defends the invariant AT the choke point too, so a
  // future caller that forgets the gate aborts loudly instead of silently contaminating the oracle
  // (the exact fake-green bug class this whole file's header comment documents).
  if (game && game->verify.inSubstrateLeg) {
    fprintf(stderr, "[engine-ov] BUG: run() reached while inSubstrateLeg is set (oracle purity "
                    "violated) addr=0x%08X — the substrate replay leg must never consult "
                    "EngineOverrides.\n", addr);
    abort();
  }
  for (int i = 0; i < mN; i++) {
    if (mAddr[i] != k) continue;
    mHits[i]++;
    if (cfg_dbg("dispatch")) {
      Sbs* sbs = game ? game->sbs : nullptr;
      int cid = sbs ? sbs->coreId(c) : -1;
      fprintf(stderr, "[dispatch] f%u core=%c 0x%08X %s ra=%08X a0=%08X a1=%08X a2=%08X a3=%08X\n",
              sbs ? sbs->frame() : 0, cid < 0 ? '-' : (cid ? 'B' : 'A'),
              addr, mName[i], c->r[31], c->r[4], c->r[5], c->r[6], c->r[7]);
    }
    // PSXPORT_MIRROR_VERIFY[=all|=0xADDR,...]: same generalized gate as engine_override_thunk.cpp
    // (see its comment) — covers every address wired via EngineOverrides::register_ as well as the
    // g_tab[] thunk table, so `=all` reaches BOTH override mechanisms per requirement.
    if (game && game->verify.mirrorSampleGate(addr)) {
      struct NativeCtx { EngineOverrideFn fn; Core* c; };
      NativeCtx nc{mFn[i], c};
      auto go = [](void* p) { NativeCtx* n = (NativeCtx*)p; n->fn(n->c); };
      game->verify.strictCheck(addr, go, &nc);
    } else {
      mFn[i](c);
    }
    return true;
  }
  return false;
}

void EngineOverrides::traceHit(Core* c, uint32_t addr, bool viaDirect) {
  uint32_t k = (addr & 0x1FFFFFFF) | 0x80000000u;
  for (int i = 0; i < mN; i++) {
    if (mAddr[i] != k) continue;
    mHits[i]++;
    if (cfg_dbg("dispatch") || cfg_dbg("ovhit")) {
      Sbs* sbs = game ? game->sbs : nullptr;
      int cid = sbs ? sbs->coreId(c) : -1;
      fprintf(stderr, "[dispatch] f%u core=%c 0x%08X %s ra=%08X a0=%08X a1=%08X a2=%08X a3=%08X%s\n",
              sbs ? sbs->frame() : 0, cid < 0 ? '-' : (cid ? 'B' : 'A'),
              addr, mName[i], c->r[31], c->r[4], c->r[5], c->r[6], c->r[7],
              viaDirect ? " (direct/g_override, bypassed rec_dispatch)" : "");
    }
    return;
  }
  fprintf(stderr, "[engine-ov] traceHit on unregistered 0x%08X — trampoline/register_ mismatch\n", addr);
  abort();
}

void EngineOverrides::noteSubstrateDispatch(uint32_t addr) {
  uint32_t k = (addr & 0x1FFFFFFF) | 0x80000000u;
  if (k < mLo || k > mHi) return;
  for (int i = 0; i < mN; i++) {
    if (mAddr[i] != k) continue;
    mSubHits[i]++;
    return;
  }
}

uint64_t EngineOverrides::subHitsFor(uint32_t addr) const {
  uint32_t k = (addr & 0x1FFFFFFF) | 0x80000000u;
  if (k < mLo || k > mHi) return 0;
  for (int i = 0; i < mN; i++)
    if (mAddr[i] == k) return mSubHits[i];
  return 0;
}

void EngineOverrides::dumpHitCounts(const EngineOverrides* peerSubstrate) const {
  if (!cfg_dbg("ovhit")) return;
  fprintf(stderr, "[ovhit] EngineOverrides hit counts (%d registered)%s:\n", mN,
          peerSubstrate ? " — B(gen) = SBS core-B substrate-dispatch count for the same address" : "");
  for (int i = 0; i < mN; i++) {
    if (peerSubstrate) {
      uint64_t b = peerSubstrate->subHitsFor(mAddr[i]);
      fprintf(stderr, "  0x%08X %-32s : A=%llu B(gen)=%llu%s%s\n", mAddr[i], mName[i],
              (unsigned long long)mHits[i], (unsigned long long)b,
              mHits[i] == 0 ? "   <-- NEVER HIT (registered but unreached)" : "",
              (mHits[i] != 0 && mHits[i] != b) ? "   <-- COUNT MISMATCH vs substrate" : "");
    } else {
      fprintf(stderr, "  0x%08X %-32s : %llu%s\n", mAddr[i], mName[i],
              (unsigned long long)mHits[i], mHits[i] == 0 ? "   <-- NEVER HIT (registered but unreached)" : "");
    }
  }
}
