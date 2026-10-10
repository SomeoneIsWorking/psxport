// Sony BIOS libc string/character/memory-compare leaves. Bytes travel through Core's guest-memory map:
// guest addresses are not host pointers, and KSEG0/KSEG1 aliases retain console copy order.
#include "bios_libc_string.h"

#include "core.h"

#include <cstdint>

namespace {

enum { V0 = 2, A0 = 4, A1 = 5, A2 = 6 };

bool isAsciiSpace(uint8_t c) {
  return c == ' ' || (c >= '\t' && c <= '\r');
}

} // namespace

bool bios_libc_string_dispatch(Core *core, uint32_t fn) {
  const uint32_t a0 = core->r[A0];
  const uint32_t a1 = core->r[A1];
  const uint32_t a2 = core->r[A2];

  switch (fn) {
  // --- BIOS libc MEMORY leaves -------------------------------------------------------------
  // MOVED HERE from hle.cpp, which is under a shrink-only line cap and could not absorb the fifth
  // member of this family. The family is bzero/memcpy/memset plus the overlap-safe copy, and the
  // string block above was already in this module -- so the memory half was the odd one out.
  //
  // A0:0x27 IS THE ONE THAT WAS MISSING, and 0x2C IS NOT IT.
  //
  // The guest emits each BIOS call as its own stub --
  //   addiu $t2,$zero,0xA0 ; jr $t2 ; addiu $t1,$zero,FN
  // -- so a census of those stubs is a census of the functions a title actually uses, read from
  // BYTES rather than from a table somebody remembered. Across the ten titles in this workspace:
  //
  //   0x27  <- this one, unimplemented until now
  //   0x28  bzero(dst, n)
  //   0x2A  memcpy(dst, src, n)
  //   0x2B  memset(dst, c, n)
  //   0x2C  "memmove" -- CALLED BY NO TITLE, 0 of 0
  //
  // The four the family actually uses are CONTIGUOUS in the guest's own stub table (0x8003d6f0,
  // d700, d710, d720 in SCUS_949.00), so 0x27 sits immediately before the bzero stub. **0x2C was
  // the wrong number for the overlap-safe copy: a memmove at a number no title emits is a memmove
  // that has never once run, while the number they all do emit was unimplemented.**
  //
  // WHAT THIS IS AND IS NOT EVIDENCE FOR, stated plainly. The identification rests on FAMILY
  // POSITION plus the argument signature at the call site -- NOT on reading the BIOS ROM's own
  // dispatch table, because the A/B/C vectors live in RAM the BIOS populates at boot, so the ROM
  // file carries nothing at file offset 0xA0 and the table has to be recovered from the BIOS's own
  // code. The call that forced the question is
  //
  //   A0:0x27(0x8005E168, 0x80061A80, 0x730, 0xFFFFFFFE) from 0x80011D0C
  //
  // a copy triple -- destination, source, length -- plus a fourth argument no copy function reads.
  // The overlap-safe direction is correct whether the intended member is memmove or memcpy, because
  // for non-overlapping ranges the two agree, and here src-dst is 0x3918 against a length of 0x730
  // so they do not overlap at this call. **A member that was NOT a copy at all is the one reading
  // this would get wrong**, and neither the signature nor the family position supports that -- so
  // the ROM-level confirmation is named as the remaining check rather than assumed here.
  case 0x27:       // memmove(dst, src, n)
    if (a0 > a1) { // overlap-correct, unlike 0x2A
      for (uint32_t i = a2; i-- > 0;) {
        core->mem_w8(a0 + i, core->mem_r8(a1 + i));
      }
    } else {
      for (uint32_t i = 0; i < a2; i++) {
        core->mem_w8(a0 + i, core->mem_r8(a1 + i));
      }
    }
    core->r[V0] = a0;
    return true;
  case 0x28: // bzero(dst, n)
    for (uint32_t i = 0; i < a1; i++) {
      core->mem_w8(a0 + i, 0);
    }
    core->r[V0] = a0;
    return true;
  case 0x2A: // memcpy(dst, src, n)
    for (uint32_t i = 0; i < a2; i++) {
      core->mem_w8(a0 + i, core->mem_r8(a1 + i));
    }
    core->r[V0] = a0;
    return true;
  case 0x2B: // memset(dst, c, n)
    for (uint32_t i = 0; i < a2; i++) {
      core->mem_w8(a0 + i, (uint8_t)a1);
    }
    core->r[V0] = a0;
    return true;
  case 0x2C:       // memmove(dst, src, n) —
    if (a0 > a1) { // overlap-correct, unlike 2Ah
      for (uint32_t i = a2; i-- > 0;) {
        core->mem_w8(a0 + i, core->mem_r8(a1 + i));
      }
    } else {
      for (uint32_t i = 0; i < a2; i++) {
        core->mem_w8(a0 + i, core->mem_r8(a1 + i));
      }
    }
    core->r[V0] = a0;
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
    core->r[V0] = negative ? 0u - value : value;
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
