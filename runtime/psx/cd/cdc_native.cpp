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
#include "disc_toc.h"
#include "r3000.h"
#include "xa_state.h"
#include <algorithm>
#include <lucent/log.h>
#include <optional>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// What ended a continuous read. The two cases are not interchangeable, because only one of them has
// to tell the guest.
//
//   kGuestCommand — the guest issued Pause / Stop / Reset. Its OWN command responses already carry
//                   the news: INT3 answers with the status the read had, INT2 with the status after
//                   it, so the guest's next `CdSync` poll sees the read stop. A data-ready here
//                   would be a response hardware does not produce for a command.
//
//   kDriveStopped — the drive stopped BY ITSELF, because the sector it owed could not be read: end
//                   of media, or an LBA the image cannot supply. No command is involved, so nothing
//                   the guest polls will ever change, and the only notification that exists is the
//                   data-ready the controller raises when a read ends with a sector still owed. That
//                   is a hardware fact, not a convenience: the end-of-disc path in the vendor
//                   backend this model is aligned with moves the drive out of its reading state and
//                   then raises exactly that interrupt with the new status
//                   (vendor/beetle-psx/mednafen/psx/cdc.c:1558-1570, CDCIRQ_DATA_END). libcd's
//                   `CdlReadS` waits for precisely this status, so without it the guest's wait has
//                   no way to return — measured on CTR, where the XA stream ran to its end
//                   (EOF @ LBA 140038, 941 sectors) and the guest's `CdReadyCallback` never fired
//                   once, leaving its boot loop polling a completion that had never been posted.
namespace psx::cd {
namespace {

constexpr uint8_t kCdlStatStandby = 0x02;
constexpr uint8_t kCdlStatRead = 0x20;
// Interrupt-enable bits 0-4: the five response types (INT1..INT5 are encoded in bits 0-2).
constexpr uint8_t kCdcIrqEnableAll = 0x1F;
// INT5 error code for an argument the command cannot satisfy (GetTD of a track the disc lacks).
constexpr uint8_t kCdcErrorBadArgument = 0x10;

} // namespace

void CdController::reset() {
  Disc *const wiredDisc = disc;
  psx::audio::XaStream *const wiredXa = xa;
  *this = CdController{};
  stat = kCdlStatStandby; // power-on defaults
  // The interrupt-enable register as the BIOS leaves it: its CD initialisation enables all five
  // response types before any game code runs, and the framework replaces that BIOS. A game that
  // masks responses (to poll a command itself) writes its own value.
  irq_en = kCdcIrqEnableAll;
  disc = wiredDisc;
  xa = wiredXa;
  disc_read_raw_fn = [](Disc *d, uint32_t lba, uint8_t *out, uint32_t n) {
    return d->readRaw(lba, out, n);
  };
  disc_read_sector_fn = [](Disc *d, uint32_t lba, uint8_t *out) {
    return d->readSector(lba, out);
  };
  disc_get_subq_position_fn = [](Disc *d, uint32_t lba, uint8_t *out) {
    return d->subqPosition(lba, out);
  };
}

bool CdController::queueEmpty() const {
  return q_head == q_tail;
}

// The controller's interrupt LINE to the CPU: the current response's type ANDed with the
// interrupt-enable register (bank 1 of 0x1F801802), as on hardware and in Beetle's RecalcIRQ. A
// response whose type the guest masked off is still current and readable through the flag
// register, but it raises no CPU interrupt, so the guest's interrupt handler does not consume it.
// Spyro 3's disc check relies on exactly this: it masks responses with 0x18, issues GetTN directly,
// and polls the flag register for the answer while libcd's handler stays out of the way.
bool CdController::irqLine() const {
  return !queueEmpty() && (q[q_head].type & irq_en & kCdcIrqEnableAll) != 0;
}

// A response just became current (queued into an empty queue, or uncovered by an acknowledge). The
// sequence counts responses; the I_STAT edge follows the line, so a masked response raises none.
void CdController::responseBecameCurrent() {
  ++irq_sequence;
  if (irqLine()) {
    irq_edge = 1; // -> I_STAT bit 2, latched by the MMIO dispatcher
  }
}

void CdController::raise(uint8_t type, const uint8_t *resp, int len) {
  const bool becomes_current = q_tail == q_head;
  int n = (q_tail + 1) & 7;
  if (n == q_head) {
    return; // queue full (shouldn't happen)
  }
  q[q_tail].type = type;
  q[q_tail].len = len;
  if (len) {
    memcpy(q[q_tail].resp, resp, (size_t)len);
  }
  if (becomes_current) {
    resp_rd = 0; // first entry becomes active
  }
  q_tail = n;
  if (becomes_current) {
    responseBecameCurrent();
  }
}

// The results buffer the response register (bank 0 of 0x1F801801) reads. Acknowledging clears the
// interrupt flag only: the results stay readable until the next response becomes current and
// replaces them (Beetle's BeginResults). So with the queue empty the buffer still belongs to the
// response acknowledged last, which sits in the slot just behind the head, with its read position
// kept. Spyro 3's disc check acknowledges GetTN/GetTD before it reads their answer.
const CdcIrqEnt *CdController::resultsEntry() const {
  return &q[queueEmpty() ? ((q_head - 1) & 7) : q_head];
}

uint8_t CdController::currentIrqType() const {
  return queueEmpty() ? 0u : q[q_head].type;
}

uint32_t CdController::commandArgsLba() const {
  // Setloc params: amm,ass,asect (BCD), minus the 2s lead-in.
  const uint8_t *p = command_args;
  int mm = (p[0] >> 4) * 10 + (p[0] & 0xF);
  int ss = (p[1] >> 4) * 10 + (p[1] & 0xF);
  int ff = (p[2] >> 4) * 10 + (p[2] & 0xF);
  return (uint32_t)((mm * 60 + ss) * 75 + ff - 150);
}

uint64_t CdController::seekTime(uint32_t target_lba) const {
  // Beetle's PS_CDC_CalcSeekTime with only its random 0..25000 component fixed at zero.
  const bool motor_on = (stat & kCdlStatStandby) != 0;
  const bool paused = motor_on && !reading;
  // LBAs are signed: Setloc may name the 2-second lead-in before LBA 0 (00:00:00 is LBA -150), which
  // the unsigned fields carry in two's complement. The seek distance is between the signed values.
  const int64_t initial_lba = motor_on ? static_cast<int32_t>(loc_lba) : 0;
  const int64_t signed_target_lba = static_cast<int32_t>(target_lba);
  const uint64_t difference = static_cast<uint64_t>(initial_lba > signed_target_lba ? initial_lba - signed_target_lba
                                                                                    : signed_target_lba - initial_lba);
  uint64_t ticks = motor_on ? 0u : 33'868'800u;
  const uint64_t seek_ticks = difference * 33'868'800u / (uint64_t{72} * 60u * 75u);
  ticks += seek_ticks < 20'000u ? 20'000u : seek_ticks;
  if (difference >= 2'250u) {
    ticks += 33'868'800u * 300u / 1'000u;
  } else if (paused) {
    ticks += (mode & 0x80) != 0 ? 1'237'952u : 1'237'952u * 2u;
  } else if (difference >= 3u && difference < 12u) {
    ticks += driveSectorPeriodCpuTicks(mode) * 4u;
  }
  return ticks;
}

namespace {

struct ArgumentCounts {
  uint8_t min = 0;
  uint8_t max = 0;
};

// The parameter counts a command accepts; none for an unknown command.
std::optional<ArgumentCounts> commandArgumentCounts(uint8_t command) {
  switch (command) {
  case 0x02:
    return ArgumentCounts{3, 3};
  case 0x03:
    return ArgumentCounts{0, 1};
  case 0x0D:
  case 0x1D:
    return ArgumentCounts{2, 2};
  case 0x0E:
  case 0x12:
  case 0x14:
  case 0x19:
    return ArgumentCounts{1, 1};
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
    return ArgumentCounts{0, 0};
  default:
    return std::nullopt;
  }
}

} // namespace

// Beetle-parity XA routing (vendor cdc.c DS_READING arm): with Setmode's STRSND bit (0x40) set, a
// Mode2 sector whose submode has ALL of RT|form2|audio (0x64) is XA. MODE_SF (0x08) selects exactly
// one file/channel for the SPU; an XA sector the filter rejects is dropped, never offered as data.
CdcSectorRoute CdController::sectorRoute(const uint8_t *raw) const {
  if ((mode & 0x40) == 0 || raw[15] != 2 || (raw[18] & 0x64) != 0x64) {
    return CdcSectorRoute::Data;
  }
  const bool selected = (mode & 0x08) == 0 || (raw[16] == filter_file && raw[17] == filter_chan);
  return selected ? CdcSectorRoute::XaAudio : CdcSectorRoute::XaDropped;
}

// Hand one audio sector to the SPU ring. First routing also flips the ring into PUSH mode: from
// then on the SPU pull never fetches sectors itself — the drive owns the cursor (a pull-side
// self-fetch would read ahead of the physical head and desync A/V). The sector is ALWAYS consumed:
// on hardware the decoded audio lands in the SPU's own CD buffer, and an overflowing SPU buffer
// drops samples while the drive keeps spinning, so there is no condition under which this drive
// stops for audio. Returns the frames decoded (0 when the sector carried none).
int CdController::routeAudioToSpu(const uint8_t *raw, uint32_t drive_lba) {
  xa->push_mode = 1;
  return xa->pushAudioSector(raw, drive_lba);
}

// One drive-paced sector consumption during a continuous read, at `lba`:
//   XA audio under STRSND -> decode into the SPU ring (out_audio=true; NO FIFO fill, NO INT1 —
//                           the guest never sees audio sectors in its DMA stream, exactly like
//                           hardware);
//   anything else         -> fill the data FIFO via load_sector (the caller raises INT1).
// Returns false only when the disc read itself failed. Audio is always consumed and always lets the
// head advance: an audio sector has no "retry" outcome, and the caller has no hold path to share
// with the data side.
bool CdController::consumeSector(uint32_t lba, bool *out_audio) {
  *out_audio = false;
  if (!(mode & 0x40)) { // STRSND off: every sector is a data sector
    return loadSector(lba);
  }
  uint8_t raw[2352];
  if (!disc_read_raw_fn(disc, lba, raw, sizeof(raw))) {
    return false;
  }
  const CdcSectorRoute route = sectorRoute(raw);
  if (route == CdcSectorRoute::Data) {
    return loadSector(lba);
  }
  if (route == CdcSectorRoute::XaAudio) {
    routeAudioToSpu(raw, lba);
  }
  *out_audio = true;
  return true;
}

bool CdController::loadSector(uint32_t lba) { // fill the data FIFO with the sector at lba
  // WHAT THE FIFO CONTAINS DEPENDS ON Setmode's bit 0x20 ("whole sector"), and getting this wrong is
  // invisible until much later. Streaming code reads the first 8 words of each sector expecting the
  // 4-byte header plus 8-byte subheader — that is how it tells a video sector from an audio one.
  // Serving user data there hands it 32 bytes of picture data as a subheader, it recognises nothing,
  // and it re-requests the same sector forever.
  //   bit 0x20 set  -> raw sector from offset 12 (past sync): header, subheader, then data
  //   bit 0x20 clear-> 2048 bytes of user data only
  if (mode & 0x20) {
    uint8_t raw[2352];
    if (!disc_read_raw_fn(disc, lba, raw, sizeof raw)) {
      lucent::error("cdc", "ReadN: no data for LBA {} — data FIFO left EMPTY", lba);
      data_n = 0;
      data_rd = 0;
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
    memcpy(data, raw + 12, 2340);
    data_n = 2340;
    data_rd = 0;
    return true;
  }
  uint8_t sec[2048];
  if (!disc_read_sector_fn(disc, lba, sec)) {
    lucent::error("cdc", "ReadN: no data for LBA {} (no disc, or out of range) — data FIFO left EMPTY", lba);
    data_n = 0;
    data_rd = 0;
    return false;
  }
  memcpy(data, sec, 2048);
  data_n = 2048;
  data_rd = 0;
  return true;
}

void CdController::queueDataReady() {
  const uint8_t response[1] = {stat};
  raise(1, response, 1);
}

bool CdController::postDataReady() {
  if (((q_tail + 1) & 7) == q_head) {
    return 0; // raise() would drop it; say so instead of reporting a completion that was never queued
  }
  queueDataReady();
  return 1;
}

bool CdController::postDataReadyAfterRead(uint32_t first_lba, uint32_t sectors, uint8_t mode) {
  if (sectors == 0 || read_completion_n >= 4) {
    return 0;
  }
  if (!tick_now) {
    return postDataReady();
  }
  const uint64_t now_ticks = tick_now(tick_context);
  const uint64_t start = read_completion_n ? read_completion_deadline_ticks[read_completion_n - 1] : now_ticks;
  read_completion_deadline_ticks[read_completion_n++] = (start > now_ticks ? start : now_ticks) + seekTime(first_lba) +
                                                        uint64_t{sectors} * driveSectorPeriodCpuTicks(mode);
  loc_lba = first_lba + sectors;
  stat |= kCdlStatStandby; // the spindle is up once a read has been served
  return 1;
}

// Announce every owed read completion whose drive time has elapsed, oldest first. A full response queue
// leaves the rest owed for the next service rather than dropping one.
bool CdController::serviceReadCompletions() {
  if (read_completion_n == 0 || !tick_now) {
    return 0;
  }
  const uint64_t now_ticks = tick_now(tick_context);
  int announced = 0;
  while (read_completion_n > 0 && now_ticks >= read_completion_deadline_ticks[0] && postDataReady()) {
    lucent::debug("cdcpace", "announced owed read completion now={} queue_empty={}", now_ticks, queueEmpty());
    read_completion_n--;
    for (int i = 0; i < read_completion_n; i++) {
      read_completion_deadline_ticks[i] = read_completion_deadline_ticks[i + 1];
    }
    announced = 1;
  }
  return announced;
}

void CdController::cancelDriveEvent() {
  if (!drive_event_armed) {
    return;
  }
  drive_event_armed = 0;
  drive_deadline_ticks = 0;
}

// The next sector is due one drive period after `origin`.
void CdController::scheduleSectorEventAfter(uint64_t origin) {
  if (!reading || following_sector_ready || drive_event_armed || !tick_now) {
    return;
  }
  drive_deadline_ticks = origin + driveSectorPeriodCpuTicks(mode);
  drive_event_armed = 1;
  lucent::debug("cdcpace",
                "armed sector event origin={} deadline={} mode=0x{:02X} first={}",
                origin,
                drive_deadline_ticks,
                mode,
                first_sector_pending);
}

void CdController::scheduleSectorEvent() {
  if (!tick_now) {
    return;
  }
  scheduleSectorEventAfter(tick_now(tick_context));
}

// See `CdcReadStop` above for why the two callers are not the same event. The drive-stopped case
// raises its end-of-read completion through `queue_data_ready` — the ONE response path a data
// sector uses — so the guest's interrupt handler sees the ordinary data-ready shape with the status
// byte it needs, carrying a cleared CdlStatRead because the drive has left its reading state.
bool CdController::stopContinuousRead(ReadStop stop) {
  cancelDriveEvent();
  reading = 0;
  stat &= static_cast<uint8_t>(~kCdlStatRead); // clears when the drive leaves DS_READING
  first_sector_pending = 0;
  if (stop == ReadStop::GuestCommand) {
    // A sector already announced by INT1 stays in the drive buffer; the guest's BFRD still gets it.
    return 0;
  }
  following_sector_ready = 0;
  if (postDataReady() == 0) {
    lucent::error("cdc",
                  "the drive stopped at LBA {} with no room in the controller response queue for the "
                  "end-of-read completion — the guest will not learn that this read ended",
                  loc_lba);
    return 0;
  }
  // POSTING A COMPLETION AND RAISING THE INTERRUPT ARE ONE EVENT, so this returns 1 and the drive
  // service passes it up. Queueing a response does not by itself make the CPU look: the CD line in
  // I_STAT is raised by `Timing::serviceCdc`'s `irqStatLatch()`, which it only performs when the
  // drive service reports that something happened. A completion queued on a path that reported
  // nothing therefore sat in the controller until some unrelated later poll happened to latch the
  // line — measured as the end-of-read arriving one poll late and only by luck, and for a guest
  // that polls on vblank that is a completion delivered against the wrong frame.
  return 1;
}

void CdController::startContinuousRead() {
  cancelDriveEvent();
  reading = 1;
  stat |= kCdlStatRead;
  first_sector_pending = 1;
  bfrd = 0;
  following_sector_ready = 0;
  data_n = 0;
  data_rd = 0;
  scheduleSectorEvent();
}

void CdController::bindTickSource(void *context, CdcTickNowFn now) {
  tick_context = context;
  tick_now = now;
}

// One sector whose deadline has passed. An audio sector re-arms from its own deadline, so a service
// that arrives several periods late still passes every sector the head reached in that time.
bool CdController::serviceDueSector(uint64_t now_ticks) {
  const uint64_t due_ticks = drive_deadline_ticks;
  drive_event_armed = 0;
  drive_deadline_ticks = 0;
  lucent::debug("cdcpace", "servicing sector event now={} first={} lba={}", now_ticks, first_sector_pending, loc_lba);
  if (!reading || following_sector_ready) {
    return 0;
  }

  if (first_sector_pending) {
    first_sector_pending = 0;
    bool audio = false;
    if (!consumeSector(loc_lba, &audio)) {
      return stopContinuousRead(ReadStop::DriveStopped);
    }
    // Set BEFORE `queue_data_ready` below, and that ordering is load-bearing rather than incidental:
    // `queue_data_ready` publishes `stat` as the status byte the guest reads, so setting the bit
    // after the announcement would tell a guest the read had stopped at the moment it was handed its
    // first sector. `test_the_first_data_ready_of_a_live_read_carries_the_read_bit` pins it.
    stat |= kCdlStatRead; // INT1/Getstat must identify an active sector read
    if (audio) {
      // Decoded straight to the SPU: the guest sees no data-ready. The drive keeps reading.
      scheduleSectorEventAfter(due_ticks);
      lucent::debug("cdcpace", "first sector @ {} was XA audio -> SPU ring", loc_lba);
      return 0;
    }
    queueDataReady();
    return 1;
  }

  // One drive-side buffer, one INT1. Software may still own unread bytes from the prior sector;
  // the later BFRD service request swaps this ready sector into the CPU/DMA-visible FIFO.
  // An XA-audio next sector never announces: the drive decodes it now and moves on, so a pure-
  // audio stretch produces no guest interrupts at all while the head still advances in real time.
  //
  // The peek exists only to decide audio-vs-data, so it is read at all only when Setmode's STRSND
  // bit could select audio: `sectorRoute` answers "data" for every sector without
  // it, and reading 2352 bytes per sector to learn that is pure cost on every non-XA stream.
  //
  // A peek that FAILS is the third place the drive can stop, and it is the same event as the two
  // `load_sector` failures above: this sector cannot be read, so the drive has nothing to present
  // as audio and nothing to present as data either. Announcing a data sector it cannot load is the
  // one answer that is always wrong — the data FIFO would be empty for it, so DRQSTS would stay
  // clear while the interrupt claims a sector — and it cost the guest an extra interrupt before
  // the end-of-read completion it actually needed. So the read ends here, with that completion.
  if (mode & 0x40) {
    uint8_t raw[2352];
    const uint32_t next_lba = loc_lba + 1;
    if (!disc_read_raw_fn(disc, next_lba, raw, sizeof raw)) {
      return stopContinuousRead(ReadStop::DriveStopped);
    }
    const CdcSectorRoute route = sectorRoute(raw);
    if (route != CdcSectorRoute::Data) {
      if (route == CdcSectorRoute::XaAudio) {
        routeAudioToSpu(raw, next_lba);
      }
      loc_lba++; // the head PASSED this sector; the next peek must see the one after it
      scheduleSectorEventAfter(due_ticks);
      return 0;
    }
  }
  following_sector_ready = 1;
  queueDataReady();
  return 1;
}

bool CdController::serviceDriveEvent() {
  if (!drive_event_armed || !tick_now) {
    return 0;
  }
  const uint64_t now_ticks = tick_now(tick_context);
  while (drive_event_armed && now_ticks >= drive_deadline_ticks) {
    if (serviceDueSector(now_ticks) != 0) {
      return 1;
    }
  }
  return 0;
}

// Draining the data FIFO releases that buffer; it does not drive the disc head or produce an event.
// If a following sector was announced, the next BFRD service request installs it. Clearing BFRD
// here models the hardware FIFO becoming serviceable again even when software leaves the request
// bit high.
void CdController::sectorConsumed() {
  data_n = 0;
  data_rd = 0;
  bfrd = 0;
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
void CdController::setMode(uint8_t value) {
  // Print the WHOLE byte, not just the bits this model currently acts on. Bit 0x40 (XA-ADPCM
  // enable) and bit 0x08 (XA filter) decide whether the drive is supposed to swallow audio
  // sectors into the ADPCM decoder instead of presenting them as data; a mode log that showed
  // only 0x20 would make an unimplemented audio path look like a mode the game never asked for.
  lucent::debug("cdc",
                "setmode 0x{:02X} (speed={} xa-adpcm={} filter={} whole-sector={})",
                value,
                (value & 0x80) ? 2 : 1,
                (value & 0x40) ? 1 : 0,
                (value & 0x08) ? 1 : 0,
                (value & 0x20) ? 1 : 0);
  mode = value;
  if (drive_event_armed) {
    cancelDriveEvent();
    scheduleSectorEvent();
  }
}

void CdController::setFilter(uint8_t file, uint8_t channel) {
  filter_file = file;
  filter_chan = channel;
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
// on the guest clock, and the very next CdControl(ReadN) applies `beginRead` right away — so a
// Pause issued BEFORE the read executed AFTER the read had begun and cancelled that read's own
// sector event. Measured on C-12: no completion ever arrived and the title retried one sector every
// 60 fields.
void CdController::retirePendingCommand() {
  if (!command_event_armed || !tick_now) {
    return;
  }
  for (int guard = 0; guard < 64 && command_event_armed; ++guard) {
    command_deadline_ticks = tick_now(tick_context);
    const CdcCommandEvent event = serviceCommand(1);
    if (event == CdcCommandEvent::Execute) {
      finishCommandExecution(execCommand(pending_command));
    } else if (event == CdcCommandEvent::Complete) {
      completeCommand();
    }
    // `kNone` only means this phase was not the last one; the loop keeps pulling the deadline to
    // now until the command has executed (and its completion disarms it).
  }
}

void CdController::beginRead(uint32_t lba) {
  // The read is the NEXT command after anything still pending, so the older one runs first and
  // retires here: a timed command is never executed inline at issue, so it always lands before the
  // read it was issued before.
  retirePendingCommand();
  loc_lba = lba;
  command_lba = lba;
  startContinuousRead();
}

void CdController::issueCommand(uint8_t command) {
  index = 0;
  // The responses this command produces belong to the GUEST that issued it: stock libcd polls
  // its own status and consumes them after every command (measured on C-12: Setloc -> Setmode ->
  // ReadN -> Pause, each followed by a GetStat poll), so the INT3 from execution and the INT2 from
  // completion both have a real consumer. An earlier owner deleted them here on the premise that
  // a framework-issued command has "no guest consumer", which is false for every command that
  // reaches this function: the guest issued it through its own CdControl. Deleting them left the
  // guest's post-Pause poll waiting for a completion that had already been thrown away.
  scheduleCommand(command);
  // TIMED, not inline. A controller takes its own time to accept a command and reports it in two
  // phases; `test_cdc_continuous_read` pins that a native-issued Pause observes the still-reading
  // status at execution and the paused status at completion. Ordering a framework-issued command
  // against the guest's NEXT one is the queue seam's job (`retire_pending_command`), not this
  // function's.
}

int CdController::takeCurrentResponse(uint8_t *bytes, int capacity) {
  const int savedBank = index;
  int taken = 0;
  while ((read(CdcRegister::Index) & kCdcStatResponseReady) != 0u) {
    const uint8_t value = (uint8_t)read(CdcRegister::Response);
    if (bytes && taken < capacity) {
      bytes[taken] = value;
    }
    taken++;
  }
  write(CdcRegister::Index, (uint8_t)savedBank);
  return taken;
}

int CdController::dmaRead(uint32_t *out, int words) {
  if (words <= 0) {
    return 0;
  }
  memset(out, 0, (size_t)words * sizeof *out);
  if (!bfrd) {
    return 0;
  }
  int got = 0;
  while (got < words && data_rd + 4 <= data_n) {
    const uint8_t *p = data + data_rd;
    out[got++] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    data_rd += 4;
  }
  if (got && data_rd >= data_n) {
    sectorConsumed();
  }
  return got;
}

uint64_t CdController::execCommand(uint8_t cmd) {
  lucent::debug("cdc",
                "cmd 0x{:02X} args={} [{:02X} {:02X} {:02X}]",
                cmd,
                command_arg_n,
                command_args[0],
                command_args[1],
                command_args[2]);
  const std::optional<ArgumentCounts> counts = commandArgumentCounts(cmd);
  if (!counts || command_arg_n < counts->min || command_arg_n > counts->max) {
    const uint8_t error[2] = {static_cast<uint8_t>(stat | 0x01), 0x20};
    raise(5, error, 2);
    return 0;
  }
  uint8_t r1[1] = {stat};
  switch (cmd) {
  case 0x01:
    raise(3, r1, 1);
    return 0; // Getstat
  case 0x02:
    command_lba = commandArgsLba();
    raise(3, r1, 1);
    return 0; // Setloc
  case 0x0E:
    setMode(command_args[0]);
    raise(3, r1, 1);
    return 0; // Setmode
  case 0x0D:
    setFilter(command_args[0], command_args[1]);
    raise(3, r1, 1);
    return 0;  // Setfilter
  case 0x11: { // GetlocP: current Sub-Q track/index and relative/absolute position.
    uint8_t position[8] = {};
    if (!getlocP(position)) {
      const uint8_t error[2] = {static_cast<uint8_t>(stat | 0x01), 0x80};
      raise(5, error, 2);
      return 0;
    }
    raise(3, position, 8);
    return 0;
  }
  case 0x06:
  case 0x1B: // ReadN / ReadS
    raise(3, r1, 1);
    loc_lba = command_lba;
    startContinuousRead();
    return 0;
  case 0x09: {
    const uint8_t reading_status[1] = {stat};
    const bool was_reading = reading != 0;
    stopContinuousRead(ReadStop::GuestCommand);
    raise(3, reading_status, 1);
    if (!was_reading) {
      return 5'000;
    }
    const uint64_t speed_scale = (mode & 0x80) != 0 ? 1u : 2u;
    return (1'124'584u + static_cast<uint64_t>(loc_lba) * 42'596u / (uint64_t{75} * 60u)) * speed_scale;
  }
  case 0x08: {
    const uint8_t reading_status[1] = {stat};
    const bool was_stopped = !reading && (stat & kCdlStatStandby) == 0;
    stopContinuousRead(ReadStop::GuestCommand);
    stat = 0;
    raise(3, reading_status, 1);
    return was_stopped ? 5'000u : 33'868u;
  }
  case 0x0A:
    stopContinuousRead(ReadStop::GuestCommand);
    stat = kCdlStatStandby;
    mode = 0;
    command_lba = 0;
    raise(3, r1, 1);
    return 4'100'000u; // Reset's deterministic seek-to-sector-zero completion.
  case 0x0B:
  case 0x0C:
    raise(3, r1, 1);
    return 0; // Mute / Demute
  case 0x15:
  case 0x16: // SeekL / SeekP
    raise(3, r1, 1);
    return seekTime(command_lba);
  case 0x1E: // ReadTOC
    raise(3, r1, 1);
    return 30'000'000u + seekTime(0);
  case 0x07:
    if (stat != 0) {
      const uint8_t error[2] = {static_cast<uint8_t>(stat | 0x01), 0x20};
      raise(5, error, 2);
      return 0;
    }
    stat = kCdlStatStandby;
    raise(3, r1, 1);
    return 3'386'880u;
  case 0x03:
  case 0x17:
  case 0x18: // MotorOn/SetFilter/Play/SetSession
    raise(3, r1, 1);
    return 0;
  case 0x19: { // Test (sub-function in param[0])
    if (command_args[0] == 0x20) {
      uint8_t v[4] = {0x94, 0x09, 0x19, 0xC0}; // BIOS date/version
      raise(3, v, 4);
    } else {
      raise(3, r1, 1);
    }
    return 0;
  }
  case 0x1A: { // GetID -> region/license
    raise(3, r1, 1);
    return 33'868u;
  }
  case 0x12:
    raise(3, r1, 1);
    return 33'868u;
  case 0x13:   // GetTN
  case 0x14: { // GetTD
    psx::disc_toc::BcdPair answer{};
    const bool known = disc != nullptr && (cmd == 0x13 ? psx::disc_toc::track_range(*disc, answer)
                                                       : psx::disc_toc::track_start(*disc, command_args[0], answer));
    if (!known) {
      const uint8_t error[2] = {static_cast<uint8_t>(stat | 0x01), kCdcErrorBadArgument};
      raise(5, error, 2);
      return 0;
    }
    const uint8_t t[3] = {stat, answer.first, answer.second};
    raise(3, t, 3);
    return 0;
  }
  default:
    lucent::debug("cdc", "UNHANDLED cmd 0x{:02X} -> ack only", cmd);
    raise(3, r1, 1);
    return 0;
  }
}

void CdController::completeCommand() {
  if (pending_command == 0x1A) {
    const uint8_t id[8] = {0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A'};
    raise(2, id, 8);
    return;
  }
  if (pending_command == 0x0A) {
    loc_lba = 0;
  } else if (pending_command == 0x15 || pending_command == 0x16) {
    loc_lba = command_lba;
    stat = kCdlStatStandby;
  }
  const uint8_t response[1] = {stat};
  raise(2, response, 1);
}

bool CdController::getlocP(uint8_t out[8]) const {
  return disc_get_subq_position_fn != nullptr && disc_get_subq_position_fn(disc, loc_lba, out);
}

bool CdController::nextDeadlineTicks(uint64_t *absoluteTicks) const {
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
  consider(drive_event_armed != 0, drive_deadline_ticks);
  consider(command_event_armed != 0, command_deadline_ticks);
  consider(read_completion_n != 0, read_completion_deadline_ticks[0]);
  if (!armed) {
    return 0;
  }
  *absoluteTicks = earliest;
  return 1;
}

bool CdController::service() {
  // The oracle services its drive counter before its command counter when both expire in the same
  // update. Preserve that ordering so an exact tie makes INT1 current and queues command INT3.
  const uint64_t irq_sequence_before = irq_sequence;
  serviceDriveEvent();
  serviceReadCompletions();
  const CdcCommandEvent event = serviceCommand(queueEmpty());
  if (event == CdcCommandEvent::Execute) {
    finishCommandExecution(execCommand(pending_command));
  } else if (event == CdcCommandEvent::Complete) {
    completeCommand();
  }
  return irq_sequence != irq_sequence_before;
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
std::uint32_t CdController::sectorPayloadBytes() const {
  return (mode & 0x20u) != 0 ? 12u + 2048u : 2048u;
}

void CdController::writeRequestRegister(uint8_t value) {
  const uint8_t asserted = value & 0x80;
  if (!asserted) {
    bfrd = 0;
    // As Beetle: a clear without bit 6 empties the data FIFO but keeps the sector buffer for a re-request.
    if ((value & 0x40) == 0) {
      data_rd = data_n;
    }
    return;
  }
  const bool flushed = data_n > 0 && data_rd >= data_n;

  // An asserted-to-asserted write leaves the active FIFO and its cursor alone: a consumer that
  // splits one sector transfer around such a write is still reading THAT sector (psxport issue
  // 0002). What a repeated write does carry is the request for the sector the drive has since
  // ANNOUNCED — stock libcd writes the bit once per sector and never clears it in between, so a
  // write that only acted on a 0 -> 1 transition left every sector after the first unanswered.
  const bool fresh_request = bfrd == 0;
  bfrd = 1;
  if (!reading && !following_sector_ready) {
    if (flushed) {
      data_rd = 0;
    }
    return;
  }

  // The announced sector becomes the one a request presents once the guest holds all of the
  // previous sector's payload. Measured on C-12: its read pump asked for sector N+1 while the FIFO
  // still held N's EDC/ECC tail, read those bytes back, and its own sector-locator check reported
  // `CdRead: sector error` once per 60 fields.
  if (following_sector_ready && (data_n == 0 || data_rd >= sectorPayloadBytes())) {
    loc_lba++;
    following_sector_ready = 0;
    if (!loadSector(loc_lba)) {
      stopContinuousRead(ReadStop::DriveStopped);
      return;
    }
    // THE HANDOFF IS THE DRIVE'S CLOCK, not the request bit's. One buffered sector means the drive
    // reads again only once software has taken the announced one, and this request is that handoff
    // even when the request bit was already latched. Leaving the next sector unannounced until a
    // 0 -> 1 transition that stock libcd never performs stalls a long read after the first sector
    // it hands over on a repeated write (measured on C-12: a 121-sector read stopped at three).
    scheduleSectorEvent();
  } else {
    if (flushed) {
      data_rd = 0;
    }
    if (fresh_request && data_n > 0 && !following_sector_ready) {
      scheduleSectorEvent();
    }
  }
}

uint32_t CdController::read(CdcRegister reg) {
  switch (reg) {
  case CdcRegister::Index: { // status register
    uint8_t st = (uint8_t)index;
    if (param_n == 0) {
      st |= 0x08; // PRMEMPT (param FIFO empty)
    }
    if (param_n < 16) {
      st |= 0x10; // PRMWRDY (param FIFO not full)
    }
    if (resp_rd < resultsEntry()->len) {
      st |= 0x20; // RSLRRDY (response ready)
    }
    if (bfrd && data_rd < data_n) {
      st |= 0x40; // DRQSTS (data FIFO not empty)
    }
    if (command_event_armed && command_phase <= 1) {
      st |= 0x80; // BUSYSTS while command receive/argument/execution is pending
    }
    return st;
  }
  case CdcRegister::Response: { // response FIFO
    const CdcIrqEnt *f = resultsEntry();
    return resp_rd < f->len ? f->resp[resp_rd++] : 0;
  }
  case CdcRegister::Parameter: { // data FIFO — CPU pop path; must advance the head exactly as DMA3 does
    if (!bfrd || data_rd >= data_n) {
      return 0;
    }
    const uint8_t b = data[data_rd++];
    if (data_rd >= data_n) {
      sectorConsumed();
    }
    return b;
  }
  default: // 3: bank0/2 interrupt enable; bank1/3 interrupt flag (low 3 bits = pending type)
    if (index & 1) {
      return queueEmpty() ? 0xE0 : (uint8_t)(0xE0 | q[q_head].type);
    }
    return irq_en | 0xE0;
  }
}

void CdController::write(CdcRegister reg, uint8_t v) {
  switch (reg) {
  case CdcRegister::Index:
    index = v & 3;
    return; // index/bank select
  case CdcRegister::Response:
    if (index == 0) {
      scheduleCommand(v); // command register
    }
    return;
  case CdcRegister::Parameter:
    if (index == 0) {
      if (param_n < 16) {
        param[param_n++] = v;
      }
    } // param FIFO push
    else if (index == 1) {
      // Interrupt enable. Unmasking the type of a response that is already current asserts the line
      // now, which is a fresh edge exactly as a newly queued response would be.
      const bool line_was_asserted = irqLine();
      irq_en = v;
      if (!line_was_asserted && irqLine()) {
        irq_edge = 1;
      }
    }
    return;
  default:            // 3
    if (index == 1) { // interrupt flag: write 1s to ack/clear
      if (v & 0x07) { // ack current IRQ -> advance the queue
        if (!queueEmpty()) {
          q_head = (q_head + 1) & 7;
          // A response that was QUEUED behind the one just acked becomes current now, and that is
          // a fresh interrupt on real hardware — not a continuation of the acked one. Without this
          // edge the second and later responses of a multi-INT command sequence would be visible
          // in the FIFO but never announced, which looks exactly like a dropped response.
          if (!queueEmpty()) {
            resp_rd = 0; // its results replace the acknowledged ones
            responseBecameCurrent();
          }
        }
      }
      if (v & 0x40) {
        param_n = 0; // reset param FIFO
      }
    } else if (index == 0) { // request register
      writeRequestRegister(v);
    }
    return;
  }
}

} // namespace psx::cd
