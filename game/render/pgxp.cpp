// class Pgxp — impl. Also owns the Beetle `PGXP_pushSXYZ2f` C-ABI callback (routed to the currently-
// bound instance) and the vestigial `PGXP_NCLIP*` / `MDFNSS_StateAction` stubs Beetle expects.
#include "pgxp.h"
#include <cstring>
#include <cstdint>

// Beetle GTE data-register read (used to capture the view-space vertex IR1/IR2/IR3 at push time).
extern "C" uint32_t GTE_ReadDR(unsigned which);

Pgxp* Pgxp::sCurrent = nullptr;

void Pgxp::bind(Core* /*c*/) {
  sCurrent = this;
  std::memset(mSlots, 0, sizeof mSlots);   // invalidate cache — new binding, new frame's data owns it
}

void Pgxp::push(float x, float y, float z, uint32_t sxyPacked) {
  int sx = (int16_t)(sxyPacked & 0xFFFF), sy = (int16_t)(sxyPacked >> 16);
  uint32_t key = keyOf(sx, sy);
  Ent* e = &mSlots[slotOf(key)];
  e->key = key; e->x = x; e->y = y; e->z = z;
  // IR1/IR2/IR3 (data regs 9/10/11) still hold this vertex in view space at push time (TransformXY
  // doesn't touch them) — capture for renderer-side normal reconstruction.
  e->vx = (float)(int16_t)GTE_ReadDR(9);
  e->vy = (float)(int16_t)GTE_ReadDR(10);
  e->vz = (float)(int16_t)GTE_ReadDR(11);
  e->valid = 1;
}

bool Pgxp::lookup(int sx, int sy, float* px, float* py, float* pz) const {
  uint32_t key = keyOf(sx, sy);
  const Ent* e = &mSlots[slotOf(key)];
  if (e->valid && e->key == key) {
    if (px) *px = e->x; if (py) *py = e->y; if (pz) *pz = e->z;
    return true;
  }
  return false;
}

bool Pgxp::lookupView(int sx, int sy, float* vx, float* vy, float* vz) const {
  uint32_t key = keyOf(sx, sy);
  const Ent* e = &mSlots[slotOf(key)];
  if (e->valid && e->key == key) {
    if (vx) *vx = e->vx; if (vy) *vy = e->vy; if (vz) *vz = e->vz;
    return true;
  }
  return false;
}

void Pgxp::frameReset() {
  for (int i = 0; i < kSize; i++) mSlots[i].valid = 0;
}

// ---- Beetle GTE C-ABI callbacks (routed to the currently-bound instance) --------------------------

extern "C" void PGXP_pushSXYZ2f(float x, float y, float z, uint32_t v) {
  if (auto* p = Pgxp::current()) p->push(x, y, z, v);
}

// Vestigial Beetle hooks — savestate / NCLIP-precision — unused by this port but expected by gte.c.
extern "C" int   PGXP_NCLIP_valid(uint32_t a, uint32_t b, uint32_t c) { (void)a; (void)b; (void)c; return 0; }
extern "C" float PGXP_NCLIP(void)                                     { return 0.0f; }
extern "C" int   MDFNSS_StateAction(void* st, int load, int data_only, void* sf, const char* name) {
  (void)st; (void)load; (void)data_only; (void)sf; (void)name; return 1;
}

// ---- Free-function shim so callers with `Core* c` in scope keep working during the migration ------
// (kept intentionally thin — new code should call `c->mRender->pgxp.lookup*(...)`)
int pgxp_lookup(int sx, int sy, float* px, float* py, float* pz) {
  auto* p = Pgxp::current(); return p && p->lookup(sx, sy, px, py, pz) ? 1 : 0;
}
int pgxp_lookup_view(int sx, int sy, float* vx, float* vy, float* vz) {
  auto* p = Pgxp::current(); return p && p->lookupView(sx, sy, vx, vy, vz) ? 1 : 0;
}
void pgxp_frame_reset(void) {
  if (auto* p = Pgxp::current()) p->frameReset();
}
