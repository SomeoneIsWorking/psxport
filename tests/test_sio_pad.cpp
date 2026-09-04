// SIO0 (controller port) hardware model — Sio0, reached through Core's shipping MMIO path.
//
// The model exists because a guest can drive the port itself instead of calling the BIOS: Crash
// Bash patches its own pad engine into the kernel C0 table and does the whole handshake inside the
// interrupt element it registers. What that driver observes is asserted here directly: the response
// sequence, WHICH bytes are acknowledged, and — the part a "responds instantly" model gets wrong —
// that the /ACK is a DEADLINE, late enough that a driver which clears I_STAT bit 7 after sending a
// byte still sees that byte's ack afterwards.
#include "../runtime/psx/game.h"
#include "testutil.h"

namespace {

// The two register-level facts every case needs: a selected port with the /ACK interrupt enabled,
// and the baud/mode the measured driver programs (reload 0x88, MUL1 → 8 * 0x88 = 1088 ticks/byte).
constexpr uint16_t kCtrlSelected = 0x1003; // TXEN | select | ACK-interrupt-enable
constexpr uint16_t kMode = 0x000D;
constexpr uint16_t kBaud = 0x0088;
constexpr uint64_t kByteTicks = 8u * kBaud;
constexpr uint32_t kData = 0x1F801040u;
constexpr uint32_t kStatus = 0x1F801044u;
constexpr uint32_t kModeReg = 0x1F801048u;
constexpr uint32_t kCtrl = 0x1F80104Au;
constexpr uint32_t kBaudReg = 0x1F80104Eu;
constexpr uint32_t kIStat = 0x1F801070u;
constexpr uint32_t kIMask = 0x1F801074u;

// A port needs a Game only because the /ACK raises I_STAT, which the interrupt controller owns.
struct Port {
  Game *owner = new Game();
  Hle &hle = owner->hle;

