// sio_pad.h — class Sio0 — the controller port (SIO0, 0x1F801040-0x1F80104F) and the device on it,
// owned by Game (`c->game->sio`, back-pointer wired in Game()). Register decode is io_peripherals.h;
// the behaviour is here.
//
// It is modelled as HARDWARE, not as one driver's expectations. Crash Bash is why it exists (see
// docs/findings/crashbash-pad-sio.md in that title): it patches its own pad engine into the kernel
// C0 table and drives SIO0 through a runtime pointer, so no BIOS pad entry and no per-VBlank buffer
// fill is ever involved. Before this model the registers were unmapped I/O returning 0, the
// driver's transfers never completed, and no button state reached guest RAM.
//
// What the guest sees: it selects the port (CTRL bit 1), clocks command bytes into DATA and, for
// each byte the device acknowledges, gets the /ACK interrupt — JOY_STAT bit 9 plus I_STAT bit 7
// when CTRL bit 12 enables it. Drivers poll either one; Crash Bash's polls I_STAT through its own
// 0x1F801070 pointer and acknowledges by writing bit 7 as 0.
//
// The device side is one digital controller in slot 1: address 0x01, command 0x42, then the standard
// 0xFF / 0x41 / 0x5A / buttons-lo / buttons-hi response, active-low, from Game::pad. The final byte
// is NOT acknowledged — that is how a driver learns the transfer ended. Nothing answers any other
// address (a memory card is 0x81) or any other command, so an unmodelled exchange reads back as a
// floating bus and a missing ACK, which is what absent hardware does.
//
// A transfer is NOT instantaneous, because the one driver measured cannot work if it is: it sends a
// byte, waits out a short fixed delay, CLEARS I_STAT bit 7, and only then waits for the /ACK of the
// byte it already sent. An ack raised inside the DATA write is destroyed by that clear and the next
// byte's wait times out. dataWrite() derives the two deadlines and names the window the guest's own
// delay and timeout leave for them.
#pragma once
#include <cstdint>
class Game;

class Sio0 {
public:
  Game *game = nullptr;
  uint16_t mode = 0;
  uint16_t ctrl = 0;
  uint16_t baud = 0;
  int rx = -1;                   // byte the device shifted back, -1 = RX FIFO empty
  int pos = -1;                  // bytes exchanged with the addressed device, -1 = none addressed
  bool irq = false;              // JOY_STAT bit 9: enabled /ACK interrupt request, acked by CTRL bit 4
  uint64_t rxReadyTicks = 0;     // when the byte finishes shifting into the RX FIFO
  uint64_t ackTicks = 0;         // when /ACK arrives, 0 = the device is not going to acknowledge
  uint64_t ackPulseEndTicks = 0; // when JOY_STAT bit 7 returns low after the /ACK pulse

  void dataWrite(uint32_t byte, uint16_t buttons, uint64_t now);
  uint32_t dataRead(uint64_t now);
  void ctrlWrite(uint16_t v);
  uint32_t status(uint64_t now);
  // Fold a due /ACK into JOY_STAT bit 9 + I_STAT bit 7. Called from every I_STAT and JOY_STAT
  // access, so no poll can miss the edge regardless of which register the driver watches.
  void service(uint64_t now);
};
