#include "core.h"
#include "guest_memory_map.h"
#include "invalidation.h"
#include "render_substrate.h"
#include "side_effect_journal.h"

#include <cstdlib>
#include <cstring>
#include <lucent/log.h>

uint8_t Core::mem_r8(uint32_t a) {
  uint8_t *p = host_ptr({a, 1});
  return p ? *p : (uint8_t)deviceRead(a, 1);
}
void Core::readCString(uint32_t address, char *out, size_t cap) {
  size_t i = 0;
  for (; i + 1 < cap; i++) {
    const uint8_t ch = mem_r8(address + (uint32_t)i);
    out[i] = (char)ch;
    if (!ch) {
      break;
    }
  }
  out[i < cap ? i : cap - 1] = 0;
}
void Core::readBytes(uint32_t address, std::span<std::byte> out) {
  if (const uint8_t *p = host_ptr({address, static_cast<uint32_t>(out.size())})) {
    memcpy(out.data(), p, out.size());
    return;
  }
  for (size_t i = 0; i < out.size(); i++) {
    out[i] = static_cast<std::byte>(mem_r8(address + static_cast<uint32_t>(i)));
  }
}
uint16_t Core::mem_r16(uint32_t a) {
  uint8_t *p = host_ptr({a, 2});
  if (!p) {
    return (uint16_t)deviceRead(a, 2);
  }
  uint16_t v;
  memcpy(&v, p, 2);
  return v;
}
uint32_t Core::mem_r32(uint32_t a) {
  uint8_t *p = host_ptr({a, 4});
  if (!p) {
    return deviceRead(a, 4);
  }
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}
uint32_t Core::deviceRead(uint32_t a, uint32_t bytes) {
  if (sideEffectJournal == nullptr) {
    return io_read(a, bytes);
  }
  if (const auto replayed = sideEffectJournal->replayDeviceRead(a, bytes)) {
    return *replayed;
  }
  const uint32_t value = io_read(a, bytes);
  sideEffectJournal->recordDeviceRead(a, bytes, value);
  return value;
}
void Core::deviceWrite(uint32_t a, uint32_t v, uint32_t bytes) {
  if (sideEffectJournal == nullptr || sideEffectJournal->admitDeviceWrite(a, bytes, v)) {
    io_write(a, v, bytes);
  }
}
namespace {

// Packet-pool store attribution for `debug otattr`; a no-op unless the channel is on.
inline void pkt_track(Core *c, uint32_t a, uint32_t bytes) {
  c->rsub.otAttr.trackStore(c, {a, bytes});
}

// The native display pass reads guest state and draws to host memory only; a guest store inside it aborts.
void display_pass_write_guard(Core *c, GuestStore store) {
  if (!c->rsub.mode.displayPassArmed() || !c->isGuestStorage(store.address, store.width)) {
    return;
  }
  lucent::error("mem",
                "display-pass guard: guest write to 0x{:08X} (val 0x{:X}, width {}) inside the native display "
                "pass, which must not write guest memory. pc={:08X} sp={:08X}",
                psx::kseg0Address(store.address),
                store.value,
                store.width,
                c->pc,
                c->r[psx::cpu::kSp]);
  abort();
}

} // namespace

template <class Value, bool NotifyExecutable>
void Core::writeGuestMemory(uint32_t a, Value v, psx::cpu::ExecutableWriteSource source) {
  constexpr auto width = static_cast<uint32_t>(sizeof(Value));
  uint8_t *p = host_ptr({a, width});
  display_pass_write_guard(this, {a, static_cast<uint32_t>(v), width});
  if (storeWatch.armed()) {
    storeWatch.observe(*this, {a, static_cast<uint32_t>(v), width});
  }
  pkt_track(this, a, width);
  emission.noteStore(a);
  if (p) {
    memcpy(p, &v, width);
    if constexpr (NotifyExecutable) {
      psx::cpu::notifyExecutableWrite(*this, {a, a + width}, source);
    }
  } else {
    deviceWrite(a, v, width);
  }
}

void Core::mem_w8(uint32_t a, uint8_t v, psx::cpu::ExecutableWriteSource source) {
  writeGuestMemory(a, v, source);
}
void Core::mem_w16(uint32_t a, uint16_t v, psx::cpu::ExecutableWriteSource source) {
  writeGuestMemory(a, v, source);
}
void Core::mem_w32(uint32_t a, uint32_t v, psx::cpu::ExecutableWriteSource source) {
  writeGuestMemory(a, v, source);
}
void Core::mem_w8_unnotified(uint32_t a, uint8_t v) {
  writeGuestMemory<uint8_t, false>(a, v, psx::cpu::ExecutableWriteSource::MappedStore);
}
void Core::mem_w32_unnotified(uint32_t a, uint32_t v) {
  writeGuestMemory<uint32_t, false>(a, v, psx::cpu::ExecutableWriteSource::MappedStore);
}
void Core::mem_write(uint32_t a, std::span<const uint8_t> bytes, psx::cpu::ExecutableWriteSource source) {
  for (std::size_t i = 0; i < bytes.size(); i++) {
    mem_w8_unnotified(a + static_cast<uint32_t>(i), bytes[i]);
  }
  psx::cpu::notifyExecutableWrite(*this, {a, a + static_cast<uint32_t>(bytes.size())}, source);
}