  Port() {
    write(kModeReg, kMode, 2);
    write(kBaudReg, kBaud, 2);
    write(kCtrl, kCtrlSelected, 2);
  }
  ~Port() {
    delete owner;
  }
  uint32_t read(uint32_t addr, uint32_t bytes) {
    switch (bytes) {
    case 1:
      return owner->core.mem_r8(addr);
    case 2:
      return owner->core.mem_r16(addr);
    default:
      return owner->core.mem_r32(addr);
    }
  }
  void write(uint32_t addr, uint32_t value, uint32_t bytes) {
    switch (bytes) {
    case 1:
      owner->core.mem_w8(addr, static_cast<uint8_t>(value));
      return;
    case 2:
      owner->core.mem_w16(addr, static_cast<uint16_t>(value));
      return;
    default:
      owner->core.mem_w32(addr, value);
      return;
    }
  }
  void burn(uint64_t ticks) {
    while (ticks != 0) {
      const uint32_t chunk = ticks > 0xFFFFu ? 0xFFFFu : static_cast<uint32_t>(ticks);
      owner->timing.advanceGuestInstructionTicks(chunk);
      ticks -= chunk;
    }
  }
  void clearSioIrq() {
    write(kIStat, 0x77Fu, 4);
    write(kCtrl, read(kCtrl, 2) | 0x0010u, 2);
  }
  // Clock one byte out and settle past the whole transfer + ack, the way a driver's own delays do.
  uint32_t exchange(uint32_t tx, uint16_t buttons) {
    owner->pad.buttons = buttons;
    write(kData, tx, 1);
    burn(kByteTicks * 4); // well past both deadlines
    return read(kData, 1);
  }
};

// The full digital-pad exchange, byte for byte, with a button pressed. 0xFFF7 is START held
// (active-low bit 3) — the mask pad_input.cpp produces for the Start button.
void test_digital_pad_response_sequence() {
  Port p;
  CHECK_EQ(p.exchange(0x01, 0xFFF7u), 0xFFu); // address the controller: bus is hi-Z
  CHECK_EQ(p.exchange(0x42, 0xFFF7u), 0x41u); // read command -> device id (digital pad)
  CHECK_EQ(p.exchange(0x00, 0xFFF7u), 0x5Au); // data ready
  CHECK_EQ(p.exchange(0x00, 0xFFF7u), 0xF7u); // buttons lo — START pressed
  CHECK_EQ(p.exchange(0x00, 0xFFF7u), 0xFFu); // buttons hi
}

// Idle is 0xFFFF because the pad is active-low: nothing pressed means every bit set.
void test_idle_buttons_are_all_ones() {
  Port p;
  p.exchange(0x01, 0xFFFFu);
  p.exchange(0x42, 0xFFFFu);
  p.exchange(0x00, 0xFFFFu);
  CHECK_EQ(p.exchange(0x00, 0xFFFFu), 0xFFu);
  CHECK_EQ(p.exchange(0x00, 0xFFFFu), 0xFFu);
}

// Nothing answers an address that is not the controller's, and no ACK is raised for it. This is the
// negative the model must be able to give: absent hardware, not a pad that answers everything.
void test_unaddressed_device_never_acknowledges() {
  Port p;
  p.owner->pad.buttons = 0xFFFFu;
  p.write(kData, 0x81, 1); // memory card address — not modelled
  p.burn(kByteTicks * 4);
  CHECK_EQ(p.read(kData, 1), 0xFFu);
  CHECK_EQ(p.read(kIStat, 4) & 0x80u, 0u);
  CHECK_EQ(p.read(kStatus, 4) & 0x200u, 0u);
  // Same for a command the digital pad does not implement, after a valid address.
  Port q;
  q.exchange(0x01, 0xFFFFu);
  q.clearSioIrq();
  q.write(kData, 0x43, 1); // config-mode entry: pad ID was already shifting, then no continuation
  q.burn(kByteTicks * 4);
  CHECK_EQ(q.read(kData, 1), 0x41u);
  CHECK_EQ(q.read(kIStat, 4) & 0x80u, 0u);
}

// With the port deselected (CTRL bit 1 clear) the bus is dead — writing DATA reaches nothing.
void test_deselected_port_has_no_device() {
  Port p;
  p.write(kCtrl, 0x1001, 2); // TXEN + ACK int, but NOT select
  p.write(kData, 0x01, 1);
  p.burn(kByteTicks * 4);
  CHECK_EQ(p.read(kData, 1), 0xFFu);
  CHECK_EQ(p.read(kIStat, 4) & 0x80u, 0u);

  Port txDisabled;
  txDisabled.write(kCtrl, 0x1002, 2); // selected, but transmitter disabled
  txDisabled.write(kData, 0x01, 1);
  txDisabled.burn(kByteTicks * 4);
  CHECK_EQ(txDisabled.read(kIStat, 4) & 0x80u, 0u);

  Port secondPort;
  secondPort.write(kCtrl, 0x3003, 2); // Crash Bash selects port 2 this way; no pad is connected there
  secondPort.write(kData, 0x01, 1);
  secondPort.burn(kByteTicks * 4);
  CHECK_EQ(secondPort.read(kIStat, 4) & 0x80u, 0u);
}

void test_switching_ports_cancels_the_first_ports_pending_ack() {
  Port p;
  p.write(kData, 0x01, 1);
  p.write(kCtrl, 0x3003, 2); // move DTR from port 1 to the absent port 2 before /ACK arrives
  p.burn(kByteTicks * 4);
  CHECK_EQ(p.read(kData, 1), 0xFFu);
  CHECK_EQ(p.read(kIStat, 4) & 0x80u, 0u);
  CHECK_EQ(p.read(kStatus, 4) & 0x280u, 0u);
}

// The last byte of the response is deliberately NOT acknowledged: that absence is how a driver
// learns the transfer ended, so it has to be observable.
void test_last_response_byte_is_not_acknowledged() {
  Port p;
  const uint32_t tx[] = {0x01, 0x42, 0x00, 0x00, 0x00};
  int acked = 0;
  for (uint32_t byte : tx) {
    p.clearSioIrq();
    p.write(kData, byte, 1);
    p.burn(kByteTicks * 4);
    if (p.read(kIStat, 4) & 0x80u) {
      ++acked;
    }
  }
  CHECK_EQ(acked, 4); // 5 bytes clocked, 4 acknowledged — the count IS the claim
}

// THE ORDERING THE WHOLE MODEL EXISTS FOR. The measured driver sends a byte, waits a short fixed
// delay, CLEARS I_STAT bit 7, and only THEN waits for that byte's ack. An ack raised inside the
// DATA write is destroyed by the clear and the driver hangs on the next byte. So: nothing at the
// write, still nothing after the driver's own ~14us pre-delay, and the edge afterwards.
void test_ack_arrives_after_the_drivers_pre_delay_clear() {
  Port p;
  p.hle.i_stat = 0;
  p.write(kData, 0x01, 1);
  CHECK_EQ(p.read(kIStat, 4) & 0x80u, 0u); // not instant

  constexpr uint64_t kDriverPreDelayTicks = 0x3Cu * 8u; // 0x3C counter-2 units, guest 0x8003C688
  p.burn(kDriverPreDelayTicks);
  CHECK_EQ(p.read(kIStat, 4) & 0x80u, 0u); // still pending when the driver clears the bit
  p.write(kIStat, 0x77Fu, 4);              // the driver's `*I_STAT = 0xFFFFFF7F`

  constexpr uint64_t kDriverTimeoutTicks = 0x1AEu * 8u; // its ack timeout, guest 0x8003B7D8
  p.burn(kDriverTimeoutTicks);
  CHECK_EQ(p.read(kIStat, 4) & 0x80u, 0x80u); // and inside the timeout, not after it
}

// The measured handler waits for JOY_STAT bit 7 to return low before writing CTRL bit 4. Prove the
// pulse exists, ends within the same timeout budget, and leaves the interrupt request latched until
// the handler acknowledges it.
void test_ack_level_pulses_before_the_driver_acknowledges_it() {
  Port p;
  p.write(kData, 0x01, 1);
  constexpr uint64_t kDriverTimeoutTicks = 0x1AEu * 8u;
  bool sawPulse = false;
  uint64_t waited = 0;
  while (waited < kDriverTimeoutTicks) {
    if (p.read(kStatus, 4) & 0x80u) {
      sawPulse = true;
      break;
    }
    p.burn(1);
    ++waited;
  }
  CHECK(sawPulse);
  CHECK(waited < kDriverTimeoutTicks);
  CHECK_EQ(p.read(kStatus, 4) & 0x200u, 0x200u);
  while ((p.read(kStatus, 4) & 0x80u) != 0 && waited < kDriverTimeoutTicks) {
    p.burn(1);
    ++waited;
  }
  CHECK_EQ(p.read(kStatus, 4) & 0x80u, 0u);
  p.write(kCtrl, kCtrlSelected | 0x0010u, 2);
  CHECK_EQ(p.read(kStatus, 4) & 0x200u, 0u);
}

// The ACK is a future event. An early interrupt poll may clear an unrelated gate, but crossing the
// device deadline without polling MMIO must latch I_STAT and re-arm delivery at the actual edge.
void test_due_ack_rearms_delivery_after_an_early_poll() {
  Port p;
  p.write(kIMask, 0x80u, 4);
  p.write(kData, 0x01, 1);
  p.owner->core.pending_work |= Core::PW_IRQ;
  p.owner->hle.irqPoll(&p.owner->core);
  CHECK_EQ(p.owner->core.pending_work & Core::PW_IRQ, 0u);
  p.burn(kByteTicks * 4);
  CHECK_EQ(p.read(kIStat, 4) & 0x80u, 0x80u);
  CHECK_EQ(p.owner->core.pending_work & Core::PW_IRQ, (uint32_t)Core::PW_IRQ);
}

// The RX byte has its own, earlier deadline: a driver that waits on JOY_STAT bit 1 must actually
// wait. Reading early reads an empty FIFO.
void test_rx_fifo_fills_only_when_the_byte_has_shifted() {
  Port p;
  p.write(kData, 0x01, 1);
  CHECK_EQ(p.read(kStatus, 4) & 0x02u, 0u);
  p.burn(kByteTicks - 1);
  CHECK_EQ(p.read(kData, 1), 0xFFu);
  p.burn(1);
  CHECK_EQ(p.read(kStatus, 4) & 0x02u, 0x02u);
  CHECK_EQ(p.read(kData, 1), 0xFFu);        // the address byte's response IS 0xFF
  CHECK_EQ(p.read(kStatus, 4) & 0x02u, 0u); // popped: FIFO empty again
}

// JOY_STAT bit 9 latches an enabled ACK interrupt request, and CTRL bit 4 acknowledges it. Bits 4
// and 6 are write-only strobes and must not read back — a driver that does a read-modify-write to
// raise bit 4 would otherwise acknowledge an interrupt it never saw.
void test_status_latch_and_ctrl_strobes() {
  Port p;
  p.write(kData, 0x01, 1);
  const uint64_t settled = kByteTicks * 4;
  p.burn(settled);
  CHECK_EQ(p.read(kStatus, 4) & 0x200u, 0x200u);
  p.write(kCtrl, kCtrlSelected | 0x0010u, 2); // acknowledge
  CHECK_EQ(p.read(kCtrl, 2) & 0x0010u, 0u);
  CHECK_EQ(p.read(kStatus, 4) & 0x200u, 0u);

  p.write(kCtrl, kCtrlSelected | 0x0040u, 2); // reset
  CHECK_EQ(p.read(kCtrl, 2), 0u);
  CHECK_EQ(p.read(kBaudReg, 2), 0u);
}

void test_reset_cancels_an_in_flight_receive() {
  Port p;
  p.write(kData, 0x01, 1);
  p.write(kCtrl, kCtrlSelected | 0x0040u, 2);
  CHECK_EQ(p.read(kStatus, 4), 0x01u);
  p.burn(kByteTicks * 4);
  CHECK_EQ(p.read(kData, 1), 0xFFu);
  CHECK_EQ(p.read(kStatus, 4), 0x01u);
}

// CTRL bit 12 gates the SIO interrupt request itself: with it disabled, neither I_STAT bit 7 nor
// JOY_STAT bit 9 may latch. The physical JOY_STAT bit-7 pulse remains a separate device signal.
void test_ack_interrupt_enable_gates_only_i_stat() {
  Port p;
  p.write(kCtrl, 0x0003, 2); // selected, ACK interrupt DISABLED
  p.write(kData, 0x01, 1);
  const uint64_t settled = kByteTicks * 4;
  p.burn(settled);
  CHECK_EQ(p.read(kIStat, 4) & 0x80u, 0u);
  CHECK_EQ(p.read(kStatus, 4) & 0x200u, 0u);
}

// The transfer deadline comes from the guest's own JOY_BAUD/JOY_MODE, not from a constant of ours:
// the memory-card twin programs reload 0x22 and must shift four times faster.
void test_transfer_time_follows_the_programmed_baud() {
  Port p;
  p.write(kBaudReg, 0x0022, 2);
  p.write(kData, 0x01, 1);
  p.burn(8u * 0x22u - 1u);
  CHECK_EQ(p.read(kStatus, 4) & 0x02u, 0u);
  p.burn(1);
  CHECK_EQ(p.read(kStatus, 4) & 0x02u, 0x02u);
}

} // namespace

int main() {
  RUN(digital_pad_response_sequence);
  RUN(idle_buttons_are_all_ones);
  RUN(unaddressed_device_never_acknowledges);
  RUN(deselected_port_has_no_device);
  RUN(switching_ports_cancels_the_first_ports_pending_ack);
  RUN(last_response_byte_is_not_acknowledged);
  RUN(ack_arrives_after_the_drivers_pre_delay_clear);
  RUN(ack_level_pulses_before_the_driver_acknowledges_it);
  RUN(due_ack_rearms_delivery_after_an_early_poll);
  RUN(rx_fifo_fills_only_when_the_byte_has_shifted);
  RUN(status_latch_and_ctrl_strobes);
  RUN(reset_cancels_an_in_flight_receive);
  RUN(ack_interrupt_enable_gates_only_i_stat);
  RUN(transfer_time_follows_the_programmed_baud);
  return pt_summary();
}
