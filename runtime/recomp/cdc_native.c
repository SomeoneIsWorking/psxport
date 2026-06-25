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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int disc_read_sector(uint32_t lba, uint8_t* out);

static int s_verbose = -1;               // config (immutable after init) — NOT machine state, stays shared

// PER-INSTANCE CD-controller state: the register/FIFO/IRQ-queue model lives on the bound CdcState (one
// per Game, game.h). cdc_bind_state() points `s_cur` at the active core's CdcState before that core runs
// (native_step_frame), exactly like GTE_BindState. Until a bind happens we use a default instance so a
// pre-bind register touch is still safe.
static CdcState  s_default;
static CdcState* s_cur = &s_default;

void cdc_state_init(CdcState* s) { memset(s, 0, sizeof *s); s->stat = 0x02; }   // power-on defaults
void cdc_bind_state(CdcState* s) { s_cur = s ? s : &s_default; }

static void cdc_irq(uint8_t type, const uint8_t* resp, int len) {
  CdcState* s = s_cur;
  int n = (s->q_tail + 1) & 7;
  if (n == s->q_head) return;            // queue full (shouldn't happen)
  s->q[s->q_tail].type = type;
  s->q[s->q_tail].len = len;
  if (len) memcpy(s->q[s->q_tail].resp, resp, (size_t)len);
  if (s->q_tail == s->q_head) s->resp_rd = 0;   // first entry becomes active
  s->q_tail = n;
}
static int q_empty(void) { return s_cur->q_head == s_cur->q_tail; }

static uint32_t lba_from_param(void) {     // Setloc params: amm,ass,asect (BCD), minus the 2s lead-in
  const uint8_t* p = s_cur->param;
  int mm = (p[0] >> 4) * 10 + (p[0] & 0xF);
  int ss = (p[1] >> 4) * 10 + (p[1] & 0xF);
  int ff = (p[2] >> 4) * 10 + (p[2] & 0xF);
  return (uint32_t)((mm * 60 + ss) * 75 + ff - 150);
}

static void load_sector(void) {           // fill the data FIFO with the sector at loc_lba
  CdcState* s = s_cur;
  uint8_t sec[2048];
  if (!disc_read_sector(s->loc_lba, sec)) { s->data_n = 0; s->data_rd = 0; return; }
  memcpy(s->data, sec, 2048); s->data_n = 2048; s->data_rd = 0;
}

