#include "mtx.h"
#include "core.h"

void Mtx::identity(Core* c, uint32_t addr) {
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
