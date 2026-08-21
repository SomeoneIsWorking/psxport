// Native CD controller (CDROM registers 0x1F801800-0x1F801803).
//
// The boot stub's SCEA screen (and parts of libcd) drive the CD controller at the REGISTER level:
// it selects a register bank via 0x1F801800, pushes parameters to the param FIFO, writes a command,
// then polls the Interrupt Flag register (0x1F801803 bank1, low 3 bits = response type) for the
// controller's response. With no controller emulated those reads return 0 and the stub's SCEA state
// machine (0x800123B0) loops forever. This is a focused, faithful model of the CXD1199-style
// register interface: index banking, parameter/response/data FIFOs, and a queue of pending
// interrupts (commands that return INT3-ack-then-INT2-complete). Data reads are served from the
// disc image (disc.cpp). We complete commands SYNCHRONOUSLY (the response is ready on the next poll),
// which is correct for code that busy-polls the flag without advancing time.
//
// Register map (bank = 0x1F801800 & 3):
//   0x1F801800  W: index/bank (low 2 bits)      R: status (FIFO/busy bits + index)
//   0x1F801801  W bank0: command                R: response FIFO (pop)
//   0x1F801802  W bank0: parameter FIFO (push)   R: data FIFO (pop)
//   0x1F801803  W bank0: request (BFRD want-data) W bank1: ack/reset IRQ flags
//               R bank0/2: interrupt enable      R bank1/3: interrupt flag (pending IRQ type)
#include "cdc_state.h"
#include "disc.h"
#include "r3000.h"
#include <lucent/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// PER-INSTANCE CD-controller state: the register/FIFO/IRQ-queue model lives on a CdcState (one per
// Game, game.h) passed EXPLICITLY to every entry point — no bound "current" pointer.

void cdc_state_init(CdcState *s) {
  struct DiscState *disc = s->disc; // preserve wiring across re-init
  memset(s, 0, sizeof *s);
  s->stat = 0x02; // power-on defaults
  s->disc = disc;
}

static void cdc_irq(CdcState *s, uint8_t type, const uint8_t *resp, int len) {
  int n = (s->q_tail + 1) & 7;
  if (n == s->q_head) {
    return; // queue full (shouldn't happen)
  }
  s->q[s->q_tail].type = type;
  s->q[s->q_tail].len = len;
  if (len) {
    memcpy(s->q[s->q_tail].resp, resp, (size_t)len);
  }
  if (s->q_tail == s->q_head) {
    s->resp_rd = 0; // first entry becomes active
  }
  s->q_tail = n;
  s->irq_edge = 1; // -> I_STAT bit 2, latched by the MMIO dispatcher
}
static int q_empty(CdcState *s) {
  return s->q_head == s->q_tail;
}

static uint32_t lba_from_param(CdcState *s) { // Setloc params: amm,ass,asect (BCD), minus the 2s lead-in
  const uint8_t *p = s->param;
  int mm = (p[0] >> 4) * 10 + (p[0] & 0xF);
  int ss = (p[1] >> 4) * 10 + (p[1] & 0xF);
  int ff = (p[2] >> 4) * 10 + (p[2] & 0xF);
  return (uint32_t)((mm * 60 + ss) * 75 + ff - 150);
}