static void exec_command(uint8_t cmd) {
  CdcState* s = s_cur;
  if (s_verbose || cfg_dbg("cdc"))
    fprintf(stderr, "[cdc] cmd 0x%02X params=%d [%02X %02X %02X]\n", cmd, s->param_n,
            s->param[0], s->param[1], s->param[2]);
  uint8_t r1[1] = { s->stat };
  switch (cmd) {
    case 0x01: cdc_irq(3, r1, 1); break;                          // Getstat
    case 0x02: s->loc_lba = lba_from_param(); cdc_irq(3, r1, 1); break;  // Setloc
    case 0x0E: s->mode = s->param[0]; cdc_irq(3, r1, 1); break;   // Setmode
    case 0x06: case 0x1B:                                         // ReadN / ReadS
      s->reading = 1; cdc_irq(3, r1, 1);
      load_sector(); { uint8_t s2[1] = { s->stat }; cdc_irq(1, s2, 1); }  // INT1 data-ready
      break;
    case 0x09: s->reading = 0; cdc_irq(3, r1, 1); cdc_irq(2, r1, 1); break;  // Pause
    case 0x08: s->reading = 0; cdc_irq(3, r1, 1); cdc_irq(2, r1, 1); break;  // Stop
    case 0x0A: s->stat = 0x02; s->mode = 0; cdc_irq(3, r1, 1); cdc_irq(2, r1, 1); break;  // Init
    case 0x0B: case 0x0C: cdc_irq(3, r1, 1); break;              // Mute / Demute
    case 0x15: case 0x16:                                        // SeekL / SeekP
      cdc_irq(3, r1, 1); cdc_irq(2, r1, 1); break;
    case 0x1E:                                                   // ReadTOC
      cdc_irq(3, r1, 1); cdc_irq(2, r1, 1); break;
    case 0x07: case 0x0D: case 0x03: case 0x17: case 0x18:       // MotorOn/SetFilter/Play/SetSession
      cdc_irq(3, r1, 1); cdc_irq(2, r1, 1); break;
    case 0x19: {                                                 // Test (sub-function in param[0])
      if (s->param[0] == 0x20) { uint8_t v[4] = { 0x94, 0x09, 0x19, 0xC0 };  // BIOS date/version
        cdc_irq(3, v, 4); } else cdc_irq(3, r1, 1);
      break; }
    case 0x1A: {                                                 // GetID -> region/license
      cdc_irq(3, r1, 1);
      uint8_t id[8] = { 0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A' };  // licensed, America
      cdc_irq(2, id, 8); break; }
    case 0x13: { uint8_t t[3] = { s->stat, 0x01, 0x01 }; cdc_irq(3, t, 3); break; }  // GetTN
    case 0x14: { uint8_t t[3] = { s->stat, 0x00, 0x02 }; cdc_irq(3, t, 3); break; }  // GetTD
    default:
      if (s_verbose) fprintf(stderr, "[cdc] UNHANDLED cmd 0x%02X -> ack only\n", cmd);
      cdc_irq(3, r1, 1); break;
  }
  s->param_n = 0;                                                // command consumes the param FIFO
}

uint32_t cdc_read(uint32_t p) {
  CdcState* s = s_cur;
  switch (p & 3) {
    case 0: {  // status register
      uint8_t st = (uint8_t)s->index;
      if (s->param_n == 0) st |= 0x08;             // PRMEMPT (param FIFO empty)
      if (s->param_n < 16) st |= 0x10;             // PRMWRDY (param FIFO not full)
      if (!q_empty() && s->resp_rd < s->q[s->q_head].len) st |= 0x20;  // RSLRRDY (response ready)
      if (s->data_rd < s->data_n) st |= 0x40;      // DRQSTS (data FIFO not empty)
      return st;
    }
    case 1: {  // response FIFO
      if (q_empty()) return 0;
      CdcIrqEnt* f = &s->q[s->q_head];
      return s->resp_rd < f->len ? f->resp[s->resp_rd++] : 0;
    }
    case 2:    // data FIFO
      return s->data_rd < s->data_n ? s->data[s->data_rd++] : 0;
    case 3:    // bank0/2: interrupt enable; bank1/3: interrupt flag (low 3 bits = pending type)
      if (s->index & 1) return q_empty() ? 0xE0 : (uint8_t)(0xE0 | s->q[s->q_head].type);
      return s->irq_en | 0xE0;
  }
  return 0;
}

void cdc_write(uint32_t p, uint8_t v) {
  CdcState* s = s_cur;
  switch (p & 3) {
    case 0: s->index = v & 3; return;              // index/bank select
    case 1:
      if (s->index == 0) exec_command(v);          // command register
      return;
    case 2:
      if (s->index == 0) { if (s->param_n < 16) s->param[s->param_n++] = v; }  // param FIFO push
      else if (s->index == 1) s->irq_en = v;       // interrupt enable
      return;
    case 3:
      if (s->index == 1) {                         // interrupt flag: write 1s to ack/clear
        if (v & 0x07) {                            // ack current IRQ -> advance the queue
          if (!q_empty()) { s->q_head = (s->q_head + 1) & 7; s->resp_rd = 0; }
        }
        if (v & 0x40) s->param_n = 0;              // reset param FIFO
      } else if (s->index == 0) {                  // request register
        if (v & 0x80) { if (s->reading) load_sector(); }  // BFRD: want sector data
      }
      return;
  }
}

void cdc_init(void) { s_verbose = cfg_dbg("cdc") ? 1 : 0; cdc_state_init(&s_default); }
