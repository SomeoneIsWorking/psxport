#include "trig.h"
#include "core.h"

int32_t Trig::rsin(Core* c, int32_t angle) {
  int32_t sign = 1;
  if (angle < 0) { sign = -1; angle = -angle; }
  angle &= 0xFFF;
  auto lh = [&](uint32_t off) -> int32_t { return (int32_t)(int16_t)c->mem_r16(SIN_TAB + off); };
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

int32_t Trig::rcos(Core* c, int32_t angle) {
  if (angle < 0) angle = -angle;                         // cos is even; guest's bgez wrapper
  angle &= 0xFFF;
  auto lh = [&](uint32_t off) -> int32_t { return (int32_t)(int16_t)c->mem_r16(SIN_TAB + off); };
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
