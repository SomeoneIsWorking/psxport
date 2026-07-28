// Native CD controller (CDROM registers 0x1F801800-0x1F801803).
//
// The boot stub's SCEA screen (and parts of libcd) drive the CD controller at the REGISTER level:
// it selects a register bank via 0x1F801800, pushes parameters to the param FIFO, writes a command,
// then polls the Interrupt Flag register (0x1F801803 bank1, low 3 bits = response type) for the
// controller's response. With no controller emulated those reads return 0 and the stub's SCEA state
// machine (0x800123B0) loops forever. This is a focused, faithful model of the CXD1199-style
// register interface: index banking, parameter/response/data FIFOs, and a queue of pending
// interrupts (commands that return INT3-ack-then-INT2-complete). Data reads are served from the
// disc image (disc.c). We complete commands SYNCHRONOUSLY (the response is ready on the next poll),
// which is correct for code that busy-polls the flag without advancing time.
//
// Register map (bank = 0x1F801800 & 3):
//   0x1F801800  W: index/bank (low 2 bits)      R: status (FIFO/busy bits + index)
//   0x1F801801  W bank0: command                R: response FIFO (pop)
//   0x1F801802  W bank0: parameter FIFO (push)   R: data FIFO (pop)
//   0x1F801803  W bank0: request (BFRD want-data) W bank1: ack/reset IRQ flags
//               R bank0/2: interrupt enable      R bank1/3: interrupt flag (pending IRQ type)
#include "r3000.h"
#include "cfg.h"
#include "cdc_state.h"
#include "disc.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// PER-INSTANCE CD-controller state: the register/FIFO/IRQ-queue model lives on a CdcState (one per
// Game, game.h) passed EXPLICITLY to every entry point — no bound "current" pointer.

void cdc_state_init(CdcState* s) {
  struct DiscState* disc = s->disc;   // preserve wiring across re-init
  memset(s, 0, sizeof *s);
  s->stat = 0x02;                     // power-on defaults
  s->disc = disc;
  s->verbose = cfg_dbg("cdc") ? 1 : 0;
}

static void cdc_irq(CdcState* s, uint8_t type, const uint8_t* resp, int len) {
  int n = (s->q_tail + 1) & 7;
  if (n == s->q_head) return;            // queue full (shouldn't happen)
  s->q[s->q_tail].type = type;
  s->q[s->q_tail].len = len;
  if (len) memcpy(s->q[s->q_tail].resp, resp, (size_t)len);
  if (s->q_tail == s->q_head) s->resp_rd = 0;   // first entry becomes active
  s->q_tail = n;
  s->irq_edge = 1;                              // -> I_STAT bit 2, latched by the MMIO dispatcher
}
static int q_empty(CdcState* s) { return s->q_head == s->q_tail; }

static uint32_t lba_from_param(CdcState* s) {  // Setloc params: amm,ass,asect (BCD), minus the 2s lead-in
  const uint8_t* p = s->param;
  int mm = (p[0] >> 4) * 10 + (p[0] & 0xF);
  int ss = (p[1] >> 4) * 10 + (p[1] & 0xF);
  int ff = (p[2] >> 4) * 10 + (p[2] & 0xF);
  return (uint32_t)((mm * 60 + ss) * 75 + ff - 150);
}

static void load_sector(CdcState* s) {    // fill the data FIFO with the sector at loc_lba
  uint8_t sec[2048];
  if (!disc_read_sector(s->disc, s->loc_lba, sec)) {
    // Leave the FIFO EMPTY rather than serving zeros. A read that "succeeds" with zeroed data is
    // indistinguishable from a real one to the guest and corrupts arbitrarily far downstream; an
    // empty FIFO stalls the guest's fetch visibly, right here, with this line in the log.
    cfg_loge("cdc", "ReadN: no data for LBA %u (no disc, or out of range) — data FIFO left EMPTY",
             s->loc_lba);
    s->data_n = 0; s->data_rd = 0; return;
  }
  memcpy(s->data, sec, 2048); s->data_n = 2048; s->data_rd = 0;
}

