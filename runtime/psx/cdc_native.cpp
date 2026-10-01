// Native CD controller (CDROM registers 0x1F801800-0x1F801803).
//
// The boot stub's SCEA screen (and parts of libcd) drive the CD controller at the REGISTER level:
// it selects a register bank via 0x1F801800, pushes parameters to the param FIFO, writes a command,
// then polls the Interrupt Flag register (0x1F801803 bank1, low 3 bits = response type) for the
// controller's response. With no controller emulated those reads return 0 and the stub's SCEA state
// machine (0x800123B0) loops forever. This is a focused, faithful model of the CXD1199-style
// register interface: index banking, parameter/response/data FIFOs, and a queue of pending
// interrupts (commands that return INT3-ack-then-INT2-complete). Data reads are served from the
// disc image (disc.cpp). Command receive, argument transfer, execution, and completion are scheduled
// in deterministic guest time; ReadN sector availability uses that same clock at the nominal speed.
//
// Register map (bank = 0x1F801800 & 3):
//   0x1F801800  W: index/bank (low 2 bits)      R: status (FIFO/busy bits + index)
//   0x1F801801  W bank0: command                R: response FIFO (pop)
//   0x1F801802  W bank0: parameter FIFO (push)   R: data FIFO (pop)
//   0x1F801803  W bank0: request (BFRD want-data) W bank1: ack/reset IRQ flags
//               R bank0/2: interrupt enable      R bank1/3: interrupt flag (pending IRQ type)
#include "cd_drive_timing.h"
#include "cdc_command_phase.h"
#include "cdc_state.h"
#include "disc.h"
#include "r3000.h"
#include "xa_state.h"
#include <algorithm>
#include <lucent/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Forward decls (definitions below): the FIFO fill used by drive_consume_sector, and the
// beetle-parity routing wrapper that decides decoder-vs-FIFO per sector.
static bool load_sector(CdcState *s, uint32_t lba);
static uint64_t exec_command(CdcState *s, uint8_t cmd);
static void complete_command(CdcState *s);

// PER-INSTANCE CD-controller state: the register/FIFO/IRQ-queue model lives on a CdcState (one per
// Game, game.h) passed EXPLICITLY to every entry point — no bound "current" pointer.

constexpr uint8_t kCdlStatStandby = 0x02;
constexpr uint8_t kCdlStatRead = 0x20;

void cdc_state_init(CdcState *s) {
  struct DiscState *disc = s->disc; // preserve wiring across re-init
  memset(s, 0, sizeof *s);
  s->stat = kCdlStatStandby; // power-on defaults
  s->disc = disc;
  s->disc_read_raw_fn = disc_read_raw;
  s->disc_read_sector_fn = disc_read_sector;
  s->disc_get_subq_position_fn = disc_get_subq_position;
}

static void cdc_irq(CdcState *s, uint8_t type, const uint8_t *resp, int len) {
  const bool becomes_current = s->q_tail == s->q_head;
  int n = (s->q_tail + 1) & 7;
  if (n == s->q_head) {
    return; // queue full (shouldn't happen)
  }
  s->q[s->q_tail].type = type;
  s->q[s->q_tail].len = len;
  if (len) {
    memcpy(s->q[s->q_tail].resp, resp, (size_t)len);
  }
  if (becomes_current) {
    s->resp_rd = 0; // first entry becomes active
  }
  s->q_tail = n;
  if (becomes_current) {
    ++s->irq_sequence;
    s->irq_edge = 1; // -> I_STAT bit 2, latched by the MMIO dispatcher
  }
}
static int q_empty(const CdcState *s) {
  return s->q_head == s->q_tail;
}

uint8_t cdc_current_irq_type(const CdcState *s) {
  return !s || q_empty(s) ? 0u : s->q[s->q_head].type;
}

