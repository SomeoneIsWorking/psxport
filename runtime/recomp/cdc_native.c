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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int disc_read_sector(uint32_t lba, uint8_t* out);

static int     s_verbose = -1;
static int     s_index;                 // 0x1F801800 low 2 bits (register bank)
static uint8_t s_param[16]; static int s_param_n;
static uint8_t s_data[2340]; static int s_data_n, s_data_rd;   // sector data FIFO
static uint8_t s_irq_en;                 // interrupt enable register
static uint8_t s_stat = 0x02;            // drive status byte (bit1 = motor on)
static uint32_t s_loc_lba;               // Setloc target (set by 0x02)
static uint8_t s_mode;                   // Setmode
static int     s_reading;                // ReadN/ReadS active

// Queue of pending interrupts. A command enqueues one (INT3 ack) or two (INT3 then INT2/INT1)
// responses; the front is the "active" interrupt whose type the flag register reports and whose
// bytes the response FIFO returns. Acking (write flag) pops to the next.
typedef struct { uint8_t type; uint8_t resp[16]; int len; } CdcIrq;
static CdcIrq s_q[8];
static int s_q_head, s_q_tail, s_resp_rd;

static void cdc_irq(uint8_t type, const uint8_t* resp, int len) {
  int n = (s_q_tail + 1) & 7;
  if (n == s_q_head) return;             // queue full (shouldn't happen)
  s_q[s_q_tail].type = type;
  s_q[s_q_tail].len = len;
  if (len) memcpy(s_q[s_q_tail].resp, resp, (size_t)len);
  if (s_q_tail == s_q_head) s_resp_rd = 0;   // first entry becomes active
  s_q_tail = n;
}
static int q_empty(void) { return s_q_head == s_q_tail; }

static uint32_t lba_from_param(void) {     // Setloc params: amm,ass,asect (BCD), minus the 2s lead-in
  int mm = (s_param[0] >> 4) * 10 + (s_param[0] & 0xF);
  int ss = (s_param[1] >> 4) * 10 + (s_param[1] & 0xF);
  int ff = (s_param[2] >> 4) * 10 + (s_param[2] & 0xF);
  return (uint32_t)((mm * 60 + ss) * 75 + ff - 150);
}

static void load_sector(void) {           // fill the data FIFO with the sector at s_loc_lba
  uint8_t sec[2048];
  if (!disc_read_sector(s_loc_lba, sec)) { s_data_n = 0; s_data_rd = 0; return; }
  memcpy(s_data, sec, 2048); s_data_n = 2048; s_data_rd = 0;
}