// A whole sector has been consumed (drained by DMA3 or popped by the CPU) during a continuous
// ReadN/ReadS. Real hardware delivers the NEXT sector as a fresh INT1 one sector-time later; this
// model is synchronous, so it is ready immediately: advance the head and announce INT1.
//
// Without this, exec_command queued exactly ONE INT1 per ReadN and never advanced loc_lba, so a
// multi-sector read received sector N forever — the guest waits for a second sector that is never
// announced. Nothing is raised when the drive is not reading (Pause/Stop cleared s->reading), so no
// event is fabricated.
static void sector_consumed(CdcState* s) {
  s->data_n = 0; s->data_rd = 0;
  if (!s->reading) return;
  s->loc_lba++;
  load_sector(s);
  { uint8_t r1[1]; r1[0] = s->stat; cdc_irq(s, 1, r1, 1); }   // INT1: next sector data-ready
}

// DMA3 (CDROM -> RAM): pop up to `words` 32-bit words from the sector data FIFO. Returns the count
// actually delivered; a SHORT return means the FIFO ran dry and the caller must say so loudly — a
// transfer reported complete that moved nothing is exactly the silent lie this layer must not tell.
int cdc_dma_read(CdcState* s, uint32_t* out, int words) {
  int got = 0;
  while (got < words && s->data_rd + 4 <= s->data_n) {
    const uint8_t* p = s->data + s->data_rd;
    out[got++] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    s->data_rd += 4;
  }
  if (got && s->data_rd >= s->data_n) sector_consumed(s);
  return got;
}

