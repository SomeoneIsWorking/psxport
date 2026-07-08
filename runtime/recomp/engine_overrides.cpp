#include "engine_overrides.h"
#include "core.h"
#include "game.h"
#include "sbs.h"
#include "cfg.h"
#include <cstdio>
#include <cstdlib>

// `ovhit` dump target: the FIRST EngineOverrides to register anything (in SBS full mode that's
// core A, the pc_faithful native side — core B's table only ever traces if something calls
// traceHit() unconditionally, which the psx_fallback-gated trampolines never do, so it would read
// all-zero and isn't worth a second dump). Same one-static-pointer shape as overlay_router.cpp's
// recdep atexit hook.
static EngineOverrides* s_ovhitTarget = nullptr;
static void ovhit_dump_atexit() { if (s_ovhitTarget) s_ovhitTarget->dumpHitCounts(); }

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
  if (!s_ovhitTarget) { s_ovhitTarget = this; atexit(ovhit_dump_atexit); }
}

bool EngineOverrides::run(Core* c, uint32_t addr) {
  uint32_t k = (addr & 0x1FFFFFFF) | 0x80000000u;
  if (k < mLo || k > mHi) return false;
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
    mFn[i](c);
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

void EngineOverrides::dumpHitCounts() const {
  if (!cfg_dbg("ovhit")) return;
  fprintf(stderr, "[ovhit] EngineOverrides hit counts (%d registered):\n", mN);
  for (int i = 0; i < mN; i++)
    fprintf(stderr, "  0x%08X %-32s : %llu%s\n", mAddr[i], mName[i],
            (unsigned long long)mHits[i], mHits[i] == 0 ? "   <-- NEVER HIT (registered but unreached)" : "");
}
