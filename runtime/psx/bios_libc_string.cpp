// Sony BIOS libc string/character/memory-compare leaves. Bytes travel through Core's guest-memory map:
// guest addresses are not host pointers, and KSEG0/KSEG1 aliases retain console copy order.
#include "bios_libc_string.h"

#include "core.h"

#include <cstdint>

namespace {

enum { V0 = 2, A0 = 4, A1 = 5, A2 = 6 };

} // namespace

bool bios_libc_string_dispatch(Core *core, uint32_t fn) {
  const uint32_t a0 = core->r[A0];
  const uint32_t a1 = core->r[A1];
  const uint32_t a2 = core->r[A2];

  switch (fn) {
  case 0x15: { // strcat(dst, src)
    uint32_t end = a0;
    while (core->mem_r8(end)) {
      end++;
    }
    uint32_t i = 0;
    uint8_t ch;
    do {
      ch = core->mem_r8(a1 + i);
      core->mem_w8(end + i, ch);
      i++;
    } while (ch);
    core->r[V0] = a0;
    return true;
  }
  case 0x17: { // strcmp(s1, s2)
    uint32_t i = 0;
    uint8_t x;
    uint8_t y;
    do {
      x = core->mem_r8(a0 + i);
      y = core->mem_r8(a1 + i);
      i++;
    } while (x && x == y);
    core->r[V0] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int>(x) - static_cast<int>(y)));
    return true;
  }
  case 0x18: { // strncmp(s1, s2, n)
    int difference = 0;
    for (uint32_t i = 0; i < a2; i++) {
      const uint8_t x = core->mem_r8(a0 + i);
      const uint8_t y = core->mem_r8(a1 + i);
      difference = static_cast<int>(x) - static_cast<int>(y);
      if (difference || !x) {
        break;
      }
    }
    core->r[V0] = static_cast<uint32_t>(static_cast<int32_t>(difference));
    return true;
  }
  case 0x19: { // strcpy(dst, src)
    uint32_t i = 0;
    uint8_t ch;
    do {
      ch = core->mem_r8(a1 + i);
      core->mem_w8(a0 + i, ch);
      i++;
    } while (ch);
    core->r[V0] = a0;
    return true;
  }
  case 0x1A: { // memcmp(s1, s2, n)
    int difference = 0;
    for (uint32_t i = 0; i < a2; i++) {
      const uint8_t x = core->mem_r8(a0 + i);
      const uint8_t y = core->mem_r8(a1 + i);
      difference = static_cast<int>(x) - static_cast<int>(y);
      if (difference) {
        break;
      }
    }
    core->r[V0] = static_cast<uint32_t>(static_cast<int32_t>(difference));
    return true;
  }
  case 0x1B: { // strlen(s)
    uint32_t length = 0;
    while (core->mem_r8(a0 + length)) {
      length++;
    }
    core->r[V0] = length;
    return true;
  }
  case 0x25: { // toupper(c) — Sony BIOS's locale-independent ASCII leaf
    core->r[V0] = a0 >= static_cast<uint32_t>('a') && a0 <= static_cast<uint32_t>('z')
                      ? a0 - (static_cast<uint32_t>('a') - static_cast<uint32_t>('A'))
                      : a0;
    return true;
  }
  default:
    return false;
  }
}
