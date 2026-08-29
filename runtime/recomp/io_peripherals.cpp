// Address decode for the controller port and the root counters. See io_peripherals.h for why this
// is not in mem.cpp; the register CONTRACTS are in sio_pad.h and timing.h.
#include "io_peripherals.h"
#include "core.h"
#include "game.h"
#include <lucent/log.h>

namespace {

// Root counter 1 is HBlank-clocked and derived, so it has a value but no writable state here; root
// counter 2 is a full value/mode/target trio. Anything else in the timer block is unmapped.
constexpr uint32_t kSio0Lo = 0x1F801040u, kSio0Hi = 0x1F80104Fu;
constexpr uint32_t kCounter1Value = 0x1F801110u;
constexpr uint32_t kCounter2Lo = 0x1F801120u, kCounter2Hi = 0x1F80112Bu;
constexpr uint32_t kIrqStat = 0x1F801070u, kIrqMask = 0x1F801074u;

bool in(uint32_t p, uint32_t lo, uint32_t hi) {
  return p >= lo && p <= hi;
}

} // namespace

bool io_peripheral_read(Core &core, uint32_t addr, uint32_t &out) {
  Game &game = *core.game;
  const uint32_t p = addr & 0x1FFFFFFFu;
  if (in(p, kSio0Lo, kSio0Hi)) {
    switch (p & 0xEu) {
    case 0x0:
      out = game.sio.dataRead(game.timing.emulatedCpuTicks());
      return true; // JOY_RX_DATA
    case 0x4:
      out = game.sio.status(game.timing.emulatedCpuTicks());
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
    // `PSXPORT_DEBUG=irq` — the interrupt controller's whole traffic. Worth a channel because both
    // of the questions this subsystem raises are invisible otherwise: whether a guest VERIFIER is
    // even reaching I_STAT (before this model existed the read fell through to unmapped I/O and
    // returned 0, so every verifier rejected and nothing said so), and whether a bit the framework
    // asserted was ever acknowledged. `ra` names the verifier or handler doing the read.
    lucent::debug("irq",
                  "r {} = 0x{:03X} (stat=0x{:03X} mask=0x{:03X}) ra={:08X}",
                  p == kIrqStat ? "I_STAT" : "I_MASK",
                  rv,
                  game.hle.i_stat,
                  game.hle.i_mask,
                  core.r[31]);
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

bool io_peripheral_write(Core &core, uint32_t addr, uint32_t value) {
  Game &game = *core.game;
  const uint32_t p = addr & 0x1FFFFFFFu;
  if (in(p, kSio0Lo, kSio0Hi)) {
    switch (p & 0xEu) {
    case 0x0: // JOY_TX_DATA: the guest clocked a command byte out to the controller
      game.sio.dataWrite(value, game.pad.buttons, game.timing.emulatedCpuTicks());
      return true;
    case 0x8:
      game.sio.mode = static_cast<uint16_t>(value & 0x013Fu);
      return true;
    case 0xA:
      game.sio.ctrlWrite((uint16_t)value);
      lucent::debug("sio", "w CTRL {:04X} (raw {:04X}) ra={:08X}", game.sio.ctrl, value & 0xFFFFu, core.r[31]);
      return true;
    case 0xE:
      game.sio.baud = static_cast<uint16_t>(value);
      return true;
    default:
      return true;
    }
  }
  if (p == kIrqStat) {                       // I_STAT: acknowledge. A bit written as 0 is
    core.irqStatLatch();                     // cleared; a bit written as 1 is left alone. This
    const uint32_t before = game.hle.i_stat; // is the PSX's semantic, NOT write-1-to-clear.
    game.hle.i_stat &= value & 0x7FFu;
    lucent::debug("irq",
                  "w I_STAT 0x{:03X}: 0x{:03X} -> 0x{:03X} ra={:08X}",
                  value & 0x7FFu,
                  before,
                  game.hle.i_stat,
                  core.r[31]);
    return true;
  }
  if (p == kIrqMask) { // I_MASK
    game.hle.i_mask = value & 0x7FFu;
    core.pending_work |= Core::PW_IRQ; // unmasking may have made a latched bit deliverable
    lucent::debug("irq", "w I_MASK 0x{:03X} ra={:08X}", game.hle.i_mask, core.r[31]);
    return true;
  }
  if (in(p, kCounter2Lo, kCounter2Hi)) {
    game.timing.rootCounter2Write(p, value);
    return true;
  }
  // Root counter 1 is derived from display time and has no writable state: a write is not this
  // unit's, so it falls through to mem.cpp's unmapped-peripheral report rather than being swallowed.
  return false;
}

// Fold any interrupt edge the CD controller has raised since the last look into I_STAT, then return
// it. Bit 2 is EDGE-triggered on real hardware: the guest acks the CD controller at 0x1F801803 and
// acks I_STAT separately by writing a 0 to the bit, so deriving the bit LEVEL-style from "is the
// response queue non-empty" would be wrong in both directions — it would re-assert after an I_STAT
// ack and drop while a response is still pending. Called from every I_STAT read and write so the
// latch cannot be missed regardless of which the guest does first.
uint32_t Core::irqStatLatch() {
  // The controller port's /ACK is a deadline, not an instant edge (sio_pad.cpp): fold it in here so a
  // driver polling I_STAT sees it at the same place the CD edge appears.
  game->sio.service(game->timing.emulatedCpuTicks());
  if (game->cdc.irq_edge) {
    game->cdc.irq_edge = 0;
    game->hle.i_stat |= 1u << 2;
    pending_work |= PW_IRQ; // arm the per-function-entry delivery gate
    lucent::debug("irq",
                  "CD raised IRQ2 -> I_STAT=0x{:03X} (mask=0x{:03X}, {})",
                  game->hle.i_stat,
                  game->hle.i_mask,
                  (game->hle.i_mask & 4) ? "ENABLED" : "masked off by the guest");
  }
  return game->hle.i_stat;
}
