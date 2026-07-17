// class DualviewSnapshot — per-Core dual-view render-compare snapshot. See dualview_snapshot.h.
#include "core.h"
#include "dualview_snapshot.h"
#include <string.h>

extern "C" uint32_t gte_read_ctrl(uint32_t);
extern "C" uint32_t gte_read_data(uint32_t);
extern "C" void     gte_write_ctrl(uint32_t, uint32_t);
extern "C" void     gte_write_data(uint32_t, uint32_t);

static void dv_save(Core* c, uint8_t* ram, uint8_t* spad, uint32_t* gc, uint32_t* gd) {
  memcpy(ram, c->ram, 0x200000);
  memcpy(spad, c->scratch, 0x400);
  for (int i = 0; i < 32; i++) { gc[i] = gte_read_ctrl(i); gd[i] = gte_read_data(i); }
}
static void dv_load(Core* c, const uint8_t* ram, const uint8_t* spad, const uint32_t* gc, const uint32_t* gd) {
  memcpy(c->ram, ram, 0x200000);
  memcpy(c->scratch, spad, 0x400);
  for (int i = 0; i < 32; i++) { gte_write_ctrl(i, gc[i]); gte_write_data(i, gd[i]); }
}

void DualviewSnapshot::capturePre(Core* c)  { dv_save(c, mPreRam,  mPreSpad,  mPreGc,  mPreGd);  mHavePre = true; }
void DualviewSnapshot::capturePost(Core* c) { dv_save(c, mPostRam, mPostSpad, mPostGc, mPostGd); }
void DualviewSnapshot::restorePre(Core* c)  { dv_load(c, mPreRam,  mPreSpad,  mPreGc,  mPreGd);  }
void DualviewSnapshot::restorePost(Core* c) { dv_load(c, mPostRam, mPostSpad, mPostGc, mPostGd); }
