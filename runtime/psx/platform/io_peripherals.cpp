#include "io_peripherals.h"

#include "core.h"
#include "game.h"
#include "guest_memory_map.h"
#include "irq_edge.h"
#include "r3000.h"

#include <lucent/log.h>

namespace {

// Root counter 1 is HBlank-clocked and derived: its value is writable, its mode and target are not
// modelled; root counter 2 is a full value/mode/target trio. Anything else in the timer block is unmapped.
constexpr uint32_t kSio0Lo = 0x1F801040u, kSio0Hi = 0x1F80104Fu;
constexpr uint32_t kCounter1Value = 0x1F801110u;
constexpr uint32_t kCounter2Lo = 0x1F801120u, kCounter2Hi = 0x1F80112Bu;
constexpr uint32_t kIrqStat = kIrqStatRegister, kIrqMask = kIrqMaskRegister;

bool in(uint32_t p, uint32_t lo, uint32_t hi) {
  return p >= lo && p <= hi;
}

} // namespace

bool psx::platform::readIoPeripheral(Core &core, uint32_t addr, uint32_t &out) {
  Game &game = *core.game;
  const uint32_t p = psx::physical(addr);
  if (in(p, kSio0Lo, kSio0Hi)) {
    switch (p & 0xEu) {
    case 0x0:
      out = game.sio.dataRead();
      return true; // JOY_RX_DATA
    case 0x4:
      out = game.sio.status();
      return true; // JOY_STAT
    case 0x8:
      out = game.sio.mode;
      return true; // JOY_MODE
    case 0xA:
      out = game.sio.ctrl;
      return true; // JOY_CTRL
    case 0xE:
      out = game.sio.baud;
      return true; // JOY_BAUD
    default:
      out = 0;
      return true;
    }
  }
  if (p == kIrqStat || p == kIrqMask) { // I_STAT / I_MASK — contract in hle.h
    const uint32_t rv = (p == kIrqStat) ? core.irqStatLatch() : game.hle.i_mask;
    lucent::debug("irq",
                  "r {} = 0x{:03X} (stat=0x{:03X} mask=0x{:03X}) ra={:08X}",
                  p == kIrqStat ? "I_STAT" : "I_MASK",
                  rv,
                  game.hle.i_stat,
                  game.hle.i_mask,
                  core.r[psx::cpu::kRa]);
    out = rv;
    return true;
  }
  if (p == kCounter1Value) {
    out = game.timing.hSyncCounter();
    return true;
  }
  if (in(p, kCounter2Lo, kCounter2Hi)) {
    switch (p & 0xCu) {
    case 0x0:
      out = game.timing.rootCounter2();
      return true;
    case 0x4:
      out = game.timing.rootCounter2Mode;
      return true;
    case 0x8:
      out = game.timing.rootCounter2Target;
      return true;
    default:
      out = 0;
      return true;
    }
  }
  return false;
}

bool psx::platform::writeIoPeripheral(Core &core, GuestStore store) {
  Game &game = *core.game;
  const uint32_t value = store.value;
  const uint32_t p = psx::physical(store.address);
  if (in(p, kSio0Lo, kSio0Hi)) {
    switch (p & 0xEu) {
    case 0x0: // JOY_TX_DATA: the guest clocked a command byte out to the controller
      game.sio.dataWrite(value);
      return true;
    case 0x8:
      game.sio.mode = static_cast<uint16_t>(value & 0x013Fu);
      return true;
    case 0xA:
      game.sio.ctrlWrite((uint16_t)value);
      lucent::debug("sio", "w CTRL {:04X} (raw {:04X}) ra={:08X}", game.sio.ctrl, value & 0xFFFFu, core.r[psx::cpu::kRa]);
      return true;
    case 0xE:
      game.sio.baud = static_cast<uint16_t>(value);
      return true;
    default:
      return true;
    }
  }
  if (p == kIrqStat) { // a bit written as 0 is acknowledged; 1 leaves it alone
    core.irqStatLatch();
    const uint32_t before = game.hle.i_stat;
    game.hle.i_stat &= value & 0x7FFu;
    lucent::debug("irq",
                  "w I_STAT 0x{:03X}: 0x{:03X} -> 0x{:03X} ra={:08X}",
                  value & 0x7FFu,
                  before,
                  game.hle.i_stat,
                  core.r[psx::cpu::kRa]);
    return true;
  }
  if (p == kIrqMask) { // I_MASK
    game.hle.i_mask = value & 0x7FFu;
    core.pending_work |= Core::PW_IRQ; // unmasking may have made a latched bit deliverable
    lucent::debug("irq", "w I_MASK 0x{:03X} ra={:08X}", game.hle.i_mask, core.r[psx::cpu::kRa]);
    return true;
  }
  if (in(p, kCounter2Lo, kCounter2Hi)) {
    switch (p & 0xCu) {
    case 0x0:
      game.timing.rootCounter2WriteValue(static_cast<uint16_t>(value));
      return true;
    case 0x4:
      game.timing.rootCounter2WriteMode(value);
      return true;
    case 0x8:
      game.timing.rootCounter2WriteTarget(value);
      return true;
    default:
      return true;
    }
  }
  if (p == kCounter1Value) {
    game.timing.hSyncCounterWrite(static_cast<uint16_t>(value));
    return true;
  }
  // Root counter 1's mode and target are not modelled: the write falls through to mem.cpp's
  // unmapped-peripheral report rather than being swallowed.
  return false;
}