static bool load_sector(CdcState *s) { // fill the data FIFO with the sector at loc_lba
  // WHAT THE FIFO CONTAINS DEPENDS ON Setmode's bit 0x20 ("whole sector"), and getting this wrong is
  // invisible until much later. Streaming code reads the first 8 words of each sector expecting the
  // 4-byte header plus 8-byte subheader — that is how it tells a video sector from an audio one.
  // Serving user data there hands it 32 bytes of picture data as a subheader, it recognises nothing,
  // and it re-requests the same sector forever.
  //   bit 0x20 set  -> raw sector from offset 12 (past sync): header, subheader, then data
  //   bit 0x20 clear-> 2048 bytes of user data only
  if (s->mode & 0x20) {
    uint8_t raw[2352];
    if (!disc_read_raw(s->disc, s->loc_lba, raw, sizeof raw)) {
      lucent::error("cdc", "ReadN: no data for LBA {} — data FIFO left EMPTY", s->loc_lba);
      s->data_n = 0;
      s->data_rd = 0;
      return false;
    }
    // The subheader is at raw[16..23]: file, channel, submode, coding. Submode bit 0x04 marks an
    // XA-ADPCM audio sector. Logged for EVERY sector, with its submode, so a run's census carries
    // its own denominator — "no audio sectors" is only meaningful next to "of N sectors read".
    lucent::debug("cdc",
                  "sector LBA {} file={} chan={} submode=0x{:02X} audio={} -> data FIFO",
                  s->loc_lba,
                  raw[16],
                  raw[17],
                  raw[18],
                  (raw[18] & 0x04) ? 1 : 0);
    memcpy(s->data, raw + 12, 2340);
    s->data_n = 2340;
    s->data_rd = 0;
    return true;
  }
  uint8_t sec[2048];
  if (!disc_read_sector(s->disc, s->loc_lba, sec)) {
    lucent::error("cdc", "ReadN: no data for LBA {} (no disc, or out of range) — data FIFO left EMPTY", s->loc_lba);
    s->data_n = 0;
    s->data_rd = 0;
    return false;
  }
  memcpy(s->data, sec, 2048);
  s->data_n = 2048;
  s->data_rd = 0;
  return true;
}

static void queue_data_ready(CdcState *s) {
  const uint8_t response[1] = {s->stat};
  cdc_irq(s, 1, response, 1);
}

static void stop_continuous_read(CdcState *s) {
  s->reading = 0;
  s->following_sector_ready = 0;
}

static void start_continuous_read(CdcState *s) {
  s->reading = 1;
  s->bfrd = 0;
  s->following_sector_ready = 0;
  if (!load_sector(s)) {
    stop_continuous_read(s);
    return;
  }
  queue_data_ready(s);
}

// The drive and the BFRD data FIFO are separate buffers. Once software accepts the current sector
// into the FIFO, a continuous ReadN/ReadS can receive and announce the following sector even while
// unread bytes remain in that FIFO. The synchronous model records that drive-side availability and
// emits its one INT1 immediately; a later BFRD service request swaps the announced sector into the
// FIFO. Keeping this state separate is what permits clients to leave a raw sector's EDC/ECC tail
// unread without stalling the drive.
static void announce_following_sector(CdcState *s) {
  if (!s->reading || s->following_sector_ready) {
    return;
  }
  s->following_sector_ready = 1;
  queue_data_ready(s);
}

// Draining the data FIFO releases that buffer; it does not drive the disc head or produce an event.
// If a following sector was announced, the next BFRD service request installs it. Clearing BFRD
// here models the hardware FIFO becoming serviceable again even when software leaves the request
// bit high.
static void sector_consumed(CdcState *s) {
  s->data_n = 0;
  s->data_rd = 0;
  s->bfrd = 0;
}

// DMA3 (CDROM -> RAM): pop up to `words` 32-bit words from the sector data FIFO. Returns the count
// actually delivered; a SHORT return means the FIFO ran dry and the caller must say so loudly — a
// transfer reported complete that moved nothing is exactly the silent lie this layer must not tell.
// Begin a sequential read at `lba` and make the first sector available NOW.
//
// Why the HLE path must call this: a game can read the disc at TWO levels. File reads go through
// libcd (CdRead/CdGetSector), which this port serves natively. But XA/streaming code bypasses all of
// that and drives the hardware directly — it spins on the CD status register's DRQSTS bit waiting for
// the data FIFO to fill, then kicks DMA3. With libcd HLE'd, the controller model never saw a command,
// so its FIFO stayed empty, DRQSTS never set, and the streaming poller spun forever BEFORE the DMA
// it was preparing ever started.
//
// So the two layers must share one source of truth: when the native CD layer accepts a read, the
// controller model is positioned and loaded from the same disc image. Neither layer invents data.
void cdc_set_mode(CdcState *s, uint8_t mode) {
  // Print the WHOLE byte, not just the bits this model currently acts on. Bit 0x40 (XA-ADPCM
  // enable) and bit 0x08 (XA filter) decide whether the drive is supposed to swallow audio
  // sectors into the ADPCM decoder instead of presenting them as data; a mode log that showed
  // only 0x20 would make an unimplemented audio path look like a mode the game never asked for.
  lucent::debug("cdc",
                "setmode 0x{:02X} (speed={} xa-adpcm={} filter={} whole-sector={})",
                mode,
                (mode & 0x80) ? 2 : 1,
                (mode & 0x40) ? 1 : 0,
                (mode & 0x08) ? 1 : 0,
                (mode & 0x20) ? 1 : 0);
  s->mode = mode;
}