static void exec_command(uint8_t cmd) {
  if (s_verbose)
    fprintf(stderr, "[cdc] cmd 0x%02X params=%d [%02X %02X %02X]\n", cmd, s_param_n,
            s_param[0], s_param[1], s_param[2]);
  uint8_t r1[1] = { s_stat };
  switch (cmd) {
    case 0x01: cdc_irq(3, r1, 1); break;                          // Getstat
    case 0x02: s_loc_lba = lba_from_param(); cdc_irq(3, r1, 1); break;  // Setloc
    case 0x0E: s_mode = s_param[0]; cdc_irq(3, r1, 1); break;     // Setmode
    case 0x06: case 0x1B:                                         // ReadN / ReadS
      s_reading = 1; cdc_irq(3, r1, 1);
      load_sector(); { uint8_t s2[1] = { s_stat }; cdc_irq(1, s2, 1); }  // INT1 data-ready
      break;
    case 0x09: s_reading = 0; cdc_irq(3, r1, 1); cdc_irq(2, r1, 1); break;  // Pause
    case 0x08: s_reading = 0; cdc_irq(3, r1, 1); cdc_irq(2, r1, 1); break;  // Stop
    case 0x0A: s_stat = 0x02; s_mode = 0; cdc_irq(3, r1, 1); cdc_irq(2, r1, 1); break;  // Init
    case 0x0B: case 0x0C: cdc_irq(3, r1, 1); break;              // Mute / Demute
    case 0x15: case 0x16:                                        // SeekL / SeekP
      cdc_irq(3, r1, 1); cdc_irq(2, r1, 1); break;
    case 0x1E:                                                   // ReadTOC
      cdc_irq(3, r1, 1); cdc_irq(2, r1, 1); break;
    case 0x07: case 0x0D: case 0x03: case 0x17: case 0x18:       // MotorOn/SetFilter/Play/SetSession
      cdc_irq(3, r1, 1); cdc_irq(2, r1, 1); break;
    case 0x19: {                                                 // Test (sub-function in param[0])
      if (s_param[0] == 0x20) { uint8_t v[4] = { 0x94, 0x09, 0x19, 0xC0 };  // BIOS date/version
        cdc_irq(3, v, 4); } else cdc_irq(3, r1, 1);
      break; }
    case 0x1A: {                                                 // GetID -> region/license
      cdc_irq(3, r1, 1);
      uint8_t id[8] = { 0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A' };  // licensed, America
      cdc_irq(2, id, 8); break; }
    case 0x13: { uint8_t t[3] = { s_stat, 0x01, 0x01 }; cdc_irq(3, t, 3); break; }  // GetTN
    case 0x14: { uint8_t t[3] = { s_stat, 0x00, 0x02 }; cdc_irq(3, t, 3); break; }  // GetTD
    default:
      if (s_verbose) fprintf(stderr, "[cdc] UNHANDLED cmd 0x%02X -> ack only\n", cmd);
      cdc_irq(3, r1, 1); break;
  }
  s_param_n = 0;                                                 // command consumes the param FIFO
}

uint32_t cdc_read(uint32_t p) {
  switch (p & 3) {
    case 0: {  // status register
      uint8_t st = (uint8_t)s_index;
      if (s_param_n == 0) st |= 0x08;              // PRMEMPT (param FIFO empty)
      if (s_param_n < 16) st |= 0x10;              // PRMWRDY (param FIFO not full)
      if (!q_empty() && s_resp_rd < s_q[s_q_head].len) st |= 0x20;  // RSLRRDY (response ready)
      if (s_data_rd < s_data_n) st |= 0x40;        // DRQSTS (data FIFO not empty)
      return st;
    }
    case 1: {  // response FIFO
      if (q_empty()) return 0;
      CdcIrq* f = &s_q[s_q_head];
      return s_resp_rd < f->len ? f->resp[s_resp_rd++] : 0;
    }
    case 2:    // data FIFO
      return s_data_rd < s_data_n ? s_data[s_data_rd++] : 0;
    case 3:    // bank0/2: interrupt enable; bank1/3: interrupt flag (low 3 bits = pending type)
      if (s_index & 1) return q_empty() ? 0xE0 : (uint8_t)(0xE0 | s_q[s_q_head].type);
      return s_irq_en | 0xE0;
  }
  return 0;
}

void cdc_write(uint32_t p, uint8_t v) {
  switch (p & 3) {
    case 0: s_index = v & 3; return;               // index/bank select
    case 1:
      if (s_index == 0) exec_command(v);           // command register
      return;
    case 2:
      if (s_index == 0) { if (s_param_n < 16) s_param[s_param_n++] = v; }  // param FIFO push
      else if (s_index == 1) s_irq_en = v;         // interrupt enable
      return;
    case 3:
      if (s_index == 1) {                          // interrupt flag: write 1s to ack/clear
        if (v & 0x07) {                            // ack current IRQ -> advance the queue
          if (!q_empty()) { s_q_head = (s_q_head + 1) & 7; s_resp_rd = 0; }
        }
        if (v & 0x40) s_param_n = 0;               // reset param FIFO
      } else if (s_index == 0) {                   // request register
        if (v & 0x80) { if (s_reading) load_sector(); }  // BFRD: want sector data
      }
      return;
  }
}

void cdc_init(void) { s_verbose = getenv("PSXPORT_CDC_VERBOSE") ? 1 : 0; }
