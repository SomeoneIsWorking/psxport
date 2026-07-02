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

void Mtx::diagonal(uint32_t addr, int32_t x, int32_t y, int32_t z) {
  Core* c = this->core;
  // Guest sign-extends each arg with sll 16 / sra 16 (drop high half) — reproduce via u16 casts.
  uint32_t xw = (uint32_t)(int16_t)x & 0xFFFFu;
  uint32_t yw = (uint32_t)(int16_t)y & 0xFFFFu;
  uint32_t zw = (uint32_t)(int16_t)z & 0xFFFFu;
  c->mem_w32(addr +  0, xw);   // m[0][0]=x  m[0][1]=0
  c->mem_w32(addr +  4, 0);    // m[0][2]=0  m[1][0]=0
  c->mem_w32(addr +  8, yw);   // m[1][1]=y  m[1][2]=0
  c->mem_w32(addr + 12, 0);    // m[2][0]=0  m[2][1]=0
  c->mem_w32(addr + 16, zw);   // m[2][2]=z  pad=0
  c->mem_w32(addr + 20, 0);
  c->mem_w32(addr + 24, 0);
  c->mem_w32(addr + 28, 0);
}