static uint32_t lba_from_command_args(const CdcState *s) {
  // Setloc params: amm,ass,asect (BCD), minus the 2s lead-in.
  const uint8_t *p = s->command_args;
  int mm = (p[0] >> 4) * 10 + (p[0] & 0xF);
  int ss = (p[1] >> 4) * 10 + (p[1] & 0xF);
  int ff = (p[2] >> 4) * 10 + (p[2] & 0xF);
  return (uint32_t)((mm * 60 + ss) * 75 + ff - 150);
}

static uint64_t deterministic_seek_time(const CdcState *s, uint32_t target_lba) {
  // Beetle's PS_CDC_CalcSeekTime with only its random 0..25000 component fixed at zero.
  const bool motor_on = (s->stat & kCdlStatStandby) != 0;
  const bool paused = motor_on && !s->reading;
  const uint32_t initial_lba = motor_on ? s->loc_lba : 0;
  const uint64_t difference = initial_lba > target_lba ? initial_lba - target_lba : target_lba - initial_lba;
  uint64_t ticks = motor_on ? 0u : 33'868'800u;
  const uint64_t seek_ticks = difference * 33'868'800u / (72u * 60u * 75u);
  ticks += seek_ticks < 20'000u ? 20'000u : seek_ticks;
  if (difference >= 2'250u) {
    ticks += 33'868'800u * 300u / 1'000u;
  } else if (paused) {
    ticks += (s->mode & 0x80) != 0 ? 1'237'952u : 1'237'952u * 2u;
  } else if (difference >= 3u && difference < 12u) {
    ticks += cd_drive_sector_period_cpu_ticks(s->mode) * 4u;
  }
  return ticks;
}

static bool command_accepts_argument_count(uint8_t command, uint8_t count) {
  switch (command) {
  case 0x02:
    return count == 3;
  case 0x03:
    return count <= 1;
  case 0x0D:
  case 0x1D:
    return count == 2;
  case 0x0E:
  case 0x12:
  case 0x14:
  case 0x19:
    return count == 1;
  case 0x01:
  case 0x04:
  case 0x05:
  case 0x06:
  case 0x07:
  case 0x08:
  case 0x09:
  case 0x0A:
  case 0x0B:
  case 0x0C:
  case 0x0F:
  case 0x10:
  case 0x11:
  case 0x13:
  case 0x15:
  case 0x16:
  case 0x1A:
  case 0x1B:
  case 0x1C:
  case 0x1E:
    return count == 0;
  default:
    return false;
  }
}

// Beetle-parity XA routing test (vendor cdc.c DS_READING data path): with Setmode's STRSND bit
// (0x40) set, a Mode2 sector whose submode has ALL of RT|form2|audio (0x64) belongs to the ADPCM
// decoder -> SPU CD-audio input. MODE_SF (0x08) then selects exactly one file/channel; without that
// filter every matching XA audio sector is selected. The game's movie demuxer counts on non-selected
// sectors continuing through the data path, while selected sectors never enter its data FIFO.
int cdc_xa_sector_selected(const CdcState *s, const uint8_t *raw) {
  if ((s->mode & 0x40) == 0 || raw[15] != 2 || (raw[18] & 0x64) != 0x64) {
    return 0;
  }
  return (s->mode & 0x08) == 0 || (raw[16] == s->filter_file && raw[17] == s->filter_chan);
}

static bool sector_is_xa_audio(const CdcState *s, const uint8_t *raw) {
  return cdc_xa_sector_selected(s, raw) != 0;
}

// Hand one audio sector to the SPU ring. First routing also flips the ring into PUSH mode: from
// then on the SPU pull never fetches sectors itself — the drive owns the cursor (a pull-side
// self-fetch would read ahead of the physical head and desync A/V). Returns -1 when the ring is
// FULL: the drive must HOLD the sector (hardware backpressure — a real CD buffer cannot accept
// into a full FIFO) and retry the same sector after another period. >=0 means consumed (the
// sector moved past the head even if zero frames decoded from it).
static int route_audio_to_spu(CdcState *s, const uint8_t *raw, uint32_t drive_lba) {
  s->xa->push_mode = 1;
  return xa_push_audio_sector(s->xa, raw, drive_lba);
}

