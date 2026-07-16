#include "mtx.h"
#include "core.h"

void Mtx::identity(uint32_t addr) {
  Core* c = this->core;
  // m[0][0]=0x1000 m[0][1]=0 | m[0][2]=0 m[1][0]=0 | m[1][1]=0x1000 m[1][2]=0 |
  // m[2][0]=0 m[2][1]=0    | m[2][2]=0x1000 (pad)   | t[0]=0 t[1]=0 t[2]=0
  c->mem_w32(addr +  0, 0x00001000u);
  c->mem_w32(addr +  4, 0);
  c->mem_w32(addr +  8, 0x00001000u);
  c->mem_w32(addr + 12, 0);
  c->mem_w32(addr + 16, 0x00001000u);
  c->mem_w32(addr + 20, 0);
  c->mem_w32(addr + 24, 0);
  c->mem_w32(addr + 28, 0);
}

// FUN_800517BC (diagonal-scale matrix write) is owned by `NodeXform::seedBlock`
// (game/render/node_xform.cpp), which is the real wired dispatch target for that address
// (registered via NodeXform::registerOverrides into the global override registry). This class used to carry an
// unused, uncalled duplicate implementation (`Mtx::diagonal`) — deleted 2026-07-08 (dual-ownership
// found via codemap; dead code, zero callers).