static void exec_command(CdcState* s, uint8_t cmd) {
  if (s->verbose || cfg_dbg("cdc"))
    cfg_logi("cdc", "cmd 0x%02X params=%d [%02X %02X %02X]", cmd, s->param_n,
            s->param[0], s->param[1], s->param[2]);
  uint8_t r1[1] = { s->stat };
  switch (cmd) {
    case 0x01: cdc_irq(s, 3, r1, 1); break;                          // Getstat
    case 0x02: s->loc_lba = lba_from_param(s); cdc_irq(s, 3, r1, 1); break;  // Setloc
    case 0x0E: s->mode = s->param[0]; cdc_irq(s, 3, r1, 1); break;   // Setmode
    case 0x06: case 0x1B:                                         // ReadN / ReadS
      s->reading = 1; cdc_irq(s, 3, r1, 1);
      load_sector(s); { uint8_t s2[1] = { s->stat }; cdc_irq(s, 1, s2, 1); }  // INT1 data-ready
      break;
    case 0x09: s->reading = 0; cdc_irq(s, 3, r1, 1); cdc_irq(s, 2, r1, 1); break;  // Pause
    case 0x08: s->reading = 0; cdc_irq(s, 3, r1, 1); cdc_irq(s, 2, r1, 1); break;  // Stop
    case 0x0A: s->stat = 0x02; s->mode = 0; cdc_irq(s, 3, r1, 1); cdc_irq(s, 2, r1, 1); break;  // Init
    case 0x0B: case 0x0C: cdc_irq(s, 3, r1, 1); break;              // Mute / Demute
    case 0x15: case 0x16:                                        // SeekL / SeekP
      cdc_irq(s, 3, r1, 1); cdc_irq(s, 2, r1, 1); break;
    case 0x1E:                                                   // ReadTOC
      cdc_irq(s, 3, r1, 1); cdc_irq(s, 2, r1, 1); break;
    case 0x07: case 0x0D: case 0x03: case 0x17: case 0x18:       // MotorOn/SetFilter/Play/SetSession
      cdc_irq(s, 3, r1, 1); cdc_irq(s, 2, r1, 1); break;
    case 0x19: {                                                 // Test (sub-function in param[0])
      if (s->param[0] == 0x20) { uint8_t v[4] = { 0x94, 0x09, 0x19, 0xC0 };  // BIOS date/version
        cdc_irq(s, 3, v, 4); } else cdc_irq(s, 3, r1, 1);
      break; }
    case 0x1A: {                                                 // GetID -> region/license
      cdc_irq(s, 3, r1, 1);
      uint8_t id[8] = { 0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A' };  // licensed, America
      cdc_irq(s, 2, id, 8); break; }
    case 0x13: { uint8_t t[3] = { s->stat, 0x01, 0x01 }; cdc_irq(s, 3, t, 3); break; }  // GetTN
    case 0x14: { uint8_t t[3] = { s->stat, 0x00, 0x02 }; cdc_irq(s, 3, t, 3); break; }  // GetTD
    default:
      if (s->verbose) cfg_logi("cdc", "UNHANDLED cmd 0x%02X -> ack only", cmd);
      cdc_irq(s, 3, r1, 1); break;
  }
  s->param_n = 0;                                                // command consumes the param FIFO
}

uint32_t cdc_read(CdcState* s, uint32_t p) {
  switch (p & 3) {
    case 0: {  // status register
      uint8_t st = (uint8_t)s->index;
      if (s->param_n == 0) st |= 0x08;             // PRMEMPT (param FIFO empty)
      if (s->param_n < 16) st |= 0x10;             // PRMWRDY (param FIFO not full)
      if (!q_empty(s) && s->resp_rd < s->q[s->q_head].len) st |= 0x20;  // RSLRRDY (response ready)
      if (s->data_rd < s->data_n) st |= 0x40;      // DRQSTS (data FIFO not empty)
      return st;
    }
    case 1: {  // response FIFO
      if (q_empty(s)) return 0;
      CdcIrqEnt* f = &s->q[s->q_head];
      return s->resp_rd < f->len ? f->resp[s->resp_rd++] : 0;
    }
    case 2: {  // data FIFO — CPU pop path; must advance the head exactly as DMA3 does
      if (s->data_rd >= s->data_n) return 0;
      const uint8_t b = s->data[s->data_rd++];
      if (s->data_rd >= s->data_n) sector_consumed(s);
      return b;
    }
    case 3:    // bank0/2: interrupt enable; bank1/3: interrupt flag (low 3 bits = pending type)
      if (s->index & 1) return q_empty(s) ? 0xE0 : (uint8_t)(0xE0 | s->q[s->q_head].type);
      return s->irq_en | 0xE0;
  }
  return 0;
}

void cdc_write(CdcState* s, uint32_t p, uint8_t v) {
  switch (p & 3) {
    case 0: s->index = v & 3; return;              // index/bank select
    case 1:
      if (s->index == 0) exec_command(s, v);       // command register
      return;
    case 2:
      if (s->index == 0) { if (s->param_n < 16) s->param[s->param_n++] = v; }  // param FIFO push
      else if (s->index == 1) s->irq_en = v;       // interrupt enable
      return;
    case 3:
      if (s->index == 1) {                         // interrupt flag: write 1s to ack/clear
        if (v & 0x07) {                            // ack current IRQ -> advance the queue
          if (!q_empty(s)) {
            s->q_head = (s->q_head + 1) & 7; s->resp_rd = 0;
            // A response that was QUEUED behind the one just acked becomes current now, and that is
            // a fresh interrupt on real hardware — not a continuation of the acked one. Without this
            // edge the second and later responses of a multi-INT command sequence would be visible
            // in the FIFO but never announced, which looks exactly like a dropped response.
            if (!q_empty(s)) s->irq_edge = 1;
          }
        }
        if (v & 0x40) s->param_n = 0;              // reset param FIFO
      } else if (s->index == 0) {                  // request register
        if (v & 0x80) { if (s->reading) load_sector(s); }  // BFRD: want sector data
      }
      return;
  }
}