void cdc_begin_read(CdcState *s, uint32_t lba) {
  s->loc_lba = lba;
  start_continuous_read(s);
}

int cdc_dma_read(CdcState *s, uint32_t *out, int words) {
  if (!s->bfrd) {
    return 0;
  }
  int got = 0;
  while (got < words && s->data_rd + 4 <= s->data_n) {
    const uint8_t *p = s->data + s->data_rd;
    out[got++] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    s->data_rd += 4;
  }
  if (got && s->data_rd >= s->data_n) {
    sector_consumed(s);
  }
  return got;
}

static void exec_command(CdcState *s, uint8_t cmd) {
  lucent::debug(
      "cdc", "cmd 0x{:02X} params={} [{:02X} {:02X} {:02X}]", cmd, s->param_n, s->param[0], s->param[1], s->param[2]);
  uint8_t r1[1] = {s->stat};
  switch (cmd) {
  case 0x01:
    cdc_irq(s, 3, r1, 1);
    break; // Getstat
  case 0x02:
    s->loc_lba = lba_from_param(s);
    cdc_irq(s, 3, r1, 1);
    break; // Setloc
  case 0x0E:
    s->mode = s->param[0];
    cdc_irq(s, 3, r1, 1);
    break; // Setmode
  case 0x06:
  case 0x1B: // ReadN / ReadS
    cdc_irq(s, 3, r1, 1);
    start_continuous_read(s);
    break;
  case 0x09:
    stop_continuous_read(s);
    cdc_irq(s, 3, r1, 1);
    cdc_irq(s, 2, r1, 1);
    break; // Pause
  case 0x08:
    stop_continuous_read(s);
    cdc_irq(s, 3, r1, 1);
    cdc_irq(s, 2, r1, 1);
    break; // Stop
  case 0x0A:
    stop_continuous_read(s);
    s->stat = 0x02;
    s->mode = 0;
    cdc_irq(s, 3, r1, 1);
    cdc_irq(s, 2, r1, 1);
    break; // Init
  case 0x0B:
  case 0x0C:
    cdc_irq(s, 3, r1, 1);
    break; // Mute / Demute
  case 0x15:
  case 0x16: // SeekL / SeekP
    cdc_irq(s, 3, r1, 1);
    cdc_irq(s, 2, r1, 1);
    break;
  case 0x1E: // ReadTOC
    cdc_irq(s, 3, r1, 1);
    cdc_irq(s, 2, r1, 1);
    break;
  case 0x07:
  case 0x0D:
  case 0x03:
  case 0x17:
  case 0x18: // MotorOn/SetFilter/Play/SetSession
    cdc_irq(s, 3, r1, 1);
    cdc_irq(s, 2, r1, 1);
    break;
  case 0x19: { // Test (sub-function in param[0])
    if (s->param[0] == 0x20) {
      uint8_t v[4] = {0x94, 0x09, 0x19, 0xC0}; // BIOS date/version
      cdc_irq(s, 3, v, 4);
    } else {
      cdc_irq(s, 3, r1, 1);
    }
    break;
  }
  case 0x1A: { // GetID -> region/license
    cdc_irq(s, 3, r1, 1);
    uint8_t id[8] = {0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A'}; // licensed, America
    cdc_irq(s, 2, id, 8);
    break;
  }
  case 0x13: {
    uint8_t t[3] = {s->stat, 0x01, 0x01};
    cdc_irq(s, 3, t, 3);
    break;
  } // GetTN
  case 0x14: {
    uint8_t t[3] = {s->stat, 0x00, 0x02};
    cdc_irq(s, 3, t, 3);
    break;
  } // GetTD
  default:
    lucent::debug("cdc", "UNHANDLED cmd 0x{:02X} -> ack only", cmd);
    cdc_irq(s, 3, r1, 1);
    break;
  }
  s->param_n = 0; // command consumes the param FIFO
}

// Apply the bank-0 request register's BFRD latch. BFRD is a level, not a command: writing 0x80
// again while it is already asserted leaves the current data FIFO and its read cursor untouched.
// A real new request is the 0 -> 1 transition. In this synchronous model that transition after a
// partial read discards the unread remainder and presents the already-announced following sector;
// that is how whole-sector stream readers move on after consuming only their 2048-byte payload.
static void write_request_register(CdcState *s, uint8_t value) {
  const uint8_t asserted = value & 0x80;
  if (!asserted) {
    s->bfrd = 0;
    return;
  }
  if (s->bfrd) {
    return;
  }

  s->bfrd = 1;
  if (!s->reading) {
    return;
  }

  // Reasserting an untouched FIFO only re-enables access to those same bytes. A consumed or empty
  // FIFO accepts the following sector that the continuous drive announced independently.
  if (s->following_sector_ready && (s->data_rd > 0 || s->data_n == 0)) {
    s->loc_lba++;
    s->following_sector_ready = 0;
    if (!load_sector(s)) {
      stop_continuous_read(s);
      return;
    }
  }
  if (s->data_n > 0) {
    announce_following_sector(s);
  }
}

uint32_t cdc_read(CdcState *s, uint32_t p) {
  switch (p & 3) {
  case 0: { // status register
    uint8_t st = (uint8_t)s->index;
    if (s->param_n == 0) {
      st |= 0x08; // PRMEMPT (param FIFO empty)
    }
    if (s->param_n < 16) {
      st |= 0x10; // PRMWRDY (param FIFO not full)
    }
    if (!q_empty(s) && s->resp_rd < s->q[s->q_head].len) {
      st |= 0x20; // RSLRRDY (response ready)
    }
    if (s->bfrd && s->data_rd < s->data_n) {
      st |= 0x40; // DRQSTS (data FIFO not empty)
    }
    return st;
  }
  case 1: { // response FIFO
    if (q_empty(s)) {
      return 0;
    }
    CdcIrqEnt *f = &s->q[s->q_head];
    return s->resp_rd < f->len ? f->resp[s->resp_rd++] : 0;
  }
  case 2: { // data FIFO — CPU pop path; must advance the head exactly as DMA3 does
    if (!s->bfrd || s->data_rd >= s->data_n) {
      return 0;
    }
    const uint8_t b = s->data[s->data_rd++];
    if (s->data_rd >= s->data_n) {
      sector_consumed(s);
    }
    return b;
  }
  case 3: // bank0/2: interrupt enable; bank1/3: interrupt flag (low 3 bits = pending type)
    if (s->index & 1) {
      return q_empty(s) ? 0xE0 : (uint8_t)(0xE0 | s->q[s->q_head].type);
    }
    return s->irq_en | 0xE0;
  }
  return 0;
}

void cdc_write(CdcState *s, uint32_t p, uint8_t v) {
  switch (p & 3) {
  case 0:
    s->index = v & 3;
    return; // index/bank select
  case 1:
    if (s->index == 0) {
      exec_command(s, v); // command register
    }
    return;
  case 2:
    if (s->index == 0) {
      if (s->param_n < 16) {
        s->param[s->param_n++] = v;
      }
    } // param FIFO push
    else if (s->index == 1) {
      s->irq_en = v; // interrupt enable
    }
    return;
  case 3:
    if (s->index == 1) { // interrupt flag: write 1s to ack/clear
      if (v & 0x07) {    // ack current IRQ -> advance the queue
        if (!q_empty(s)) {
          s->q_head = (s->q_head + 1) & 7;
          s->resp_rd = 0;
          // A response that was QUEUED behind the one just acked becomes current now, and that is
          // a fresh interrupt on real hardware — not a continuation of the acked one. Without this
          // edge the second and later responses of a multi-INT command sequence would be visible
          // in the FIFO but never announced, which looks exactly like a dropped response.
          if (!q_empty(s)) {
            s->irq_edge = 1;
          }
        }
      }
      if (v & 0x40) {
        s->param_n = 0; // reset param FIFO
      }
    } else if (s->index == 0) { // request register
      write_request_register(s, v);
    }
    return;
  }
}
