#include "rng.h"
#include "game_ctx.h"
#include "core.h"
#include "game.h"
#include "sbs.h"
#include "cfg.h"
#include <cstdio>
#include <cstdlib>
#include <execinfo.h>

// Slip #5 probe (2026-07-03): count Rng::next() calls per Core, per (ra_a, ra_b) bucket to name the
// native handler that advances the seed at a different cadence than its recomp equivalent. Enable
// with PSXPORT_RNG_CALLTRACE=1. Dumps at exit via atexit — piggybacks on the existing SBS exit path
// so the log ends with a settled-state table. Ra is c->r[31] (guest ra) — for native ports the
// return address is DEAD0000 or a stale value, so the bucket lump names the calling shape, not a
// specific PC. Compare A vs B counts: same total = seed stays synced; different total = the diff
// IS the drift source. Cheap: increments two ints + a map lookup per call.
namespace {
struct Bucket { uint64_t a = 0, b = 0; };
struct Tracer {
  uint64_t totalA = 0, totalB = 0;
  // Key: guest c->r[31] rounded down to fn-entry granularity (kept as-is; caller can post-process).
  // Simple linear-probed hash keyed by (ra, is_b). Cap size for cheap hot path.
  struct Row { uint32_t ra; uint64_t a; uint64_t b; };
  static constexpr int NROWS = 256;
  Row rows[NROWS] = {};
  int nRows = 0;
  bool on = false;
  Tracer() {
    const char* e = std::getenv("PSXPORT_RNG_CALLTRACE");
    on = e && *e && e[0] != '0';
    if (on) std::atexit(&Tracer::dumpStatic);
  }
  void bump(Core* c) {
    if (!on) return;
    Sbs* sbs = c->game ? c->game->sbs : nullptr;
    int side = sbs ? sbs->coreId(c) : 0;
    if (side < 0) side = 0;
    // Bucket by HOST caller (the C fn calling rngOf(c).next()). Guest r[31] is stale DEAD0000 in
    // native handlers, so it doesn't distinguish callers. backtrace() at depth 3 skips the bump
    // frame, Rng::next frame, and lands at the caller — the native beh/handler that consumed the
    // RNG. This names the specific `beh_*` fn or `Cull::*` method that over- or under-advances.
    // Depth 2 = the direct caller of Rng::next (skip bump() and next() frames). BehaviorDispatch::
    // dispatchObj was collapsing many beh_* handlers at depth 3, so use depth 2 to name the beh
    // handler itself.
    void* bt[3];
    int n = backtrace(bt, 3);
    uintptr_t caller = (n >= 2) ? (uintptr_t)bt[1] : 0;
    uint32_t ra = (uint32_t)(caller & 0xFFFFFFFFu);   // truncate for the 32-bit ra slot
    for (int i = 0; i < nRows; i++) {
      if (rows[i].ra == ra) {
        if (side) { rows[i].b++; totalB++; } else { rows[i].a++; totalA++; }
        return;
      }
    }
    if (nRows < NROWS) {
      rows[nRows].ra = ra;
      if (side) { rows[nRows].b = 1; totalB++; } else { rows[nRows].a = 1; totalA++; }
      nRows++;
    } else {
      if (side) totalB++; else totalA++;
    }
  }
  static Tracer& instance() { static Tracer t; return t; }
  static void dumpStatic() { instance().dump(stderr); }
  void dump(FILE* out) {
    std::fprintf(out, "[rng-calltrace] totalA=%llu totalB=%llu deltaA-B=%lld  (unique ra=%d)\n",
                 (unsigned long long)totalA, (unsigned long long)totalB,
                 (long long)((long long)totalA - (long long)totalB), nRows);
    // Sort by |a-b| descending — biggest drift contributors first.
    int idx[NROWS];
    for (int i = 0; i < nRows; i++) idx[i] = i;
    for (int i = 0; i < nRows; i++)
      for (int j = i + 1; j < nRows; j++) {
        long long di = (long long)rows[idx[i]].a - (long long)rows[idx[i]].b;
        long long dj = (long long)rows[idx[j]].a - (long long)rows[idx[j]].b;
        if (std::llabs(dj) > std::llabs(di)) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
      }
    std::fprintf(out, "[rng-calltrace] top drift rows (host_caller: A / B / A-B):\n");
    // Resolve host caller addrs to symbols so the row names the .cpp fn directly.
    void* addrs[32]; int nres = 0;
    for (int i = 0; i < nRows && nres < 32; i++, nres++) addrs[nres] = (void*)(uintptr_t)rows[idx[i]].ra;
    char** syms = backtrace_symbols(addrs, nres);
    for (int i = 0; i < nres; i++) {
      Row& r = rows[idx[i]];
      long long d = (long long)r.a - (long long)r.b;
      std::fprintf(out, "[rng-calltrace]   %8llu  %8llu  %+lld  %s\n",
                   (unsigned long long)r.a, (unsigned long long)r.b, d,
                   syms ? syms[i] : "(no symbol)");
    }
    if (syms) std::free(syms);
  }
};
}  // namespace

int32_t Rng::next() {
  Tracer::instance().bump(core);
  uint32_t s = core->mem_r32(SEED_ADDR) * 0x41C64E6Du + 0x3039u;
  core->mem_w32(SEED_ADDR, s);
  return (int32_t)((s >> 16) & 0x7FFFu);
}

// FUN_80032A44 — scaled random. Disas 0x80032A44..0x80032A84 verbatim: `sra v0, 15` on the 32×32
// MULT product. Both operands are treated as 32-bit signed (recomp: MULT after mov s0,a1/s1,a0 —
// no explicit sign-extension needed since a0/a1 arrive as 32-bit int registers).
int32_t Rng::inRange(int32_t lo, int32_t hi) {
  int32_t r = next();                                  // [0, 0x7FFF]
  int32_t span = hi - lo;                              // caller expected: hi >= lo (recomp doesn't check)
  int32_t prod = (int32_t)((int64_t)r * (int64_t)span);
  return (prod >> 15) + lo;
}
