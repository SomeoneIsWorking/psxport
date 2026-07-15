#include "trig.h"
#include "core.h"

int32_t Trig::rsin(int32_t angle) const {
  Core* c = this->core;
  int32_t sign = 1;
  if (angle < 0) { sign = -1; angle = -angle; }
  angle &= 0xFFF;
  auto lh = [&](uint32_t off) -> int32_t { return c->mem_r16s(SIN_TAB + off); };
  int32_t r;
  if (angle < 1025) {
    r = lh((uint32_t)angle * 2u);                        // Q1: sin(a) = tab[a]
  } else if (angle < 2049) {
    r = lh((uint32_t)(2048 - angle) * 2u);               // Q2: sin(a) = tab[2048-a]
  } else if (angle < 3073) {
    r = -lh((uint32_t)(angle - 2048) * 2u);              // Q3: sin(a) = -tab[a-2048]
  } else {
    r = -lh((uint32_t)(4096 - angle) * 2u);              // Q4: sin(a) = -tab[4096-a]
  }
  return sign * r;
}

int32_t Trig::ratan2(int32_t y_in, int32_t x_in) const {
  Core* c = this->core;
  // Guest FUN_80085690. MIPS convention: a0=y, a1=x. Returns 12-bit angle (4096 == 2π).
  // Two flags — a2 = "x was negative", a3 = "y was negative"; sign-strip both, table-lookup atan on the
  // first octant, quadrant fixup at the tail. `q * 2` is the table offset (int16 entries at guest 0x800AA490).
  uint32_t a0 = (uint32_t)y_in;
  uint32_t a1 = (uint32_t)x_in;
  int32_t x_neg = 0, y_neg = 0;
  if ((int32_t)a1 < 0) { x_neg = 1; a1 = 0u - a1; }
  if ((int32_t)a0 < 0) { y_neg = 1; a0 = 0u - a0; }
  if (a1 == 0 && a0 == 0) return 0;
  auto lh = [&](uint32_t idx) -> int32_t {
    return c->mem_r16s(ATAN_TAB + idx * 2u);
  };
  int32_t v1;
  if ((int32_t)a0 < (int32_t)a1) {
    // |y| < |x| — atan(y/x). Guest uses (y<<10)/x, or if y already has top-10 bits set, y/(x>>10) to
    // keep the divisor in range. First-octant so q ∈ [0, 1024] and table[q] is atan(q/1024)*4096/2π.
    uint32_t q;
    if (a0 & 0x7FE00000u) {
      q = (uint32_t)((int32_t)a0 / ((int32_t)a1 >> 10));
    } else {
      q = (uint32_t)((int32_t)(a0 << 10) / (int32_t)a1);
    }
    v1 = lh(q);
  } else {
    // |y| >= |x| — π/2 - atan(x/y). Same overflow-guard split.
    uint32_t q;
    if (a1 & 0x7FE00000u) {
      q = (uint32_t)((int32_t)a1 / ((int32_t)a0 >> 10));
    } else {
      q = (uint32_t)((int32_t)(a1 << 10) / (int32_t)a0);
    }
    v1 = 1024 - lh(q);
  }
  if (x_neg) v1 = 2048 - v1;
  if (y_neg) v1 = -v1;
  return v1;
}

int32_t Trig::angleCmp(int32_t a, int32_t b, int32_t mode) {
  uint32_t d = (uint32_t)(a - b - 1024) & 0xFFFu;
  int32_t inFirstHalf = (d < 2048u) ? 1 : 0;
  return (mode == 0) ? inFirstHalf : (inFirstHalf ^ 1);
}

int32_t Trig::rcos(int32_t angle) const {
  Core* c = this->core;
  if (angle < 0) angle = -angle;                         // cos is even; guest's bgez wrapper
  angle &= 0xFFF;
  auto lh = [&](uint32_t off) -> int32_t { return c->mem_r16s(SIN_TAB + off); };
  if (angle < 1025) {
    return lh((uint32_t)(1024 - angle) * 2u);            // Q1: cos(a) = tab[1024-a]
  } else if (angle < 2049) {
    return -lh((uint32_t)(angle - 1024) * 2u);           // Q2: cos(a) = -tab[a-1024]
  } else if (angle < 3073) {
    return -lh((uint32_t)(3072 - angle) * 2u);           // Q3: cos(a) = -tab[3072-a]
  } else {
    return lh((uint32_t)(angle - 3072) * 2u);            // Q4: cos(a) = tab[a-3072]
  }
}

// ── Override wiring (phase-3 fallthrough native-ize, 2026-07-15) ────────────────────────────────────
#include "game.h"
#include "engine_overrides.h"
extern void shard_set_override(uint32_t, void (*)(Core*));
extern void gen_func_80083E80(Core*);
extern void gen_func_80085690(Core*);
namespace {
void ov_trigRsin(Core* c) {
  if (c->game->psx_fallback) { gen_func_80083E80(c); return; }
  c->game->engine_overrides.traceHit(c, 0x80083E80u);
  c->r[2] = (uint32_t)c->trig.rsin((int32_t)c->r[4]);
}
void ov_trigRatan2(Core* c) {
  if (c->game->psx_fallback) { gen_func_80085690(c); return; }
  c->game->engine_overrides.traceHit(c, 0x80085690u);
  c->r[2] = (uint32_t)c->trig.ratan2((int32_t)c->r[4], (int32_t)c->r[5]);
}
}  // namespace

// angleCmp (0x80077768) is NOT wired here — it fired in NONE of the current replays (its dispatch
// caller is CutsceneCamera::snapFollowA), so MIRROR_VERIFY couldn't gate it. Deferred to Phase 5
// (cutscene-camera scene coverage). rsin/ratan2 fire constantly (MV 102593/79617 passes, byte-exact).
void Trig::registerOverrides(Game* game) {
  EngineOverrides& ov = game->engine_overrides;
  ov.register_(0x80083E80u, "Trig::rsin",   ov_trigRsin);   shard_set_override(0x80083E80u, ov_trigRsin);
  ov.register_(0x80085690u, "Trig::ratan2", ov_trigRatan2); shard_set_override(0x80085690u, ov_trigRatan2);
}
