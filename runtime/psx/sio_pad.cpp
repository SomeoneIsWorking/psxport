// The controller port and its device. Contract, and why every deadline here exists, is sio_pad.h.
#include "sio_pad.h"
#include "game.h"
#include <lucent/log.h>

// TIMING. A byte is shifted at the programmed baud rate and the device answers with an /ACK pulse
// some microseconds later; both are what a driver actually waits on, so both are deadlines here in
// the same emulated CPU ticks the rest of the runtime measures.
//
//   shift time  = 8 bits x the guest's own JOY_BAUD reload x the JOY_MODE baud factor. Derived from
//                 the registers the guest wrote, never a constant of ours: Crash Bash programs
//                 reload 0x88 with MUL1, i.e. ~32us, and its memory-card twin programs 0x22.
//   ack delay   = 64 CPU clocks after the final bit, followed by a 32-clock active pulse. These are
//                 the digital-pad timing values used by the vendored Beetle oracle (frontio.c).
//
// The window is not free to choose. Crash Bash's driver (guest 0x8003B6E8 -> 0x8003BC78 ->
// 0x8003B7D8) sends a byte, waits 0x3C counter-2 units (~14us), CLEARS I_STAT bit 7, and only then
// waits up to 0x1AE units (~102us) for that byte's ack. So the ack must land later than ~14us and
// sooner than ~116us after the write, or the driver either loses the edge or times out. The oracle's
// transfer-plus-64-clock delay lands inside that independently measured window.
static constexpr uint64_t kAckDelayTicks = 64;
static constexpr uint64_t kAckPulseTicks = 32;

// Bit period in CPU ticks: JOY_BAUD reload scaled by the JOY_MODE baud-rate factor (bits 0-1).
static uint64_t transfer_ticks(uint16_t mode, uint16_t baud) {
  const uint64_t reload = baud ? baud : 1u;
  uint64_t factor = 1;
  switch (mode & 0x3u) {
  case 2:
    factor = 16;
    break;
  case 3:
    factor = 64;
    break;
  default:
    factor = 1;
    break; // 0 and 1 are both MUL1
  }
  const uint64_t bitTicks = reload * factor < 0x20u ? 0x20u : reload * factor;
  return 8u * bitTicks;
}

// One DATA write clocks one byte both ways. The sequence of RESPONSES is the standard digital-pad
// one; what a driver really keys on is which bytes are ACKNOWLEDGED, which is every byte of the
// response except its last.
void Sio0::dataWrite(uint32_t byte, uint16_t buttons, uint64_t now) {
  const uint32_t tx = byte & 0xFFu;
  int resp = 0xFF; // an unaddressed / absent device leaves the bus floating high
  bool ack = false;
  // CTRL bit 0 enables transmit, bit 1 selects the device, and bit 13 selects the second physical
  // port. This model owns one controller on port 1; a disabled transmitter or port 2 is silent.
  if ((ctrl & 0x2003u) == 0x0003u) {
    if (pos < 0) {
      if (tx == 0x01u) { // address 0x01: the controller. 0x81 is a memory card — not modelled here
        pos = 0;
        ack = true;
      }
    } else {
      const int step = pos++;
      switch (step) {
      case 0:
        // The pad ID is shifted out WHILE the command byte is shifted in. Whether the command is
        // accepted controls the following ACK/continuation, not the response already on the wire.
        resp = 0x41;       // device id: digital pad, one halfword of data
        if (tx == 0x42u) { // read buttons; a digital pad answers no other command
          ack = true;
        } else {
          pos = -1;
        }
        break;
      case 1:
        resp = 0x5A; // data follows
        ack = true;
        break;
      case 2:
        resp = buttons & 0xFFu;
        ack = true;
        break;
      case 3:
        resp = (buttons >> 8) & 0xFFu;
        pos = -1; // last byte of the response: deliberately NOT acknowledged
        break;
      default:
        pos = -1;
        break;
      }
    }
  }
  rx = resp;
  rxReadyTicks = now + transfer_ticks(mode, baud);
  ackTicks = ack ? rxReadyTicks + kAckDelayTicks : 0;
  lucent::debug("sio",
                "tx {:02X} -> rx {:02X} {} (pos {}, ctrl {:04X}, +{} ticks)",
                tx,
                (uint32_t)resp,
                ack ? "ACK" : "no-ack",
                pos,
                ctrl,
                rxReadyTicks - now);
}

void Sio0::service(uint64_t now) {
  if (ackTicks == 0 || now < ackTicks) {
    return;
  }
  const uint64_t arrived = ackTicks;
  ackTicks = 0;
  ackPulseEndTicks = arrived + kAckPulseTicks;
  if (ctrl & 0x1000u) { // CTRL bit 12: /ACK interrupt enable
    irq = true;
    game->hle.i_stat |= 0x80u;
    game->core.pending_work |= Core::PW_IRQ;
  }
  lucent::debug("sio", "/ACK{}", (ctrl & 0x1000u) ? " -> JOY_STAT#9 + I_STAT#7" : " (interrupt disabled)");
}

uint32_t Sio0::dataRead(uint64_t now) {
  service(now);
  if (rx < 0 || now < rxReadyTicks) {
    return 0xFFu; // the RX FIFO is empty: the bus reads as floating
  }
  const uint32_t rv = (uint32_t)rx;
  rx = -1;
  return rv;
}

void Sio0::ctrlWrite(uint16_t v) {
  const uint16_t before = ctrl;
  // Bits 4 (acknowledge) and 6 (reset) are write-only strobes. They must not read back set: this
  // title's driver does a read-modify-write to raise the acknowledge bit, and a sticky 0x10 would
  // make it acknowledge an interrupt it never saw.
  ctrl = v & 0x3F2Fu & static_cast<uint16_t>(~0x0050u);
  if (v & 0x0040u) { // reset: registers and the transfer in progress
    mode = 0;
    ctrl = 0;
    baud = 0;
    rx = -1;
    rxReadyTicks = 0;
    pos = -1;
    irq = false;
    ackTicks = 0;
    ackPulseEndTicks = 0;
  }
  if (v & 0x0010u) { // acknowledge: clears the error bits and JOY_STAT bit 9
    irq = false;
  }
  if ((before & 0x2002u) != (ctrl & 0x2002u)) {
    // Dropping DTR or selecting the other physical port ends this device's exchange and cancels a
    // not-yet-arrived pulse, matching the oracle's per-port SetDTR transition.
    pos = -1;
    rx = -1;
    rxReadyTicks = 0;
    ackTicks = 0;
    ackPulseEndTicks = 0;
  }
}

uint32_t Sio0::status(uint64_t now) {
  service(now);
  // bit 0 TX ready: no pending/in-progress byte. bit 1 RX FIFO not empty. bit 7 is the active /ACK
  // input pulse; Crash Bash waits for it to return low before acknowledging bit 9 through CTRL bit 4.
  const bool rx_ready = rx >= 0 && now >= rxReadyTicks;
  const bool tx_finished = rxReadyTicks == 0 || now >= rxReadyTicks;
  const bool ack_active = ackPulseEndTicks != 0 && now < ackPulseEndTicks;
  return (tx_finished ? 0x01u : 0u) | (rx_ready ? 0x02u : 0u) | (ack_active ? 0x80u : 0u) | (irq ? 0x200u : 0u);
}
