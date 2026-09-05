#include "core.h"
#include "invalidation.h"
#include "render_substrate.h"

#include <cstdlib>
#include <cstring>
#include <lucent/log.h>

uint8_t Core::mem_r8(uint32_t a) {
  uint8_t *p = host_ptr(a, 1);
  return p ? *p : (uint8_t)io_read(a, 1);
}
uint16_t Core::mem_r16(uint32_t a) {
  uint8_t *p = host_ptr(a, 2);
  if (!p) {
    return (uint16_t)io_read(a, 2);
  }
  uint16_t v;
  memcpy(&v, p, 2);
  return v;
}
uint32_t Core::mem_r32(uint32_t a) {
  uint8_t *p = host_ptr(a, 4);
  if (!p) {
    return io_read(a, 4);
  }
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}
// OT/GTE submission attribution (`debug otattr`, game/render/ot_attr.h): attribute a packet-pool store
// to the current otattr-shadow-stack fn + render-walk node. No-op unless the channel is on.
static inline void pkt_track(Core *c, uint32_t a, uint32_t bytes) {
  c->rsub.otAttr.trackStore(c, a, bytes);
}

// FAIL-FAST guard for the pc_render READ-ONLY-OVERLAY invariant (CLAUDE.md): pc_render's native
// display pass (DisplayPassGuard-armed scope in game_tomba2.cpp's Engine::drawOTag) reads guest RAM
// + engine state and draws to HOST memory only — it must NEVER write guest main RAM or scratchpad.
// Checked at the top of every guest store; the `!armed` branch is the hot-path no-op (single
// predicted-false bool read gates the address test, so this is cheap when the flag is off, which is
// always except during the pc_render display pass on this Core).
static void display_pass_write_guard(Core *c, uint32_t a, uint32_t v, int width) {
  if (!c->rsub.mode.displayPassArmed()) {
    return;
  }
  const uint32_t p = a & 0x1FFFFFFF;
  const bool guest_ram = p < 0x200000;
  const bool scratchpad = p >= 0x1F800000 && p < 0x1F800400;
  if (!guest_ram && !scratchpad) {
    return;
  }
  lucent::info("mem",
               "\n[pc_render VIOLATION] guest write to 0x{:08X} (val 0x{:X}, width {}) during pc_render display pass — "
               "pc_render MUST be a read-only overlay (CLAUDE.md). interp_pc={:08X} sp={:08X}",
               0x80000000u | p,
               v,
               width,
               c->pc,
               c->r[29]);
  abort();
}

template <class Value> void Core::writeGuestMemory(uint32_t a, Value v) {
  constexpr auto width = static_cast<uint32_t>(sizeof(Value));
  uint8_t *p = host_ptr(a, width);
  display_pass_write_guard(this, a, v, width);
  wwatch_check(a, v, width);
  cw_check(a, v, width);
  pkt_track(this, a, width);
  if (p) {
    memcpy(p, &v, width);
    psx::cpu::notifyExecutableWrite(*this, {a, a + width}, psx::cpu::ExecutableWriteSource::MappedStore);
  } else {
    io_write(a, v, width);
  }
}

void Core::mem_w8(uint32_t a, uint8_t v) {
  writeGuestMemory(a, v);
}
void Core::mem_w16(uint32_t a, uint16_t v) {
  writeGuestMemory(a, v);
}
void Core::mem_w32(uint32_t a, uint32_t v) {
  writeGuestMemory(a, v);
}

// lwl/lwr/swl/swr: little-endian unaligned word merge.
uint32_t Core::mem_lwl(uint32_t cur, uint32_t a) {
  const uint32_t aligned = mem_r32(a & ~3u);
  const uint32_t sh = (a & 3) * 8;
  const uint32_t keep = (0x00FFFFFFu >> sh);
  return (cur & keep) | (aligned << (24 - sh));
}
uint32_t Core::mem_lwr(uint32_t cur, uint32_t a) {
  const uint32_t aligned = mem_r32(a & ~3u);
  const uint32_t sh = (a & 3) * 8;
  const uint32_t keep = sh ? (0xFFFFFF00u << (24 - sh)) : 0;
  return (cur & keep) | (aligned >> sh);
}
void Core::mem_swl(uint32_t a, uint32_t v) {
  const uint32_t base = a & ~3u;
  const uint32_t sh = (a & 3) * 8;
  const uint32_t aligned = mem_r32(base);
  const uint32_t keep = 0xFFFFFF00u << sh;
  mem_w32(base, (aligned & keep) | (v >> (24 - sh)));
}
void Core::mem_swr(uint32_t a, uint32_t v) {
  const uint32_t base = a & ~3u;
  const uint32_t sh = (a & 3) * 8;
  const uint32_t aligned = mem_r32(base);
  // keep = the low `sh` bits (bytes BELOW the store address, preserved by SWR). 0xFFFFFFFF>>(32-sh).
  const uint32_t keep = sh ? (0xFFFFFFFFu >> (32 - sh)) : 0;
  mem_w32(base, (aligned & keep) | (v << sh));
}

// 0x8009A420 FUN_8009A420 — psyq libc `memset`, statically linked into MAIN.EXE (confirmed by
// its position immediately below `rand` at 0x8009A450 in the psyq libc block, per
// docs/engine_re.md). WIRED 2026-07-16 (guest_memset_install below; the pool.cpp callsite's
// guest dispatch routes here). Binary re-verification at 0x8009A420 found one draft bug: dst NULL
// returns 0 and **n<=0 also returns 0** (the delay slot
// loads r2=dst but the fall-through overwrites it with 0 — the earlier draft note claiming
// "return dst as-is" was wrong); else byte-fill [dst, dst+n) and return the ORIGINAL dst (the
// guest loop advances a copy of a0, never the value it returns). Leaf, no stack frame; a0/a2 are
// caller-saved so their guest-side advance needs no mirror.
uint32_t Core::guestMemset(uint32_t dst, uint8_t val, int32_t n) {
  if (dst == 0u) {
    return 0u;
  }
  if (n <= 0) {
    return 0u;
  }
  uint32_t cur = dst;
  for (int32_t i = 0; i < n; i++, cur++) {
    mem_w8(cur, val);
  }
  return dst;
}