// One drive-paced sector consumption during a continuous read, at `lba`:
//   XA audio under STRSND -> decode into the SPU ring (out_audio=true; NO FIFO fill, NO INT1 —
//                           the guest never sees audio sectors in its DMA stream, exactly like
//                           hardware);
//   anything else         -> fill the data FIFO via load_sector (the caller raises INT1).
// Returns false only when the disc read itself failed. out_held=true marks the backpressure case:
// the ring was full, NOTHING advanced, and the caller must retry this same sector next period.
static bool drive_consume_sector(CdcState *s, uint32_t lba, bool *out_audio, bool *out_held) {
  *out_audio = false;
  *out_held = false;
  if (!(s->mode & 0x40)) { // STRSND off: every sector is a data sector
    return load_sector(s, lba);
  }
  uint8_t raw[2352];
  if (!s->disc_read_raw_fn(s->disc, lba, raw, sizeof raw)) {
    return false;
  }
  if (!sector_is_xa_audio(s, raw)) {
    return load_sector(s, lba);
  }
  if (route_audio_to_spu(s, raw, lba) < 0) {
    *out_held = true;
    return true;
  }
  *out_audio = true;
  return true;
}

static bool load_sector(CdcState *s, uint32_t lba) { // fill the data FIFO with the sector at lba
  // WHAT THE FIFO CONTAINS DEPENDS ON Setmode's bit 0x20 ("whole sector"), and getting this wrong is
  // invisible until much later. Streaming code reads the first 8 words of each sector expecting the
  // 4-byte header plus 8-byte subheader — that is how it tells a video sector from an audio one.
  // Serving user data there hands it 32 bytes of picture data as a subheader, it recognises nothing,
  // and it re-requests the same sector forever.
  //   bit 0x20 set  -> raw sector from offset 12 (past sync): header, subheader, then data
  //   bit 0x20 clear-> 2048 bytes of user data only
  if (s->mode & 0x20) {
    uint8_t raw[2352];
    if (!s->disc_read_raw_fn(s->disc, lba, raw, sizeof raw)) {
      lucent::error("cdc", "ReadN: no data for LBA {} — data FIFO left EMPTY", lba);
      s->data_n = 0;
      s->data_rd = 0;
      return false;
    }
    // The subheader is at raw[16..23]: file, channel, submode, coding. Submode bit 0x04 marks an
    // XA-ADPCM audio sector. Logged for EVERY sector, with its submode, so a run's census carries
    // its own denominator — "no audio sectors" is only meaningful next to "of N sectors read".
    lucent::debug("cdc",
                  "sector LBA {} file={} chan={} submode=0x{:02X} audio={} -> data FIFO",
                  lba,
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
  if (!s->disc_read_sector_fn(s->disc, lba, sec)) {
    lucent::error("cdc", "ReadN: no data for LBA {} (no disc, or out of range) — data FIFO left EMPTY", lba);
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

int cdc_post_data_ready(CdcState *s) {
  if (((s->q_tail + 1) & 7) == s->q_head) {
    return 0; // cdc_irq would drop it; say so instead of reporting a completion that was never queued
  }
  queue_data_ready(s);
  return 1;
}

int cdc_post_data_ready_after_read(CdcState *s, uint32_t first_lba, uint32_t sectors, uint8_t mode) {
  if (sectors == 0 || s->read_completion_n >= 4) {
    return 0;
  }
  if (!s->tick_now) {
    return cdc_post_data_ready(s);
  }
  const uint64_t now_ticks = s->tick_now(s->tick_context);
  const uint64_t start = s->read_completion_n ? s->read_completion_deadline_ticks[s->read_completion_n - 1] : now_ticks;
  s->read_completion_deadline_ticks[s->read_completion_n++] =
      (start > now_ticks ? start : now_ticks) + deterministic_seek_time(s, first_lba) +
      uint64_t{sectors} * cd_drive_sector_period_cpu_ticks(mode);
  s->loc_lba = first_lba + sectors;
  s->stat |= kCdlStatStandby; // the spindle is up once a read has been served
  return 1;
}

// Announce every owed read completion whose drive time has elapsed, oldest first. A full response queue
// leaves the rest owed for the next service rather than dropping one.
static int service_read_completions(CdcState *s) {
  if (s->read_completion_n == 0 || !s->tick_now) {
    return 0;
  }
  const uint64_t now_ticks = s->tick_now(s->tick_context);
  int announced = 0;
  while (s->read_completion_n > 0 && now_ticks >= s->read_completion_deadline_ticks[0] && cdc_post_data_ready(s)) {
    lucent::debug("cdcpace", "announced owed read completion now={} queue_empty={}", now_ticks, q_empty(s));
    s->read_completion_n--;
    for (int i = 0; i < s->read_completion_n; i++) {
      s->read_completion_deadline_ticks[i] = s->read_completion_deadline_ticks[i + 1];
    }
    announced = 1;
  }
  return announced;
}

static void cancel_drive_event(CdcState *s) {
  if (!s->drive_event_armed) {
    return;
  }
  s->drive_event_armed = 0;
  s->drive_deadline_ticks = 0;
}

static void schedule_sector_event(CdcState *s) {
  if (!s->reading || s->following_sector_ready || s->drive_event_armed || !s->tick_now) {
    return;
  }
  const uint64_t now_ticks = s->tick_now(s->tick_context);
  s->drive_deadline_ticks = now_ticks + cd_drive_sector_period_cpu_ticks(s->mode);
  s->drive_event_armed = 1;
  lucent::debug("cdcpace",
                "armed sector event now={} deadline={} mode=0x{:02X} first={}",
                now_ticks,
                s->drive_deadline_ticks,
                s->mode,
                s->first_sector_pending);
}

static void stop_continuous_read(CdcState *s) {
  cancel_drive_event(s);
  s->reading = 0;
  s->stat &= static_cast<uint8_t>(~kCdlStatRead); // clears when the drive leaves DS_READING
  s->first_sector_pending = 0;
  s->following_sector_ready = 0;
}

static void start_continuous_read(CdcState *s) {
  cancel_drive_event(s);
  s->reading = 1;
  s->stat |= kCdlStatRead;
  s->first_sector_pending = 1;
  s->bfrd = 0;
  s->following_sector_ready = 0;
  s->data_n = 0;
  s->data_rd = 0;
  schedule_sector_event(s);
}

void cdc_bind_tick_source(CdcState *s, void *context, CdcTickNowFn now) {
  s->tick_context = context;
  s->tick_now = now;
}

static int service_drive_event(CdcState *s) {
  if (!s->drive_event_armed || !s->tick_now) {
    return 0;
  }

  const uint64_t now_ticks = s->tick_now(s->tick_context);
  if (now_ticks < s->drive_deadline_ticks) {
    return 0;
  }

  s->drive_event_armed = 0;
  s->drive_deadline_ticks = 0;
  lucent::debug(
      "cdcpace", "servicing sector event now={} first={} lba={}", now_ticks, s->first_sector_pending, s->loc_lba);
  if (!s->reading || s->following_sector_ready) {
    return 0;
  }

  if (s->first_sector_pending) {
    s->first_sector_pending = 0;
    bool audio = false, held = false;
    if (!drive_consume_sector(s, s->loc_lba, &audio, &held)) {
      stop_continuous_read(s);
      return 0;
    }
    s->stat |= kCdlStatRead; // INT1/Getstat must identify an active sector read
    if (audio || held) {
      // Decoded straight to the SPU (or the ring is full and the sector is held for retry): the
      // guest sees no data-ready. The drive keeps reading — reschedule so the stream stays paced.
      schedule_sector_event(s);
      if (held) {
        s->first_sector_pending = 1; // retry THIS sector once the ring drains
      }
      lucent::debug("cdcpace", "first sector @ {} was XA audio -> SPU ring (held={})", s->loc_lba, held);
      return 0;
    }
    queue_data_ready(s);
    return 1;
  }

  // One drive-side buffer, one INT1. Software may still own unread bytes from the prior sector;
  // the later BFRD service request swaps this ready sector into the CPU/DMA-visible FIFO.
  // An XA-audio next sector never announces: the drive decodes it now and moves on, so a pure-
  // audio stretch produces no guest interrupts at all while the head still advances in real time.
  // Ring full -> hold: retry the SAME sector next period (no head advance, no announce).
  {
    bool next_audio = false;
    uint8_t raw[2352];
    const uint32_t next_lba = s->loc_lba + 1;
    if (s->disc_read_raw_fn(s->disc, next_lba, raw, sizeof raw) && sector_is_xa_audio(s, raw)) {
      if (route_audio_to_spu(s, raw, next_lba) >= 0) {
        s->loc_lba++; // the head PASSED this sector; the next peek must see the one after it
      }
      schedule_sector_event(s);
      return 0;
    }
  }
  s->following_sector_ready = 1;
  queue_data_ready(s);
  return 1;
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

// DMA3 (CDROM -> RAM): produce exactly `words` controller-read words. Bytes available in the data
// FIFO are drained first; the controller returns zero after depletion, matching Beetle's
// PS_CDC_DMARead rather than leaving the destination's old RAM contents in place. The return value
// is the number of words sourced from the FIFO, so the DMA owner can report the exact zero-filled
// denominator without pretending those zeros came from the disc.
// Begin a sequential read at `lba`; the first sector becomes available after one drive period.
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
  if (s->drive_event_armed) {
    cancel_drive_event(s);
    schedule_sector_event(s);
  }
}

void cdc_set_filter(CdcState *s, uint8_t file, uint8_t channel) {
  s->filter_file = file;
  s->filter_chan = channel;
  lucent::debug("cdc", "setfilter file={} chan={}", file, channel);
}

// Retire a still-pending command through the SAME execute/complete phases the clock-driven path
// uses, so a command keeps its own timing and its two responses. This is a QUEUE-SEAM helper, not an
// inline execution: it runs at the point where a LATER effect must not overtake an older command,
// never at the point the command was accepted.
//
// The controller executes the commands it accepted in the order it accepted them. The native CD
// path breaks that order when it accepts a read's effect synchronously: a framework-issued Pause
// (CdControl(Pause), answered to the guest synchronously by the CD command owner) is still pending
// on the guest clock, and the very next CdControl(ReadN) applies `cdc_begin_read` right away — so a
// Pause issued BEFORE the read executed AFTER the read had begun and cancelled that read's own
// sector event. Measured on C-12: no completion ever arrived and the title retried one sector every
// 60 fields.
static void retire_pending_command(CdcState *s) {
  if (!s->command_event_armed || !s->tick_now) {
    return;
  }
  for (int guard = 0; guard < 64 && s->command_event_armed; ++guard) {
    s->command_deadline_ticks = s->tick_now(s->tick_context);
    const CdcCommandEvent event = cdc_command_service(s, 1);
    if (event == CdcCommandEvent::kExecute) {
      cdc_command_finish_execution(s, exec_command(s, s->pending_command));
    } else if (event == CdcCommandEvent::kComplete) {
      complete_command(s);
    }
    // `kNone` only means this phase was not the last one; the loop keeps pulling the deadline to
    // now until the command has executed (and its completion disarms it).
  }
}

void cdc_begin_read(CdcState *s, uint32_t lba) {
  // The read is the NEXT command after anything still pending, so the older one runs first. The
  // timed contract is unchanged — the command was not executed inline at issue — but a command can
  // no longer land after the read it was issued before.
  retire_pending_command(s);
  s->loc_lba = lba;
  s->command_lba = lba;
  start_continuous_read(s);
}

void cdc_issue_command(CdcState *s, uint8_t command) {
  s->index = 0;
  // The responses this command produces belong to the GUEST that issued it: stock libcd polls
  // its own status and consumes them after every command (measured on C-12: Setloc -> Setmode ->
  // ReadN -> Pause, each followed by a GetStat poll), so the INT3 from execution and the INT2 from
  // completion both have a real consumer. An earlier owner deleted them here on the premise that
  // a framework-issued command has "no guest consumer", which is false for every command that
  // reaches this function: the guest issued it through its own CdControl. Deleting them left the
  // guest's post-Pause poll waiting for a completion that had already been thrown away.
  cdc_command_schedule(s, command);
  // TIMED, not inline. A controller takes its own time to accept a command and reports it in two
  // phases; `test_cdc_continuous_read` pins that a native-issued Pause observes the still-reading
  // status at execution and the paused status at completion. Ordering a framework-issued command
  // against the guest's NEXT one is the queue seam's job (`retire_pending_command`), not this
  // function's.
}

int cdc_take_current_response(CdcState *s, uint8_t *first_byte) {
  const int savedBank = s->index;
  uint8_t first = 0;
  int taken = 0;
  for (int i = 0;; i++) {
    if ((cdc_read(s, CDC_REG_INDEX) & CDC_STAT_RESPONSE_READY) == 0u) {
      break;
    }
    const uint8_t value = (uint8_t)cdc_read(s, CDC_REG_RESPONSE);
    if (i == 0) {
      first = value;
    }
    taken = 1;
  }
  cdc_write(s, CDC_REG_INDEX, (uint8_t)savedBank);
  if (taken && first_byte) {
    *first_byte = first;
  }
  return taken;
}

int cdc_dma_read(CdcState *s, uint32_t *out, int words) {
  if (words <= 0) {
    return 0;
  }
  memset(out, 0, (size_t)words * sizeof *out);
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

static uint64_t exec_command(CdcState *s, uint8_t cmd) {
  lucent::debug("cdc",
                "cmd 0x{:02X} args={} [{:02X} {:02X} {:02X}]",
                cmd,
                s->command_arg_n,
                s->command_args[0],
                s->command_args[1],
                s->command_args[2]);
  if (!command_accepts_argument_count(cmd, s->command_arg_n)) {
    const uint8_t error[2] = {static_cast<uint8_t>(s->stat | 0x01), 0x20};
    cdc_irq(s, 5, error, 2);
    return 0;
  }
  uint8_t r1[1] = {s->stat};
  switch (cmd) {
  case 0x01:
    cdc_irq(s, 3, r1, 1);
    return 0; // Getstat
  case 0x02:
    s->command_lba = lba_from_command_args(s);
    cdc_irq(s, 3, r1, 1);
    return 0; // Setloc
  case 0x0E:
    cdc_set_mode(s, s->command_args[0]);
    cdc_irq(s, 3, r1, 1);
    return 0; // Setmode
  case 0x0D:
    cdc_set_filter(s, s->command_args[0], s->command_args[1]);
    cdc_irq(s, 3, r1, 1);
    return 0;  // Setfilter
  case 0x11: { // GetlocP: current Sub-Q track/index and relative/absolute position.
    uint8_t position[8] = {};
    if (!s->disc_get_subq_position_fn || !s->disc_get_subq_position_fn(s->disc, s->loc_lba, position)) {
      const uint8_t error[2] = {static_cast<uint8_t>(s->stat | 0x01), 0x80};
      cdc_irq(s, 5, error, 2);
      return 0;
    }
    cdc_irq(s, 3, position, 8);
    return 0;
  }
  case 0x06:
  case 0x1B: // ReadN / ReadS
    cdc_irq(s, 3, r1, 1);
    s->loc_lba = s->command_lba;
    start_continuous_read(s);
    return 0;
  case 0x09: {
    const uint8_t reading_status[1] = {s->stat};
    const bool was_reading = s->reading != 0;
    stop_continuous_read(s);
    cdc_irq(s, 3, reading_status, 1);
    if (!was_reading) {
      return 5'000;
    }
    const uint64_t speed_scale = (s->mode & 0x80) != 0 ? 1u : 2u;
    return (1'124'584u + static_cast<uint64_t>(s->loc_lba) * 42'596u / (75u * 60u)) * speed_scale;
  }
  case 0x08: {
    const uint8_t reading_status[1] = {s->stat};
    const bool was_stopped = !s->reading && (s->stat & kCdlStatStandby) == 0;
    stop_continuous_read(s);
    s->stat = 0;
    cdc_irq(s, 3, reading_status, 1);
    return was_stopped ? 5'000u : 33'868u;
  }
  case 0x0A:
    stop_continuous_read(s);
    s->stat = kCdlStatStandby;
    s->mode = 0;
    s->command_lba = 0;
    cdc_irq(s, 3, r1, 1);
    return 4'100'000u; // Reset's deterministic seek-to-sector-zero completion.
  case 0x0B:
  case 0x0C:
    cdc_irq(s, 3, r1, 1);
    return 0; // Mute / Demute
  case 0x15:
  case 0x16: // SeekL / SeekP
    cdc_irq(s, 3, r1, 1);
    return deterministic_seek_time(s, s->command_lba);
  case 0x1E: // ReadTOC
    cdc_irq(s, 3, r1, 1);
    return 30'000'000u + deterministic_seek_time(s, 0);
  case 0x07:
    if (s->stat != 0) {
      const uint8_t error[2] = {static_cast<uint8_t>(s->stat | 0x01), 0x20};
      cdc_irq(s, 5, error, 2);
      return 0;
    }
    s->stat = kCdlStatStandby;
    cdc_irq(s, 3, r1, 1);
    return 3'386'880u;
  case 0x03:
  case 0x17:
  case 0x18: // MotorOn/SetFilter/Play/SetSession
    cdc_irq(s, 3, r1, 1);
    return 0;
  case 0x19: { // Test (sub-function in param[0])
    if (s->command_args[0] == 0x20) {
      uint8_t v[4] = {0x94, 0x09, 0x19, 0xC0}; // BIOS date/version
      cdc_irq(s, 3, v, 4);
    } else {
      cdc_irq(s, 3, r1, 1);
    }
    return 0;
  }
  case 0x1A: { // GetID -> region/license
    cdc_irq(s, 3, r1, 1);
    return 33'868u;
  }
  case 0x12:
    cdc_irq(s, 3, r1, 1);
    return 33'868u;
  case 0x13: {
    uint8_t t[3] = {s->stat, 0x01, 0x01};
    cdc_irq(s, 3, t, 3);
    return 0;
  } // GetTN
  case 0x14: {
    uint8_t t[3] = {s->stat, 0x00, 0x02};
    cdc_irq(s, 3, t, 3);
    return 0;
  } // GetTD
  default:
    lucent::debug("cdc", "UNHANDLED cmd 0x{:02X} -> ack only", cmd);
    cdc_irq(s, 3, r1, 1);
    return 0;
  }
}

static void complete_command(CdcState *s) {
  if (s->pending_command == 0x1A) {
    const uint8_t id[8] = {0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A'};
    cdc_irq(s, 2, id, 8);
    return;
  }
  if (s->pending_command == 0x0A) {
    s->loc_lba = 0;
  } else if (s->pending_command == 0x15 || s->pending_command == 0x16) {
    s->loc_lba = s->command_lba;
    s->stat = kCdlStatStandby;
  }
  const uint8_t response[1] = {s->stat};
  cdc_irq(s, 2, response, 1);
}

int cdc_next_deadline_ticks(const CdcState *s, uint64_t *absoluteTicks) {
  // Every armed deadline, so a Lightrec segment ends at the earliest of them: the drive's next sector,
  // the command phase, and the oldest paced read completion (the completion must be announced when the
  // drive is done, not at the next unrelated service).
  uint64_t earliest = 0;
  bool armed = false;
  const auto consider = [&](bool isArmed, uint64_t deadline) {
    if (isArmed && (!armed || deadline < earliest)) {
      earliest = deadline;
      armed = true;
    }
  };
  consider(s->drive_event_armed != 0, s->drive_deadline_ticks);
  consider(s->command_event_armed != 0, s->command_deadline_ticks);
  consider(s->read_completion_n != 0, s->read_completion_deadline_ticks[0]);
  if (!armed) {
    return 0;
  }
  *absoluteTicks = earliest;
  return 1;
}

int cdc_drive_service(CdcState *s) {
  // The oracle services its drive counter before its command counter when both expire in the same
  // update. Preserve that ordering so an exact tie makes INT1 current and queues command INT3.
  const uint64_t irq_sequence_before = s->irq_sequence;
  service_drive_event(s);
  service_read_completions(s);
  const CdcCommandEvent event = cdc_command_service(s, q_empty(s));
  if (event == CdcCommandEvent::kExecute) {
    cdc_command_finish_execution(s, exec_command(s, s->pending_command));
  } else if (event == CdcCommandEvent::kComplete) {
    complete_command(s);
  }
  return s->irq_sequence != irq_sequence_before;
}

// Apply the bank-0 request register's BFRD latch. BFRD is a level, not a command: writing 0x80
// again while it is already asserted leaves the current data FIFO and its read cursor untouched.
// A real new request is the 0 -> 1 transition. BFRD controls only software access to the data FIFO;
// elapsed drive time owns the following-sector INT1. A transition after that event discards any
// unread remainder and presents the already-ready sector.
// The consumable payload of one presented sector: the 12 header/subheader bytes whole-sector
// framing adds, then the 2048 data bytes. The EDC/ECC tail a 2352-byte raw sector carries after
// that is not data any consumer reads, so a request that has passed this point has the sector and
// is asking for the next one.
static std::uint32_t sector_payload_bytes(const CdcState *s) {
  return (s->mode & 0x20u) != 0 ? 12u + 2048u : 2048u;
}

static void write_request_register(CdcState *s, uint8_t value) {
  const uint8_t asserted = value & 0x80;
  if (!asserted) {
    s->bfrd = 0;
    return;
  }

  // An asserted-to-asserted write leaves the active FIFO and its cursor alone: a consumer that
  // splits one sector transfer around such a write is still reading THAT sector (psxport issue
  // 0002). What a repeated write does carry is the request for the sector the drive has since
  // ANNOUNCED — stock libcd writes the bit once per sector and never clears it in between, so a
  // write that only acted on a 0 -> 1 transition left every sector after the first unanswered.
  const bool fresh_request = s->bfrd == 0;
  s->bfrd = 1;
  if (!s->reading) {
    return;
  }

  // The announced sector becomes the one a request presents once the guest holds all of the
  // previous sector's payload. Measured on C-12: its read pump asked for sector N+1 while the FIFO
  // still held N's EDC/ECC tail, read those bytes back, and its own sector-locator check reported
  // `CdRead: sector error` once per 60 fields.
  if (s->following_sector_ready && (s->data_n == 0 || s->data_rd >= sector_payload_bytes(s))) {
    s->loc_lba++;
    s->following_sector_ready = 0;
    if (!load_sector(s, s->loc_lba)) {
      stop_continuous_read(s);
      return;
    }
    // THE HANDOFF IS THE DRIVE'S CLOCK, not the request bit's. One buffered sector means the drive
    // reads again only once software has taken the announced one, and this request is that handoff
    // even when the request bit was already latched. Leaving the next sector unannounced until a
    // 0 -> 1 transition that stock libcd never performs stalls a long read after the first sector
    // it hands over on a repeated write (measured on C-12: a 121-sector read stopped at three).
    schedule_sector_event(s);
  } else if (fresh_request && s->data_n > 0 && !s->following_sector_ready) {
    schedule_sector_event(s);
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
    if (s->command_event_armed && s->command_phase <= 1) {
      st |= 0x80; // BUSYSTS while command receive/argument/execution is pending
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
      cdc_command_schedule(s, v); // command register
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
            ++s->irq_sequence;
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
