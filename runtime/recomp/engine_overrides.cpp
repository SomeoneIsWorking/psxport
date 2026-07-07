#include "engine_overrides.h"
#include "core.h"
#include "game.h"
#include "sbs.h"
#include "cfg.h"
#include <cstdio>
#include <cstdlib>

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
}

bool EngineOverrides::run(Core* c, uint32_t addr) {
  uint32_t k = (addr & 0x1FFFFFFF) | 0x80000000u;
  if (k < mLo || k > mHi) return false;
  for (int i = 0; i < mN; i++) {
    if (mAddr[i] != k) continue;
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
