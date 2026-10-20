// Sony BIOS libc string/character/memory-compare leaves. Bytes travel through Core's guest-memory map:
// guest addresses are not host pointers, and KSEG0/KSEG1 aliases retain console copy order.
#include "bios_libc_string.h"

#include "core.h"

#include <cstdint>

namespace {

bool isAsciiSpace(uint8_t c) {
  return c == ' ' || (c >= '\t' && c <= '\r');
}

} // namespace

void psx::hle::guestCopy(Core &core, uint32_t dst, uint32_t src, uint32_t n, CopyOrder order) {
  if (order == CopyOrder::OverlapSafe && dst > src) {
    for (uint32_t i = n; i-- > 0;) {
      core.mem_w8(dst + i, core.mem_r8(src + i));
    }
    return;
  }
  for (uint32_t i = 0; i < n; i++) {
    core.mem_w8(dst + i, core.mem_r8(src + i));
  }
}

bool psx::hle::dispatchLibcString(Core *core, uint32_t fn) {
  const uint32_t a0 = core->r[psx::cpu::kA0];
  const uint32_t a1 = core->r[psx::cpu::kA1];
  const uint32_t a2 = core->r[psx::cpu::kA2];

  switch (fn) {
  case 0x27: // memmove(dst, src, n); 0x2C is the same leaf at another vector
  case 0x2C:
    psx::hle::guestCopy(*core, a0, a1, a2, psx::hle::CopyOrder::OverlapSafe);
    core->r[psx::cpu::kV0] = a0;
    return true;
  case 0x28: // bzero(dst, n)
    for (uint32_t i = 0; i < a1; i++) {
      core->mem_w8(a0 + i, 0);
    }
    core->r[psx::cpu::kV0] = a0;
    return true;
  case 0x2A: // memcpy(dst, src, n)
    psx::hle::guestCopy(*core, a0, a1, a2, psx::hle::CopyOrder::Forward);
    core->r[psx::cpu::kV0] = a0;
    return true;
  case 0x2B: // memset(dst, c, n)
    for (uint32_t i = 0; i < a2; i++) {
      core->mem_w8(a0 + i, (uint8_t)a1);
    }
    core->r[psx::cpu::kV0] = a0;
    return true;
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
    core->r[psx::cpu::kV0] = a0;
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
    core->r[psx::cpu::kV0] = static_cast<uint32_t>(static_cast<int32_t>(static_cast<int>(x) - static_cast<int>(y)));
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
    core->r[psx::cpu::kV0] = static_cast<uint32_t>(static_cast<int32_t>(difference));
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
    core->r[psx::cpu::kV0] = a0;
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
    core->r[psx::cpu::kV0] = static_cast<uint32_t>(static_cast<int32_t>(difference));
    return true;
  }
  case 0x1B: { // strlen(s)
    uint32_t length = 0;
    while (core->mem_r8(a0 + length)) {
      length++;
    }
    core->r[psx::cpu::kV0] = length;
    return true;
  }
  case 0x10:   // atoi(s)
  case 0x11: { // atol(s)
    uint32_t i = 0;
    while (isAsciiSpace(core->mem_r8(a0 + i))) {
      i++;
    }
    bool negative = false;
    const uint8_t sign = core->mem_r8(a0 + i);
    if (sign == '-' || sign == '+') {
      negative = sign == '-';
      i++;
    }
    uint32_t value = 0;
    for (uint8_t digit = core->mem_r8(a0 + i); digit >= '0' && digit <= '9'; digit = core->mem_r8(a0 + ++i)) {
      value = value * 10u + static_cast<uint32_t>(digit - '0');
    }
    core->r[psx::cpu::kV0] = negative ? 0u - value : value;
    return true;
  }
  case 0x25: { // toupper(c) — Sony BIOS's locale-independent ASCII leaf
    core->r[psx::cpu::kV0] = a0 >= static_cast<uint32_t>('a') && a0 <= static_cast<uint32_t>('z')
                                 ? a0 - (static_cast<uint32_t>('a') - static_cast<uint32_t>('A'))
                                 : a0;
    return true;
  }
  default:
    return false;
  }
}
